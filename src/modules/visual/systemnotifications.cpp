#include "systemnotifications.hpp"

#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/BlockSource.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/DimensionIdentity.hpp>
#include <worldanalysis/sdk/world/Level.hpp>
#include <worldanalysis/sdk/world/Weather.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Vec2 = worldanalysis::sdk::Vec2;
using Vec3 = worldanalysis::sdk::Vec3;
using Clock = std::chrono::steady_clock;
using Segment = worldanalysis::worldoverlay::Segment;
using Quad = worldanalysis::worldoverlay::Quad;
using DimensionKind = worldanalysis::sdk::dimension_identity::Kind;

struct BlockPosRaw { int x = 0, y = 0, z = 0; };
using GetBlockFn = void* (*)(void*, const BlockPosRaw&);
using ActorGetHealthFn = int (*)(void*);
using AttributesGetByNameFn = void* (*)(void*, const std::string*);

enum class NotificationKind : std::uint8_t {
    Weather,
    Time,
    LevelUp,
    LevelDown,
    SlimeChunk,
    SleepDeprived,
    HpLow,
    HpRecovered,
    Dimension,
    DayRollover,
    HungerLow,
    SaturationEmpty,
    ModuleState
};

struct Notification {
    Vec3 anchor{};
    std::string title;
    std::string body;
    std::vector<float> samples;
    NotificationKind kind = NotificationKind::Weather;
    int lane = 0;
    Clock::time_point created{};
    Clock::time_point expires{};
};

struct GridSnapshot {
    bool visible = false;
    int chunkX = 0;
    int chunkZ = 0;
    int dimensionId = 0;
    void* region = nullptr;
    float opacity = 0.0f;
    float modeBlend = 0.0f;
    Clock::time_point enteredAt{};
    Clock::time_point expiresAt{};
    std::array<float, 64> surfaceY{};
    std::array<bool, 64> surfaceReady{};
    std::size_t nextSurfaceSample = 0;
};

constexpr float kPi = 3.14159265358979323846f;
constexpr std::size_t kMaxNotifications = 8;
constexpr std::uint32_t kAttributesHash = 0xFD3B0613u;
constexpr std::size_t kAttributesStride = 0x58;
constexpr std::int64_t kInsomniaThresholdTicks = 72000;
constexpr float kHungerLowThreshold = 6.0f;
constexpr float kHungerResetThreshold = 8.0f;
constexpr float kSaturationEmptyThreshold = .05f;
constexpr float kSaturationResetThreshold = 1.0f;
constexpr int kOverworldMinY = -64;
constexpr int kOverworldMaxY = 319;
constexpr int kSurfaceSamplesPerTick = 2;
constexpr int kDimensionStableTicksRequired = 4;

SystemNotificationsModule* g_mod = nullptr;
GetBlockFn g_getBlock = nullptr;
ActorGetHealthFn g_getHealth = nullptr;
AttributesGetByNameFn g_getAttributeByName = nullptr;
std::vector<Notification> g_notifications;
std::mutex g_mutex;
Vec3 g_eyePosition{};
Vec2 g_playerRotation{};
bool g_playerReady = false;
int g_weatherState = -1;
float g_weatherIntensity = 0.0f;
int g_timePhase = -1;
int g_lastLevel = -1;
int g_lastHealth = std::numeric_limits<int>::min();
bool g_criticalHealthSeen = false;
int g_lastChunkX = 0;
int g_lastChunkZ = 0;
int g_lastChunkDimension = std::numeric_limits<int>::min();
bool g_chunkReady = false;
std::int64_t g_awakeTicks = 0;
int g_lastRawTime = std::numeric_limits<int>::min();
bool g_sleepAlerted = false;
std::int64_t g_lastDay = std::numeric_limits<std::int64_t>::min();
void* g_dimensionPtr = nullptr;
DimensionKind g_dimensionKind = DimensionKind::Unknown;
DimensionKind g_previousDimensionKind = DimensionKind::Unknown;
bool g_dimensionNoticePending = false;
int g_dimensionStableTicks = 0;
float g_lastHunger = std::numeric_limits<float>::quiet_NaN();
float g_lastSaturation = std::numeric_limits<float>::quiet_NaN();
bool g_hungerAlerted = false;
bool g_saturationAlerted = false;
bool g_gridWasEnabled = false;
bool g_poweredOnPending = false;

// The grid state is deliberately independent of the transient notification
// cards so the surface/lattice can smoothly fade and resize while the player
// remains inside the slime chunk.
GridSnapshot g_grid{};

bool plausiblePtr(std::uintptr_t p, std::size_t alignment = alignof(void*)) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    constexpr std::uintptr_t mask = 0x00FFFFFFFFFFFFFFULL;
    constexpr std::uintptr_t max = 0x0010000000000000ULL;
    const auto address = p & mask;
    return address >= 0x10000 && address < max
        && (alignment <= 1 || (address & (alignment - 1)) == 0);
#else
    return p >= 0x10000 && (alignment <= 1 || (p & (alignment - 1)) == 0);
#endif
}

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 3e7f && std::abs(p.z) < 3e7f
        && p.y > -1024.0f && p.y < 4096.0f;
}
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 normalize(const Vec3& v, const Vec3& fallback = {1, 0, 0}) {
    const float l = length(v);
    return l > .0001f ? mul(v, 1.0f / l) : fallback;
}
float smoothStep(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}
std::uint32_t white(float alpha) {
    return (static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + .5f) << 24)
        | 0x00FFFFFFu;
}
std::uint32_t grey(float alpha, std::uint8_t value) {
    return (static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + .5f) << 24)
        | (static_cast<std::uint32_t>(value) << 16)
        | (static_cast<std::uint32_t>(value) << 8)
        | value;
}

void* runtimeComponent(void* rawActor, std::uint32_t typeHash, std::size_t stride) {
    if (!rawActor || !stride) return nullptr;
    constexpr std::uint64_t noNode = std::numeric_limits<std::uint64_t>::max();
    const auto actor = reinterpret_cast<std::uintptr_t>(rawActor);
    const auto registry = *reinterpret_cast<const std::uintptr_t*>(actor + 0x10);
    const std::uint32_t entity = *reinterpret_cast<const std::uint32_t*>(actor + 0x18);
    if (!plausiblePtr(registry)) return nullptr;
    const auto bb = *reinterpret_cast<const std::uintptr_t*>(registry + 0x38);
    const auto be = *reinterpret_cast<const std::uintptr_t*>(registry + 0x40);
    const auto nodes = *reinterpret_cast<const std::uintptr_t*>(registry + 0x50);
    const auto sentinel = *reinterpret_cast<const std::uintptr_t*>(registry + 0x58);
    if (!plausiblePtr(bb) || !plausiblePtr(be) || be <= bb || !plausiblePtr(nodes)) return nullptr;
    const std::size_t bucketCount = (be - bb) >> 3;
    if (!bucketCount || bucketCount > (1u << 20)) return nullptr;
    std::uint64_t nodeIndex = *reinterpret_cast<const std::uint64_t*>(bb + (((bucketCount - 1) & typeHash) << 3));
    std::uintptr_t storage = 0;
    for (std::size_t guard = 0; nodeIndex != noNode && guard < 512; ++guard) {
        if (nodeIndex > (1u << 24)) return nullptr;
        const auto node = nodes + static_cast<std::uintptr_t>(nodeIndex) * 0x20;
        if (!plausiblePtr(node)) return nullptr;
        if (*reinterpret_cast<const std::uint32_t*>(node + 0x8) == typeHash) {
            if (node == sentinel) return nullptr;
            storage = *reinterpret_cast<const std::uintptr_t*>(node + 0x10);
            break;
        }
        nodeIndex = *reinterpret_cast<const std::uint64_t*>(node);
    }
    if (!plausiblePtr(storage)) return nullptr;
    const auto storageBegin = *reinterpret_cast<const std::uintptr_t*>(storage + 0x08);
    const auto storageEnd = *reinterpret_cast<const std::uintptr_t*>(storage + 0x10);
    if (!plausiblePtr(storageBegin) || !plausiblePtr(storageEnd) || storageEnd < storageBegin) return nullptr;
    const std::size_t page = (static_cast<std::size_t>(entity) >> 11) & 0x7F;
    if (page >= ((storageEnd - storageBegin) >> 3)) return nullptr;
    const auto sparsePage = *reinterpret_cast<const std::uintptr_t*>(storageBegin + page * 8);
    if (!plausiblePtr(sparsePage)) return nullptr;
    const std::size_t slot = (static_cast<std::size_t>(entity) & 0x3FFFFu) & 0x7FFu;
    const std::uint32_t packed = *reinterpret_cast<const std::uint32_t*>(sparsePage + slot * 4);
    if ((packed ^ (entity & 0xFFFC0000u)) > 0x3FFFEu) return nullptr;
    const auto dense = *reinterpret_cast<const std::uintptr_t*>(storage + 0x50);
    if (!plausiblePtr(dense)) return nullptr;
    const auto densePage = *reinterpret_cast<const std::uintptr_t*>(dense + ((static_cast<std::size_t>(packed) >> 4) & 0x3FF8u));
    if (!plausiblePtr(densePage)) return nullptr;
    const auto component = densePage + static_cast<std::size_t>(packed & 0x7Fu) * stride;
    return plausiblePtr(component, 4) ? reinterpret_cast<void*>(component) : nullptr;
}

bool playerAttribute(void* player, const std::string& canonical, const std::string& shortName,
                     float minimum, float maximum, float& value) {
    if (!g_getAttributeByName) return false;
    void* attributes = runtimeComponent(player, kAttributesHash, kAttributesStride);
    if (!attributes) return false;
    void* instance = g_getAttributeByName(attributes, &canonical);
    if (!instance) instance = g_getAttributeByName(attributes, &shortName);
    if (!instance || !plausiblePtr(reinterpret_cast<std::uintptr_t>(instance), 4)) return false;
    const float current = *reinterpret_cast<const float*>(static_cast<const std::byte*>(instance) + 0x7C);
    if (!std::isfinite(current) || current < minimum || current > maximum) return false;
    value = current;
    return true;
}

bool playerLevel(void* player, int& level) {
    static const std::string canonical = "minecraft:player.level";
    static const std::string shortName = "player.level";
    float current = 0.0f;
    if (!playerAttribute(player, canonical, shortName, 0.0f, 1000000.0f, current)) return false;
    level = static_cast<int>(std::floor(current + .001f));
    return true;
}

bool playerHunger(void* player, float& value) {
    static const std::string canonical = "minecraft:player.hunger";
    static const std::string shortName = "player.hunger";
    return playerAttribute(player, canonical, shortName, 0.0f, 20.1f, value);
}
bool playerSaturation(void* player, float& value) {
    static const std::string canonical = "minecraft:player.saturation";
    static const std::string shortName = "player.saturation";
    return playerAttribute(player, canonical, shortName, 0.0f, 20.1f, value);
}

int levelTime(worldanalysis::sdk::Level* level) {
    if (!level || !plausiblePtr(reinterpret_cast<std::uintptr_t>(level)))
        return std::numeric_limits<int>::min();
    auto** table = *reinterpret_cast<void***>(level);
    constexpr auto slot = worldanalysis::sdk::offsets::VTable::Level_getTime;
    if (!table || !plausiblePtr(reinterpret_cast<std::uintptr_t>(table[slot]), 4))
        return std::numeric_limits<int>::min();
    using Fn = int (*)(void*);
    const int value = reinterpret_cast<Fn>(table[slot])(level);
    // Keep obviously corrupt ABI results from feeding day/sleep state. Normal
    // world times can be large, including negative values on unusual worlds.
    return (value > -2000000000 && value < 2000000000)
        ? value : std::numeric_limits<int>::min();
}

int dimensionCacheKey(DimensionKind kind) {
    return worldanalysis::sdk::dimension_identity::cacheKey(kind);
}

int weatherState(worldanalysis::sdk::Weather* weather, float& intensity) {
    if (!weather) { intensity = 0.0f; return 0; }
    const float rain = std::max(weather->rainLevel(), weather->targetRainLevel());
    const float lightning = std::max(weather->lightningLevel(), weather->targetLightningLevel());
    intensity = std::clamp(std::max(rain, lightning), 0.0f, 1.0f);
    if (lightning > .15f) return 2;
    if (rain > .15f) return 1;
    return 0;
}
std::string weatherName(int state) { return state == 2 ? "THUNDER" : state == 1 ? "RAIN" : "CLEAR"; }
float weatherSeverity(int state) { return state == 2 ? 1.0f : state == 1 ? .55f : 0.0f; }
int timePhase(int raw) {
    int time = raw % 24000;
    if (time < 0) time += 24000;
    if (time < 6000) return 0;
    if (time < 12000) return 1;
    if (time < 18000) return 2;
    return 3;
}
std::string timeName(int phase) {
    switch (phase) {
        case 0: return "MORNING";
        case 1: return "NOON / MIDDAY";
        case 2: return "EVENING";
        default: return "MIDNIGHT";
    }
}
std::vector<float> timeGraph(int phase) {
    switch (phase) {
        case 0: return {.18f, .29f, .43f, .58f, .72f, .84f};
        case 1: return {.52f, .68f, .82f, .92f, .98f, 1.0f};
        case 2: return {.95f, .82f, .66f, .48f, .31f, .18f};
        default: return {.30f, .20f, .12f, .07f, .025f, 0.0f};
    }
}
std::vector<float> transitionGraph(float from, float to) {
    const float delta = to - from;
    return {from, from + delta * .16f, from + delta * .38f, from + delta * .62f,
            from + delta * .83f, to};
}

int floorDiv16(int value) {
    if (value >= 0) return value >> 4;
    return -static_cast<int>((static_cast<unsigned int>(-value) + 15u) >> 4);
}
bool isBedrockSlimeChunk(int chunkX, int chunkZ) {
    const std::uint32_t seed = static_cast<std::uint32_t>(chunkX) * 0x1F1F1F1Fu
        ^ static_cast<std::uint32_t>(chunkZ);
    std::mt19937 random(seed);
    return (random() % 10u) == 0u;
}

std::string_view blockIdentifier(const void* block) {
    if (!block) return {};
    const auto type = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(block) + worldanalysis::sdk::offsets::Block::mBlockType);
    if (!plausiblePtr(type)) return {};
    const auto address = type + worldanalysis::sdk::offsets::BlockType::mNameInfo
        + worldanalysis::sdk::offsets::NameInfo::mFullName
        + worldanalysis::sdk::offsets::HashedString::mString;
    const auto* value = reinterpret_cast<const std::string*>(address);
    if (value->size() > 128 || (!value->empty() && !value->data())) return {};
    return {value->data(), value->size()};
}

bool isAirBlock(const void* block) {
    const auto id = blockIdentifier(block);
    return id.empty() || id == "minecraft:air" || id == "minecraft:cave_air"
        || id == "minecraft:void_air";
}

std::pair<int, int> perimeterSampleBlock(int chunkX, int chunkZ, std::size_t index) {
    const int x0 = chunkX * 16, z0 = chunkZ * 16;
    index %= 64;
    if (index < 16) return {x0 + static_cast<int>(index), z0};
    if (index < 32) return {x0 + 15, z0 + static_cast<int>(index - 16)};
    if (index < 48) return {x0 + 15 - static_cast<int>(index - 32), z0 + 15};
    return {x0, z0 + 15 - static_cast<int>(index - 48)};
}

Vec3 perimeterPoint(int chunkX, int chunkZ, std::size_t index, float y) {
    const float x0 = static_cast<float>(chunkX * 16), z0 = static_cast<float>(chunkZ * 16);
    index %= 64;
    if (index < 16) return {x0 + static_cast<float>(index) + .5f, y, z0 + .01f};
    if (index < 32) return {x0 + 15.99f, y, z0 + static_cast<float>(index - 16) + .5f};
    if (index < 48) return {x0 + 15.5f - static_cast<float>(index - 32), y, z0 + 15.99f};
    return {x0 + .01f, y, z0 + 15.5f - static_cast<float>(index - 48)};
}

bool findSurfaceY(void* region, int x, int z, float& y) {
    if (!region || !g_getBlock) return false;
    for (int blockY = kOverworldMaxY; blockY >= kOverworldMinY; --blockY) {
        const BlockPosRaw pos{x, blockY, z};
        void* block = g_getBlock(region, pos);
        if (block && !isAirBlock(block)) { y = static_cast<float>(blockY + 1) + .025f; return true; }
    }
    return false;
}

void sampleGridSurface() {
    GridSnapshot snapshot;
    { std::lock_guard lock(g_mutex); snapshot = g_grid; }
    if (!snapshot.visible || !snapshot.region || snapshot.dimensionId != 0 || !g_getBlock) return;
    for (int n = 0; n < kSurfaceSamplesPerTick; ++n) {
        std::size_t index = snapshot.nextSurfaceSample % 64;
        bool foundPending = false;
        for (std::size_t attempt = 0; attempt < 64; ++attempt) {
            index = (snapshot.nextSurfaceSample + attempt) % 64;
            if (!snapshot.surfaceReady[index]) { foundPending = true; break; }
        }
        if (!foundPending) return;
        const auto [x, z] = perimeterSampleBlock(snapshot.chunkX, snapshot.chunkZ, index);
        float y = 0.0f;
        if (findSurfaceY(snapshot.region, x, z, y)) {
            std::lock_guard lock(g_mutex);
            if (g_grid.visible && g_grid.region == snapshot.region && g_grid.chunkX == snapshot.chunkX
                && g_grid.chunkZ == snapshot.chunkZ && g_grid.dimensionId == snapshot.dimensionId) {
                g_grid.surfaceY[index] = y;
                g_grid.surfaceReady[index] = true;
                g_grid.nextSurfaceSample = (index + 1) % 64;
                snapshot = g_grid;
            }
        } else {
            snapshot.nextSurfaceSample = (index + 1) % 64;
            std::lock_guard lock(g_mutex);
            if (g_grid.visible && g_grid.region == snapshot.region)
                g_grid.nextSurfaceSample = snapshot.nextSurfaceSample;
        }
    }
}

bool usesNodeNetwork(NotificationKind kind) {
    switch (kind) {
        case NotificationKind::SlimeChunk:
        case NotificationKind::Dimension:
        case NotificationKind::ModuleState:
            return true;
        default:
            return false;
    }
}

Vec3 notificationAnchor(const Vec3& eye, const Vec2& rotation, int lane) {
    struct Lane { float lateral; float vertical; float depth; };
    static constexpr std::array<Lane, kMaxNotifications> lanes{{
        {-1.30f,  .02f, 3.35f}, { 1.30f,  .02f, 3.35f},
        {-1.42f,  .48f, 3.55f}, { 1.42f,  .48f, 3.55f},
        {-1.42f, -.48f, 3.55f}, { 1.42f, -.48f, 3.55f},
        {-1.68f,  .82f, 3.85f}, { 1.68f, -.82f, 3.85f}
    }};
    lane = std::clamp(lane, 0, static_cast<int>(lanes.size() - 1));
    const float yaw = rotation.y * kPi / 180.0f;
    const Vec3 forward{-std::sin(yaw), 0, std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0, std::sin(yaw)};
    return add(eye, add(mul(forward, lanes[lane].depth),
        add(mul(right, lanes[lane].lateral), Vec3{0, lanes[lane].vertical, 0})));
}

int reserveLaneLocked(Clock::time_point now) {
    g_notifications.erase(std::remove_if(g_notifications.begin(), g_notifications.end(),
        [&](const Notification& n) { return n.expires <= now; }), g_notifications.end());
    std::array<bool, kMaxNotifications> used{};
    for (const auto& note : g_notifications)
        if (note.lane >= 0 && note.lane < static_cast<int>(used.size())) used[note.lane] = true;
    for (std::size_t i = 0; i < used.size(); ++i) if (!used[i]) return static_cast<int>(i);
    const auto oldest = std::min_element(g_notifications.begin(), g_notifications.end(),
        [](const Notification& a, const Notification& b) { return a.created < b.created; });
    const int lane = oldest == g_notifications.end() ? 0 : oldest->lane;
    if (oldest != g_notifications.end()) g_notifications.erase(oldest);
    return lane;
}

void addNotification(NotificationKind kind, std::string title, std::string body,
                     std::vector<float> samples = {}) {
    if (!g_mod) return;
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (!g_playerReady) return;
    for (const auto& existing : g_notifications) {
        if (existing.kind == kind && existing.body == body
            && std::chrono::duration<float>(now - existing.created).count() < .45f) return;
    }
    const int lane = reserveLaneLocked(now);
    Notification note{};
    note.kind = kind;
    note.lane = lane;
    note.anchor = notificationAnchor(g_eyePosition, g_playerRotation, lane);
    note.title = worldanalysis::worldoverlay::sanitizeText(title, 32);
    note.body = worldanalysis::worldoverlay::sanitizeText(body, 32);
    note.samples = std::move(samples);
    note.created = now;
    note.expires = now + std::chrono::milliseconds(static_cast<int>(
        std::clamp(g_mod->notificationDuration, 1.0f, 10.0f) * 1000.0f));
    g_notifications.push_back(std::move(note));
}

Vec3 graphPoint(const Notification& note, const Vec3& right, const Vec3& up,
                std::size_t index, float normalized, float intro) {
    const float count = std::max<std::size_t>(2, note.samples.size());
    const float x = -.50f + 1.00f * static_cast<float>(index) / (count - 1.0f);
    const float y = -.245f + .135f * normalized;
    return add(note.anchor, add(mul(right, x * intro), mul(up, y * intro)));
}

void appendDot(std::vector<Segment>& out, const Vec3& center, const Vec3& right,
               const Vec3& up, float radius, float phase) {
    constexpr int sides = 12;
    const float pulse = 1.0f + .14f * std::sin(phase);
    radius *= pulse;
    for (int i = 0; i < sides; ++i) {
        const float a = 2.0f * kPi * i / sides;
        const float b = 2.0f * kPi * (i + 1) / sides;
        out.push_back({
            add(center, add(mul(right, std::cos(a) * radius), mul(up, std::sin(a) * radius))),
            add(center, add(mul(right, std::cos(b) * radius), mul(up, std::sin(b) * radius)))
        });
    }
}

void renderLineGraph(const worldanalysis::worldoverlay::RenderContext& context, void* material,
                     const Notification& note, const Vec3& right, const Vec3& up,
                     float intro, float alpha, float age, float speed) {
    if (note.samples.size() < 2) return;
    float lo = *std::min_element(note.samples.begin(), note.samples.end());
    float hi = *std::max_element(note.samples.begin(), note.samples.end());
    if (std::abs(hi - lo) < .001f) { lo -= .5f; hi += .5f; }
    std::vector<Vec3> points;
    points.reserve(note.samples.size());
    for (std::size_t i = 0; i < note.samples.size(); ++i) {
        const float normalized = std::clamp((note.samples[i] - lo) / (hi - lo), 0.0f, 1.0f);
        points.push_back(graphPoint(note, right, up, i, normalized, intro));
    }
    const float reveal = smoothStep((age - .18f / speed) / (.72f / speed));
    const float segmentFloat = reveal * static_cast<float>(points.size() - 1);
    const std::size_t fullSegments = static_cast<std::size_t>(std::floor(segmentFloat));
    const float partial = segmentFloat - static_cast<float>(fullSegments);
    std::vector<Segment> lines;
    for (std::size_t i = 0; i < std::min(fullSegments, points.size() - 1); ++i)
        lines.push_back({points[i], points[i + 1]});
    if (fullSegments < points.size() - 1 && partial > .001f) {
        const Vec3 end = add(points[fullSegments], mul(sub(points[fullSegments + 1], points[fullSegments]), partial));
        lines.push_back({points[fullSegments], end});
    }
    if (!lines.empty()) worldanalysis::worldoverlay::drawLines(context, material, lines, white(.90f * alpha), 2.0f);
    std::vector<Segment> dots;
    const std::size_t visibleDots = std::min(points.size(), fullSegments + (partial > .12f ? 2u : 1u));
    for (std::size_t i = 0; i < visibleDots; ++i)
        appendDot(dots, points[i], right, up, .018f * intro, age * 6.0f + static_cast<float>(i) * .8f);
    if (!dots.empty()) worldanalysis::worldoverlay::drawLines(context, material, dots, white(alpha), 2.3f);
}

void renderNodeNetwork(const worldanalysis::worldoverlay::RenderContext& context, void* material,
                       const Notification& note, const Vec3& right, const Vec3& up,
                       float intro, float alpha, float age, float speed) {
    static constexpr std::array<std::array<float, 2>, 7> nodes{{
        {{-.46f, -.22f}}, {{-.27f, -.14f}}, {{-.08f, -.25f}}, {{.10f, -.13f}},
        {{.28f, -.23f}}, {{.45f, -.15f}}, {{.05f, -.30f}}
    }};
    static constexpr std::array<std::array<int, 2>, 9> links{{
        {{0,1}}, {{1,2}}, {{1,3}}, {{2,3}}, {{2,6}}, {{3,4}}, {{3,6}}, {{4,5}}, {{4,6}}
    }};
    std::array<Vec3, nodes.size()> world{};
    for (std::size_t i = 0; i < nodes.size(); ++i)
        world[i] = add(note.anchor, add(mul(right, nodes[i][0] * intro), mul(up, nodes[i][1] * intro)));
    const float reveal = smoothStep((age - .12f / speed) / (.68f / speed));
    std::vector<Segment> lines;
    const float linkFloat = reveal * static_cast<float>(links.size());
    const std::size_t full = static_cast<std::size_t>(std::floor(linkFloat));
    for (std::size_t i = 0; i < std::min(full, links.size()); ++i)
        lines.push_back({world[links[i][0]], world[links[i][1]]});
    if (full < links.size()) {
        const float part = linkFloat - static_cast<float>(full);
        if (part > .001f) {
            const Vec3 a = world[links[full][0]], b = world[links[full][1]];
            lines.push_back({a, add(a, mul(sub(b, a), part))});
        }
    }
    if (!lines.empty()) worldanalysis::worldoverlay::drawLines(context, material, lines, grey(.72f * alpha, 190), 1.8f);
    std::vector<Segment> dots;
    const std::size_t visible = std::min(nodes.size(), static_cast<std::size_t>(1 + reveal * nodes.size()));
    for (std::size_t i = 0; i < visible; ++i)
        appendDot(dots, world[i], right, up, .020f * intro,
                  age * 7.0f + static_cast<float>(i) * .9f);
    if (!dots.empty()) worldanalysis::worldoverlay::drawLines(context, material, dots, white(alpha), 2.2f);
}

void appendTerrainPerimeter(std::vector<Segment>& out, const GridSnapshot& grid, float reveal) {
    const float progress = std::clamp(reveal, 0.0f, 1.0f) * 64.0f;
    const std::size_t complete = static_cast<std::size_t>(std::floor(progress));
    const float partial = progress - static_cast<float>(complete);
    auto appendStep = [&](std::size_t index, float part) {
        const std::size_t next = (index + 1) % 64;
        if (!grid.surfaceReady[index] || !grid.surfaceReady[next] || part <= .001f) return;
        const Vec3 a = perimeterPoint(grid.chunkX, grid.chunkZ, index, grid.surfaceY[index]);
        const Vec3 b = perimeterPoint(grid.chunkX, grid.chunkZ, next, grid.surfaceY[next]);
        const Vec3 horizontalEnd{a.x + (b.x - a.x) * part, a.y, a.z + (b.z - a.z) * part};
        out.push_back({a, horizontalEnd});
        if (part >= .999f && std::abs(a.y - b.y) > .01f)
            out.push_back({horizontalEnd, b});
    };
    for (std::size_t i = 0; i < std::min<std::size_t>(complete, 64); ++i) appendStep(i, 1.0f);
    if (complete < 64 && partial > .001f) appendStep(complete, partial);
}

void appendFullHeightBoundary(std::vector<Segment>& out, const GridSnapshot& grid,
                              float reveal, float blend) {
    blend = smoothStep(std::clamp(blend, 0.0f, 1.0f));
    if (blend <= .002f) return;
    const float top = static_cast<float>(kOverworldMaxY + 1);
    const float bottom = static_cast<float>(kOverworldMinY);
    for (std::size_t i = 0; i < 64; i += 4) {
        if (!grid.surfaceReady[i]) continue;
        const Vec3 surface = perimeterPoint(grid.chunkX, grid.chunkZ, i, grid.surfaceY[i]);
        const float yTop = surface.y + (top - surface.y) * blend * reveal;
        const float yBottom = surface.y + (bottom - surface.y) * blend * reveal;
        out.push_back({{surface.x, yBottom, surface.z}, {surface.x, yTop, surface.z}});
    }

    // Once the vertical growth is visible, cap both ends with a chunk-edge
    // perimeter. The rings themselves grow with the same smooth blend.
    if (blend > .04f) {
        const float x0 = static_cast<float>(grid.chunkX * 16);
        const float z0 = static_cast<float>(grid.chunkZ * 16);
        const float x1 = x0 + 16.0f, z1 = z0 + 16.0f;
        float averageSurface = 0.0f; int ready = 0;
        for (std::size_t i = 0; i < 64; ++i) if (grid.surfaceReady[i]) { averageSurface += grid.surfaceY[i]; ++ready; }
        if (!ready) return;
        averageSurface /= static_cast<float>(ready);
        const float yTop = averageSurface + (top - averageSurface) * blend * reveal;
        const float yBottom = averageSurface + (bottom - averageSurface) * blend * reveal;
        auto ring = [&](float y) {
            out.push_back({{x0,y,z0},{x1,y,z0}}); out.push_back({{x1,y,z0},{x1,y,z1}});
            out.push_back({{x1,y,z1},{x0,y,z1}}); out.push_back({{x0,y,z1},{x0,y,z0}});
        };
        ring(yTop); ring(yBottom);
    }
}

void renderSlimeGrid(const worldanalysis::worldoverlay::RenderContext& context) {
    if (!g_mod || !g_mod->enabled) return;
    GridSnapshot grid;
    { std::lock_guard lock(g_mutex); grid = g_grid; }
    if (!grid.visible || grid.opacity <= .01f) return;
    // Slime chunk geometry is intentionally depth-tested: the notification
    // card may show through terrain, but the terrain outline itself should not.
    void* material = worldanalysis::worldoverlay::depthMaterial();
    if (!material) return;

    const auto now = Clock::now();
    const float speed = std::clamp(g_mod->animationSpeed, .5f, 1.5f);
    const float age = std::max(0.0f, std::chrono::duration<float>(now - grid.enteredAt).count());
    const float reveal = smoothStep(age / (.65f / speed));
    const float pulse = .90f + .10f * std::sin(age * 2.6f);
    const float alpha = std::clamp(grid.opacity * pulse, 0.0f, 1.0f);

    std::vector<Segment> ground;
    ground.reserve(160);
    appendTerrainPerimeter(ground, grid, reveal);
    if (!ground.empty())
        worldanalysis::worldoverlay::drawLines(context, material, ground, white(.94f * alpha), 2.5f);

    std::vector<Segment> fullHeight;
    fullHeight.reserve(40);
    appendFullHeightBoundary(fullHeight, grid, reveal, grid.modeBlend);
    if (!fullHeight.empty())
        worldanalysis::worldoverlay::drawLines(context, material, fullHeight, white(.52f * alpha), 1.6f);
}

void renderNotifications(void* self, void* screen, void* a3) {
    (void)a3;
    if (!g_mod || !g_mod->enabled) return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color,g_mod->backgroundColor,g_mod->backgroundOpacity);
    worldanalysis::worldoverlay::RenderContext context;
    if (!worldanalysis::worldoverlay::makeContext(self, screen, context)) return;

    renderSlimeGrid(context);

    std::vector<Notification> notes;
    {
        std::lock_guard lock(g_mutex);
        notes = g_notifications;
    }
    void* material = worldanalysis::worldoverlay::throughWallMaterial();
    if (!material) return;
    const auto now = Clock::now();
    const float speed = std::clamp(g_mod->animationSpeed, .5f, 1.5f);
    for (const auto& note : notes) {
        if (note.expires <= now) continue;
        const float age = std::chrono::duration<float>(now - note.created).count();
        const float remaining = std::chrono::duration<float>(note.expires - now).count();
        const float intro = smoothStep(age / (.30f / speed));
        const float fade = smoothStep(remaining / (.42f / speed));
        const float alpha = intro * fade;
        if (alpha <= .001f) continue;

        Vec3 right{}, up{};
        worldanalysis::worldoverlay::billboardBasis(note.anchor, context.camera, right, up);
        const float halfW = .72f * intro, halfH = .37f * intro;
        const Vec3 bl = add(note.anchor, add(mul(right, -halfW), mul(up, -halfH)));
        const Vec3 br = add(note.anchor, add(mul(right,  halfW), mul(up, -halfH)));
        const Vec3 tr = add(note.anchor, add(mul(right,  halfW), mul(up,  halfH)));
        const Vec3 tl = add(note.anchor, add(mul(right, -halfW), mul(up,  halfH)));
        worldanalysis::worldoverlay::drawQuads(context, material, {{bl,br,tr,tl}}, (static_cast<std::uint32_t>(alpha * 28.f) << 24));
        worldanalysis::worldoverlay::drawLines(context, material,
            {{bl,br},{br,tr},{tr,tl},{tl,bl}}, white(alpha), 2.1f);

        const float textAlpha = alpha * smoothStep((intro - .28f) / .72f);
        constexpr float innerWidth = 1.18f;
        const float titlePx = worldanalysis::worldoverlay::fitBillboardTextPixelSize(
            note.title, innerWidth, .0185f, .0085f);
        const float bodyPx = worldanalysis::worldoverlay::fitBillboardTextPixelSize(
            note.body, innerWidth, .0140f, .0075f);
        worldanalysis::worldoverlay::drawBillboardTextOriented(context, material, note.title,
            add(note.anchor, mul(up, .225f)), right, up, titlePx, white(textAlpha), false);
        worldanalysis::worldoverlay::drawBillboardTextOriented(context, material, note.body,
            add(note.anchor, mul(up, .070f)), right, up, bodyPx, white(textAlpha), false);

        if (usesNodeNetwork(note.kind))
            renderNodeNetwork(context, material, note, right, up, intro, textAlpha, age, speed);
        else
            renderLineGraph(context, material, note, right, up, intro, textAlpha, age, speed);
    }
}

void resetTracking() {
    g_weatherState = -1;
    g_weatherIntensity = 0.0f;
    g_timePhase = -1;
    g_lastLevel = -1;
    g_lastHealth = std::numeric_limits<int>::min();
    g_criticalHealthSeen = false;
    g_chunkReady = false;
    g_lastChunkDimension = std::numeric_limits<int>::min();
    g_awakeTicks = 0;
    g_lastRawTime = std::numeric_limits<int>::min();
    g_sleepAlerted = false;
    g_lastDay = std::numeric_limits<std::int64_t>::min();
    g_dimensionPtr = nullptr;
    g_dimensionKind = DimensionKind::Unknown;
    g_previousDimensionKind = DimensionKind::Unknown;
    g_dimensionNoticePending = false;
    g_dimensionStableTicks = 0;
    g_lastHunger = std::numeric_limits<float>::quiet_NaN();
    g_lastSaturation = std::numeric_limits<float>::quiet_NaN();
    g_hungerAlerted = false;
    g_saturationAlerted = false;
    g_gridWasEnabled = false;
    g_poweredOnPending = false;
    { std::lock_guard lock(g_mutex); g_grid = {}; }
}

void updateGridState(bool inSlimeChunk, bool chunkChanged, int chunkX, int chunkZ,
                     int dimId, void* region) {
    if (!g_mod) return;
    const auto now = Clock::now();
    const float speed = std::clamp(g_mod->animationSpeed, .5f, 1.5f);
    const float duration = std::clamp(g_mod->slimeChunkGridDuration, 3.0f, 60.0f);
    const float targetBlend = std::clamp(g_mod->slimeChunkGridMode / 100.0f, 0.0f, 1.0f);
    std::lock_guard lock(g_mutex);

    // A highlight is triggered once when the player enters a slime chunk. It
    // expires even if the player stays there, so chunks never remain painted.
    const bool gridJustEnabled = g_mod->slimeChunkGrid && !g_gridWasEnabled;
    g_gridWasEnabled = g_mod->slimeChunkGrid;
    if (g_mod->slimeChunkGrid && inSlimeChunk && (chunkChanged || gridJustEnabled)) {
        g_grid = {};
        g_grid.visible = true;
        g_grid.chunkX = chunkX;
        g_grid.chunkZ = chunkZ;
        g_grid.dimensionId = dimId;
        g_grid.region = region;
        g_grid.enteredAt = now;
        g_grid.expiresAt = now + std::chrono::milliseconds(static_cast<int>(duration * 1000.0f));
        g_grid.modeBlend = 0.0f;
    }

    if (!g_grid.visible) return;
    const bool sameChunk = inSlimeChunk && g_grid.chunkX == chunkX && g_grid.chunkZ == chunkZ
        && g_grid.dimensionId == dimId && g_grid.region == region;
    const bool alive = sameChunk && g_mod->slimeChunkGrid && now < g_grid.expiresAt;
    const float targetOpacity = alive ? 1.0f : 0.0f;
    g_grid.opacity += (targetOpacity - g_grid.opacity) * std::clamp(.20f * speed, .10f, .34f);
    g_grid.modeBlend += (targetBlend - g_grid.modeBlend) * std::clamp(.14f * speed, .07f, .24f);
    if (g_grid.opacity < .008f && !alive) {
        g_grid.opacity = 0.0f;
        g_grid.visible = false;
        g_grid.region = nullptr;
    }
}

} // namespace

SystemNotificationsModule::SystemNotificationsModule()
    : Module("System Notifications",
        "World-space technical alerts beside the current view. Includes weather/time/XP, slime chunks with a temporary terrain/full-height outline, sleep/health, dimension, hunger/saturation and recovery events. Cards use fitted text plus smooth line-dot or node-network graphs and remain visible through terrain.") {
    g_mod = this;
    showInMenu = true;
    exposeBackgroundStyle = true;
}
SystemNotificationsModule::~SystemNotificationsModule() { if (g_mod == this) g_mod = nullptr; }

void SystemNotificationsModule::onInit() {
    // Do not resolve SignatureId::Time here. On the supplied 26.45 binary that
    // pattern lands on an unrelated aggregate-returning virtual wrapper and
    // calling it as int(Level*) is the loaded-world crash this module had.
    if (auto address = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))
        g_getBlock = reinterpret_cast<GetBlockFn>(address);
    if (auto address = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetHealth))
        g_getHealth = reinterpret_cast<ActorGetHealthFn>(address);
    if (auto address = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::AttributesGetInstanceByName))
        g_getAttributeByName = reinterpret_cast<AttributesGetByNameFn>(address);
    worldanalysis::worldoverlay::initialize();
    worldanalysis::worldoverlay::registerRenderCallback(renderNotifications);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>(
        [](auto& event) { if (g_mod) g_mod->handleTick(event.player); });
}

void SystemNotificationsModule::onEnable() {
    // The render callback is registered once in onInit(). Re-registering here
    // duplicates the world pass after repeated toggles and can multiply cards.
    resetTracking();
    std::lock_guard lock(g_mutex);
    g_notifications.clear();
    g_playerReady = false;
    // The master module always acknowledges its own activation. The child
    // Module Notification toggle controls reports for *other* modules only.
    g_poweredOnPending = true;
}

void SystemNotificationsModule::onDisable() {
    resetTracking();
    std::lock_guard lock(g_mutex);
    g_notifications.clear();
    g_playerReady = false;
}

void SystemNotificationsModule::handleTick(worldanalysis::sdk::Player* player) {
    if (!enabled || !player) return;
    const bool anyFeature = weatherNotifications || timeNotifications || levelNotifications
        || slimeChunkNotifications || sleepDeprivedNotifications || hpLowNotifications
        || dimensionNotifications || dayRolloverNotifications || hungerLowNotifications
        || saturationEmptyNotifications
        || levelDownNotifications || healthRecoveredNotifications
        || moduleNotifications || slimeChunkGrid || g_poweredOnPending;
    if (!anyFeature) {
        std::lock_guard lock(g_mutex);
        g_playerReady = false;
        g_notifications.clear();
        g_grid = {};
        return;
    }
    const Vec3 position = player->position();
    if (!finite(position)) return;
    const auto bounds = player->bounds();
    Vec3 eye{position.x, position.y + 1.62f, position.z};
    if (finite(bounds.max) && finite(bounds.min) && bounds.max.y > bounds.min.y)
        eye.y = bounds.max.y - .12f;
    {
        std::lock_guard lock(g_mutex);
        g_eyePosition = eye;
        g_playerRotation = player->rotation();
        g_playerReady = true;
        const auto now = Clock::now();
        g_notifications.erase(std::remove_if(g_notifications.begin(), g_notifications.end(),
            [&](const Notification& note) { return note.expires <= now; }), g_notifications.end());
    }
    if (g_poweredOnPending) {
        g_poweredOnPending = false;
        addNotification(NotificationKind::ModuleState, "SYSTEM NOTIFICATIONS", "POWERED ON",
            {.18f,.72f,.38f,.88f,.54f,1.0f});
    }

    // Dimension identity is read from the object's RTTI. Do not call
    // BlockSource::getDimensionId here: that manually indexed virtual was the
    // remaining dimension-toggle crash surface on the supplied 26.45 image.
    auto* dimension = player->dimension();
    const DimensionKind currentKind = worldanalysis::sdk::dimension_identity::kind(dimension);
    const int dimId = dimensionCacheKey(currentKind);
    if (dimension != g_dimensionPtr) {
        const DimensionKind oldKind = g_dimensionKind;
        g_dimensionPtr = dimension;
        g_previousDimensionKind = oldKind;
        g_dimensionKind = currentKind;
        g_dimensionStableTicks = 0;
        g_dimensionNoticePending = oldKind != DimensionKind::Unknown
            && currentKind != DimensionKind::Unknown && oldKind != currentKind;

        // Never carry transition-sensitive pointers/state across dimensions.
        g_weatherState = -1;
        g_timePhase = -1;
        g_chunkReady = false;
        g_lastChunkDimension = std::numeric_limits<int>::min();
        g_lastRawTime = std::numeric_limits<int>::min();
        g_lastDay = std::numeric_limits<std::int64_t>::min();
        g_gridWasEnabled = false;
        { std::lock_guard lock(g_mutex); g_grid = {}; }
    } else {
        g_dimensionKind = currentKind;
        if (g_dimensionStableTicks < kDimensionStableTicksRequired) ++g_dimensionStableTicks;
    }

    // Give a newly loaded/changed dimension a handful of local-player ticks
    // before touching Weather, Level, BlockSource, ECS attributes or held-item
    // state. This prevents transition-frame pointers from being consumed by
    // optional notification features.
    if (!dimension || currentKind == DimensionKind::Unknown
        || g_dimensionStableTicks < kDimensionStableTicksRequired) {
        return;
    }

    if (g_dimensionNoticePending) {
        if (dimensionNotifications) {
            addNotification(NotificationKind::Dimension, "DIMENSION CHANGED",
                std::string(worldanalysis::sdk::dimension_identity::displayName(g_previousDimensionKind))
                    + " -> " + worldanalysis::sdk::dimension_identity::displayName(currentKind));
        }
        g_dimensionNoticePending = false;
    }

    if (weatherNotifications) {
        float intensity = 0.0f;
        const int state = weatherState(dimension->weather(), intensity);
        if (g_weatherState < 0) {
            g_weatherState = state;
            g_weatherIntensity = intensity;
        } else if (state != g_weatherState) {
            const float from = weatherSeverity(g_weatherState), to = weatherSeverity(state);
            addNotification(NotificationKind::Weather, "WEATHER CHANGE",
                weatherName(g_weatherState) + " -> " + weatherName(state), transitionGraph(from, to));
            g_weatherState = state;
            g_weatherIntensity = intensity;
        } else {
            g_weatherIntensity = intensity;
        }
    } else {
        g_weatherState = -1;
    }

    // The old SignatureId::Time wrapper is never called. 26.45's actual
    // Level time getter is the verified virtual slot +0x428 (133), also used
    // by native helpers that reduce the returned value modulo 24,000.
    int rawTime = std::numeric_limits<int>::min();
    if (timeNotifications || dayRolloverNotifications || sleepDeprivedNotifications)
        rawTime = levelTime(player->level());

    if (timeNotifications && rawTime != std::numeric_limits<int>::min()) {
        const int phase = timePhase(rawTime);
        if (g_timePhase < 0) g_timePhase = phase;
        else if (phase != g_timePhase) {
            addNotification(NotificationKind::Time, "TIME OF DAY", timeName(phase), timeGraph(phase));
            g_timePhase = phase;
        }
    } else if (!timeNotifications) {
        g_timePhase = -1;
    }

    if (dayRolloverNotifications && rawTime != std::numeric_limits<int>::min()) {
        const std::int64_t day = static_cast<std::int64_t>(rawTime) >= 0
            ? static_cast<std::int64_t>(rawTime) / 24000 : 0;
        if (g_lastDay == std::numeric_limits<std::int64_t>::min()) g_lastDay = day;
        else if (day > g_lastDay) {
            addNotification(NotificationKind::DayRollover, "NEW MINECRAFT DAY",
                "DAY " + std::to_string(day), {.05f,.22f,.38f,.58f,.78f,1.0f});
            g_lastDay = day;
        } else if (day < g_lastDay) {
            g_lastDay = day;
        }
    } else if (!dayRolloverNotifications) {
        g_lastDay = std::numeric_limits<std::int64_t>::min();
    }

    if (levelNotifications || levelDownNotifications) {
        int level = 0;
        if (playerLevel(player, level)) {
            if (g_lastLevel < 0) g_lastLevel = level;
            else if (level > g_lastLevel) {
                if (levelNotifications) {
                    addNotification(NotificationKind::LevelUp, "LEVEL UP",
                        "LEVEL " + std::to_string(g_lastLevel) + " -> " + std::to_string(level),
                        transitionGraph(static_cast<float>(g_lastLevel), static_cast<float>(level)));
                }
                g_lastLevel = level;
            } else if (level < g_lastLevel) {
                if (levelDownNotifications) {
                    addNotification(NotificationKind::LevelDown, "LEVEL SPENT / LOST",
                        "LEVEL " + std::to_string(g_lastLevel) + " -> " + std::to_string(level),
                        transitionGraph(static_cast<float>(g_lastLevel), static_cast<float>(level)));
                }
                g_lastLevel = level;
            }
        }
    } else {
        g_lastLevel = -1;
    }

    const int blockX = static_cast<int>(std::floor(position.x));
    const int blockZ = static_cast<int>(std::floor(position.z));
    const int chunkX = floorDiv16(blockX), chunkZ = floorDiv16(blockZ);
    const bool needChunkState = slimeChunkNotifications || slimeChunkGrid;
    bool inSlimeChunk = false;
    bool chunkChanged = false;
    if (needChunkState) {
        inSlimeChunk = (dimId == 0) && isBedrockSlimeChunk(chunkX, chunkZ);
        chunkChanged = !g_chunkReady || chunkX != g_lastChunkX || chunkZ != g_lastChunkZ
            || dimId != g_lastChunkDimension;
        if (chunkChanged) {
            if (slimeChunkNotifications && inSlimeChunk) {
                addNotification(NotificationKind::SlimeChunk, "SLIME CHUNK DETECTED",
                    "CHUNK " + std::to_string(chunkX) + ", " + std::to_string(chunkZ));
            }
            g_chunkReady = true;
            g_lastChunkX = chunkX;
            g_lastChunkZ = chunkZ;
            g_lastChunkDimension = dimId;
        }
    } else {
        g_chunkReady = false;
    }
    void* region = dimension->blockSource();
    updateGridState(inSlimeChunk, chunkChanged, chunkX, chunkZ, dimId, region);
    sampleGridSurface();

    if (sleepDeprivedNotifications && rawTime != std::numeric_limits<int>::min()) {
        if (g_lastRawTime == std::numeric_limits<int>::min()) {
            g_lastRawTime = rawTime;
        } else {
            const std::int64_t delta = static_cast<std::int64_t>(rawTime) - g_lastRawTime;
            const int previousPhase = ((g_lastRawTime % 24000) + 24000) % 24000;
            const int currentPhase = ((rawTime % 24000) + 24000) % 24000;
            const bool slept = delta > 100 && previousPhase >= 12000 && currentPhase <= 1200;
            if (slept) {
                g_awakeTicks = 0;
                g_sleepAlerted = false;
            } else if (delta > 0 && delta <= 100) {
                g_awakeTicks += delta;
            }
            g_lastRawTime = rawTime;
        }
        if (!g_sleepAlerted && g_awakeTicks >= kInsomniaThresholdTicks) {
            addNotification(NotificationKind::SleepDeprived, "SLEEP DEPRIVED",
                "3+ DAYS WITHOUT SLEEP", {1.0f,.86f,.68f,.49f,.31f,.15f,.04f});
            g_sleepAlerted = true;
        }
    } else if (!sleepDeprivedNotifications) {
        g_awakeTicks = 0;
        g_lastRawTime = std::numeric_limits<int>::min();
        g_sleepAlerted = false;
    }

    if ((hpLowNotifications || healthRecoveredNotifications) && g_getHealth) {
        const int health = g_getHealth(player);
        if (health >= 0 && health < 1000000) {
            if (g_lastHealth == std::numeric_limits<int>::min()) {
                g_lastHealth = health;
                g_criticalHealthSeen = health <= 2;
                if (hpLowNotifications && health <= 2) {
                    addNotification(NotificationKind::HpLow, "HP LOW", "1 HEART REMAINING",
                        {10.0f,8.2f,6.5f,4.9f,3.2f,2.0f});
                }
            } else {
                if (health <= 2 && g_lastHealth > 2) {
                    g_criticalHealthSeen = true;
                    if (hpLowNotifications) {
                        const float start = static_cast<float>(std::max(g_lastHealth, 8));
                        addNotification(NotificationKind::HpLow, "HP LOW", "1 HEART REMAINING",
                            transitionGraph(start, 2.0f));
                    }
                }
                if (healthRecoveredNotifications && g_criticalHealthSeen && health >= 6) {
                    addNotification(NotificationKind::HpRecovered, "HP RECOVERED",
                        std::to_string(health) + " HP", transitionGraph(2.0f, static_cast<float>(health)));
                    g_criticalHealthSeen = false;
                }
                g_lastHealth = health;
            }
        }
    } else {
        g_lastHealth = std::numeric_limits<int>::min();
        g_criticalHealthSeen = false;
    }

    if (hungerLowNotifications) {
        float hunger = 0.0f;
        if (playerHunger(player, hunger)) {
            const float previous = g_lastHunger;
            if (std::isnan(previous)) {
                g_lastHunger = hunger;
                g_hungerAlerted = hunger <= kHungerLowThreshold;
            } else {
                if (!g_hungerAlerted && hunger <= kHungerLowThreshold && previous > kHungerLowThreshold) {
                    addNotification(NotificationKind::HungerLow, "HUNGER LOW",
                        std::to_string(static_cast<int>(std::lround(hunger))) + " / 20",
                        transitionGraph(std::max(previous, 10.0f), hunger));
                    g_hungerAlerted = true;
                } else if (g_hungerAlerted && hunger >= kHungerResetThreshold) {
                    g_hungerAlerted = false;
                }
                g_lastHunger = hunger;
            }
        }
    } else {
        g_lastHunger = std::numeric_limits<float>::quiet_NaN();
        g_hungerAlerted = false;
    }

    if (saturationEmptyNotifications) {
        float saturation = 0.0f;
        if (playerSaturation(player, saturation)) {
            const float previous = g_lastSaturation;
            if (std::isnan(previous)) {
                // First valid sample is a baseline, not a state transition. If the
                // module is enabled while already empty, wait for recovery before
                // allowing a future depletion notification.
                g_lastSaturation = saturation;
                g_saturationAlerted = saturation <= kSaturationEmptyThreshold;
            } else {
                if (!g_saturationAlerted && saturation <= kSaturationEmptyThreshold
                    && previous > kSaturationEmptyThreshold) {
                    addNotification(NotificationKind::SaturationEmpty, "SATURATION EMPTY",
                        "FOOD BUFFER DEPLETED", {6.0f,4.6f,3.2f,2.0f,.8f,0.0f});
                    g_saturationAlerted = true;
                } else if (g_saturationAlerted && saturation >= kSaturationResetThreshold) {
                    g_saturationAlerted = false;
                }
                g_lastSaturation = saturation;
            }
        }
    } else {
        g_lastSaturation = std::numeric_limits<float>::quiet_NaN();
        g_saturationAlerted = false;
    }


}


void SystemNotificationsModule::notifyModuleState(std::string_view moduleName, bool poweredOn) {
    if (!enabled || !moduleNotifications || moduleName.empty() || moduleName == "System Notifications") return;
    std::string body(moduleName);
    body += poweredOn ? "  POWERED ON" : "  POWERED OFF";
    addNotification(NotificationKind::ModuleState, poweredOn ? "MODULE ENABLED" : "MODULE DISABLED",
        std::move(body), poweredOn ? std::vector<float>{.15f,.55f,.32f,.82f,.62f,1.0f}
                                  : std::vector<float>{1.0f,.72f,.84f,.42f,.55f,.18f});
}

void SystemNotificationsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    weatherNotifications = j.value("weatherNotifications", weatherNotifications);
    timeNotifications = j.value("timeNotifications", timeNotifications);
    levelNotifications = j.value("levelNotifications", levelNotifications);
    slimeChunkNotifications = j.value("slimeChunkNotifications", slimeChunkNotifications);
    sleepDeprivedNotifications = j.value("sleepDeprivedNotifications", sleepDeprivedNotifications);
    hpLowNotifications = j.value("hpLowNotifications", hpLowNotifications);
    dimensionNotifications = j.value("dimensionNotifications", dimensionNotifications);
    dayRolloverNotifications = j.value("dayRolloverNotifications", dayRolloverNotifications);
    hungerLowNotifications = j.value("hungerLowNotifications", hungerLowNotifications);
    saturationEmptyNotifications = j.value("saturationEmptyNotifications", saturationEmptyNotifications);
    levelDownNotifications = j.value("levelDownNotifications", levelDownNotifications);
    healthRecoveredNotifications = j.value("healthRecoveredNotifications", healthRecoveredNotifications);
    moduleNotifications = j.value("moduleNotifications", moduleNotifications);
    slimeChunkGrid = j.value("slimeChunkGrid", slimeChunkGrid);
    // Migrate the previous height slider to the new Ground <-> Full Height
    // blend: any positive legacy height starts in Full Height mode.
    slimeChunkGridMode = std::clamp(j.value("slimeChunkGridMode",
        j.value("slimeChunkGridHeight", 0) > 0 ? 100 : slimeChunkGridMode), 0, 100);
    slimeChunkGridDuration = std::clamp(j.value("slimeChunkGridDuration", slimeChunkGridDuration), 3.0f, 60.0f);
    notificationDuration = std::clamp(j.value("notificationDuration", notificationDuration), 1.0f, 10.0f);
    animationSpeed = std::clamp(j.value("animationSpeed", animationSpeed), .5f, 1.5f);
}

void SystemNotificationsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["weatherNotifications"] = weatherNotifications;
    j["timeNotifications"] = timeNotifications;
    j["levelNotifications"] = levelNotifications;
    j["slimeChunkNotifications"] = slimeChunkNotifications;
    j["sleepDeprivedNotifications"] = sleepDeprivedNotifications;
    j["hpLowNotifications"] = hpLowNotifications;
    j["dimensionNotifications"] = dimensionNotifications;
    j["dayRolloverNotifications"] = dayRolloverNotifications;
    j["hungerLowNotifications"] = hungerLowNotifications;
    j["saturationEmptyNotifications"] = saturationEmptyNotifications;
    j["levelDownNotifications"] = levelDownNotifications;
    j["healthRecoveredNotifications"] = healthRecoveredNotifications;
    j["moduleNotifications"] = moduleNotifications;
    j["slimeChunkGrid"] = slimeChunkGrid;
    j["slimeChunkGridMode"] = std::clamp(slimeChunkGridMode, 0, 100);
    j["slimeChunkGridDuration"] = std::clamp(slimeChunkGridDuration, 3.0f, 60.0f);
    j["notificationDuration"] = notificationDuration;
    j["animationSpeed"] = animationSpeed;
}
