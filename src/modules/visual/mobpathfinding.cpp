#include "mobpathfinding.hpp"

#include "core/memory/Hooks.hpp"
#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Vec3 = worldanalysis::sdk::Vec3;
using Segment = worldanalysis::worldoverlay::Segment;
using Clock = std::chrono::steady_clock;

using NavTickFn = void (*)(void*, void*, void*);
// PathNavigation*, NavigationComponent*, Mob*, unique_ptr<Path>*, speed.
// AArch64 carries the final float in s0 while the four pointers use x0-x3.
using AssignPathFn = bool (*)(void*, void*, void*, void*, float);
using NavigationGetPathFn = void* (*)(void*);
using PathGetNodeCountFn = std::size_t (*)(void*);
using PathGetCurrentIndexFn = std::size_t (*)(void*);
using PathGetPosAtNodeFn = Vec3 (*)(void*, void*, std::size_t);

constexpr std::size_t kNavigationPathOffset = 0x58;
constexpr std::size_t kMaxPathNodes = 192;
constexpr std::size_t kMaxTrackedMobs = 192;
constexpr float kRouteExpirySeconds = 2.0f;
constexpr float kMinRange = 4.0f;
constexpr float kMaxRange = 128.0f;
constexpr float kLineAnimationSeconds = 0.50f;
constexpr float kTextFadeSeconds = 0.10f;
constexpr float kHalfThickness = 0.0075f;
constexpr float kEndpointRadius = 0.075f;
constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint32_t kMobCategory = 0x2;
constexpr std::uint32_t kHostileCategory = 0x4;

enum class RouteKind : std::uint8_t {
    Hostile,
    Normal,
    Other,
};

struct Route {
    std::vector<Vec3> points;
    Vec3 destination{};
    RouteKind kind = RouteKind::Other;
    std::uint64_t fingerprint = 0;
    Clock::time_point acquiredAt{};
    Clock::time_point updatedAt{};
};

MobPathfindingModule* g_mod = nullptr;
NavigationGetPathFn g_getPath = nullptr;
PathGetNodeCountFn g_getNodeCount = nullptr;
PathGetCurrentIndexFn g_getCurrentIndex = nullptr;
PathGetPosAtNodeFn g_getPosAtNode = nullptr;

AssignPathFn g_assignPathOriginal = nullptr;
NavTickFn g_baseTickOriginal = nullptr;
NavTickFn g_genericTickOriginal = nullptr;
NavTickFn g_flyingTickOriginal = nullptr;
NavTickFn g_hoverTickOriginal = nullptr;
NavTickFn g_wallTickOriginal = nullptr;
NavTickFn g_waterTickOriginal = nullptr;

void* g_assignPathTarget = nullptr;
void* g_baseTickTarget = nullptr;
void* g_genericTickTarget = nullptr;
void* g_flyingTickTarget = nullptr;
void* g_hoverTickTarget = nullptr;
void* g_wallTickTarget = nullptr;
void* g_waterTickTarget = nullptr;

bool g_assignPathPatched = false;
bool g_baseTickPatched = false;
bool g_genericTickPatched = false;
bool g_flyingTickPatched = false;
bool g_hoverTickPatched = false;
bool g_wallTickPatched = false;
bool g_waterTickPatched = false;

std::unordered_map<std::uintptr_t, Route> g_routes;
std::mutex g_routesMutex;

bool plausible(std::uintptr_t p, std::size_t alignment = alignof(void*)) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    constexpr std::uintptr_t kAddressMask = 0x00FFFFFFFFFFFFFFULL;
    constexpr std::uintptr_t kMaxUserAddress = 0x0010000000000000ULL;
    const std::uintptr_t address = p & kAddressMask;
    if (address < 0x10000 || address >= kMaxUserAddress
        || (alignment > 1 && (address & (alignment - 1)))) return false;
#else
    if (p < 0x10000 || (alignment > 1 && (p & (alignment - 1)))) return false;
#endif
    return true;
}

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 3e7f && std::abs(p.z) < 3e7f
        && p.y > -1024.f && p.y < 4096.f;
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float length(const Vec3& v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
float distanceSq(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return d.x*d.x+d.y*d.y+d.z*d.z;
}
Vec3 normalize(const Vec3& v, const Vec3& fallback = {1,0,0}) {
    const float l = length(v);
    return l > .0001f ? mul(v, 1.f/l) : fallback;
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float smoothStep(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x*x*(3.f-2.f*x);
}
std::uint32_t withAlpha(std::uint32_t color, float alpha) {
    const auto baseAlpha = static_cast<float>((color >> 24) & 0xFF);
    const auto scaled = static_cast<std::uint32_t>(
        std::clamp(baseAlpha * std::clamp(alpha, 0.f, 1.f), 0.f, 255.f) + .5f);
    return (scaled << 24) | (color & 0x00FFFFFFu);
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h<<6) + (h>>2);
    return h;
}
std::uint64_t routeFingerprint(std::size_t count, const Vec3& destination) {
    std::uint64_t h = static_cast<std::uint64_t>(count);
    h = mix(h, static_cast<std::uint32_t>(std::lround(destination.x*8.f)));
    h = mix(h, static_cast<std::uint32_t>(std::lround(destination.y*8.f)));
    return mix(h, static_cast<std::uint32_t>(std::lround(destination.z*8.f)));
}

RouteKind routeKind(void* mob) {
    if (!mob) return RouteKind::Other;
    const auto categories =
        reinterpret_cast<worldanalysis::sdk::Actor*>(mob)->categories();
    if ((categories & kHostileCategory) != 0) return RouteKind::Hostile;
    if ((categories & kMobCategory) != 0) return RouteKind::Normal;
    return RouteKind::Other;
}

Vec3 actorStart(void* mob, const Vec3& fallback) {
    if (!mob || !plausible(reinterpret_cast<std::uintptr_t>(mob))) return fallback;
    const Vec3 p = reinterpret_cast<worldanalysis::sdk::Actor*>(mob)->position();
    if (!finite(p) || distanceSq(p, fallback) > 64.f*64.f) return fallback;
    return {p.x,p.y+.10f,p.z};
}

// Snapshot a route without deleting an older valid snapshot when the native
// tick has just consumed its final node. Stale snapshots expire naturally.
bool captureRoute(void* navigationComponent, void* mob) {
    if (!g_mod || !g_mod->enabled || !navigationComponent || !mob
        || !plausible(reinterpret_cast<std::uintptr_t>(navigationComponent))
        || !plausible(reinterpret_cast<std::uintptr_t>(mob))
        || !g_getNodeCount || !g_getCurrentIndex || !g_getPosAtNode) return false;

    // NavigationComponent::getPath() in 26.45 is
    // `ldr x0, [x0,#0x58]; ret`. Keep +0x58 only as a signature fallback.
    void* path = g_getPath ? g_getPath(navigationComponent)
        : *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(navigationComponent)+kNavigationPathOffset);
    if (!path || !plausible(reinterpret_cast<std::uintptr_t>(path))) return false;

    const std::size_t count = g_getNodeCount(path);
    if (count == 0 || count > kMaxPathNodes) return false;
    const std::size_t current = g_getCurrentIndex(path);
    if (current >= count) return false;

    std::vector<Vec3> nodes;
    nodes.reserve(count-current+1);
    for (std::size_t i=current; i<count; ++i) {
        // Path::getPosAtNode is actor-aware in 26.45; the Mob argument is
        // required for the adjusted world-space node position.
        const Vec3 p = g_getPosAtNode(path, mob, i);
        if (!finite(p)) return false;
        if (nodes.empty() || distanceSq(nodes.back(), p) > .0025f)
            nodes.push_back(p);
    }
    if (nodes.empty()) return false;

    const Vec3 start = actorStart(mob, nodes.front());
    if (distanceSq(start, nodes.front()) > .0025f)
        nodes.insert(nodes.begin(), start);
    if (nodes.size() < 2) return false;

    Route next{};
    next.points = std::move(nodes);
    next.destination = next.points.back();
    next.kind = routeKind(mob);
    next.fingerprint = routeFingerprint(count, next.destination);
    next.updatedAt = Clock::now();

    const auto key = reinterpret_cast<std::uintptr_t>(mob);
    std::lock_guard lock(g_routesMutex);
    auto existing = g_routes.find(key);
    if (existing != g_routes.end()
        && existing->second.fingerprint == next.fingerprint
        && existing->second.acquiredAt.time_since_epoch().count() != 0)
        next.acquiredAt = existing->second.acquiredAt;
    else
        next.acquiredAt = next.updatedAt;
    g_routes[key] = std::move(next);

    if (g_routes.size() > kMaxTrackedMobs) {
        const auto now = Clock::now();
        for (auto it=g_routes.begin(); it!=g_routes.end();) {
            if (std::chrono::duration<float>(now-it->second.updatedAt).count()
                > kRouteExpirySeconds)
                it = g_routes.erase(it);
            else
                ++it;
        }
    }
    return true;
}

void runNavigationTick(NavTickFn original, void* self,
                       void* navigationComponent, void* mob) {
    if (!g_mod || !g_mod->enabled) {
        if (original) original(self, navigationComponent, mob);
        return;
    }
    // Capture on both sides. Some navigation families clear or advance the
    // current Path during tick, which made an after-only capture permanently
    // miss short paths.
    captureRoute(navigationComponent, mob);
    if (original) original(self, navigationComponent, mob);
    // Assignment can occur and be consumed inside original(). The common
    // assign hook publishes that short-lived route, so do not erase it merely
    // because neither edge of this tick still owns the Path.
    captureRoute(navigationComponent, mob);
}

void baseTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_baseTickOriginal, self, nav, mob);
}
void genericTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_genericTickOriginal, self, nav, mob);
}
void flyingTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_flyingTickOriginal, self, nav, mob);
}
void hoverTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_hoverTickOriginal, self, nav, mob);
}
void wallTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_wallTickOriginal, self, nav, mob);
}
void waterTickHook(void* self, void* nav, void* mob) {
    runNavigationTick(g_waterTickOriginal, self, nav, mob);
}

bool assignPathHook(void* self, void* navigationComponent, void* mob,
                    void* incomingPath, float speed) {
    const bool assigned = g_assignPathOriginal
        ? g_assignPathOriginal(self, navigationComponent, mob, incomingPath, speed)
        : false;
    // This common assignment point covers navigation subclasses whose tick
    // override changes in a point release. Tick hooks then keep it refreshed.
    if (g_mod && g_mod->enabled) captureRoute(navigationComponent, mob);
    return assigned;
}

void appendThick(std::vector<Segment>& out, const Vec3& a, const Vec3& b,
                 const Vec3& camera, float halfWidth) {
    const Vec3 direction = normalize(sub(b, a), {0,1,0});
    const Vec3 midpoint = mul(add(a, b), .5f);
    const Vec3 perpendicular = normalize(
        cross(direction, normalize(sub(camera, midpoint), {0,0,1})), {1,0,0});
    for (float weight : {-2.f,-1.f,0.f,1.f,2.f}) {
        const Vec3 offset = mul(perpendicular, halfWidth*weight*.5f);
        out.push_back({add(a, offset), add(b, offset)});
    }
}

void appendRing(std::vector<Segment>& out, const Vec3& center,
                const Vec3& camera, float radius = kEndpointRadius) {
    constexpr int sides = 20;
    const Vec3 toCamera = normalize(
        {camera.x-center.x,0,camera.z-center.z}, {0,0,1});
    const Vec3 right = normalize({toCamera.z,0,-toCamera.x}, {1,0,0});
    for (int i=0; i<sides; ++i) {
        const float a = 2.f*kPi*static_cast<float>(i)/sides;
        const float b = 2.f*kPi*static_cast<float>(i+1)/sides;
        const Vec3 p0 = add(center, add(
            mul(right, std::cos(a)*radius), Vec3{0,std::sin(a)*radius,0}));
        const Vec3 p1 = add(center, add(
            mul(right, std::cos(b)*radius), Vec3{0,std::sin(b)*radius,0}));
        appendThick(out, p0, p1, camera, kHalfThickness*.75f);
    }
}

std::vector<Segment> visibleRoute(const std::vector<Vec3>& points,
                                  float progress, const Vec3& camera,
                                  Vec3& revealEnd) {
    std::vector<Segment> out;
    if (points.size() < 2) {
        revealEnd = points.empty() ? Vec3{} : points.front();
        return out;
    }
    std::vector<float> lengths(points.size()-1);
    float total = 0.f;
    for (std::size_t i=1; i<points.size(); ++i) {
        lengths[i-1] = length(sub(points[i], points[i-1]));
        total += lengths[i-1];
    }
    if (total <= .0001f) {
        revealEnd = points.back();
        return out;
    }

    float remaining = total*std::clamp(progress, 0.f, 1.f);
    revealEnd = points.front();
    out.reserve(points.size()*8+48);
    appendRing(out, points.front(), camera);
    for (std::size_t i=1; i<points.size() && remaining>0.f; ++i) {
        const float segmentLength = lengths[i-1];
        if (segmentLength <= .0001f) continue;
        const float fraction = std::min(1.f, remaining/segmentLength);
        const Vec3 end = add(
            points[i-1], mul(sub(points[i], points[i-1]), fraction));
        appendThick(out, points[i-1], end, camera, kHalfThickness);
        revealEnd = end;
        remaining -= segmentLength;
        if (fraction < 1.f) break;
    }
    if (progress > .01f)
        appendRing(out, revealEnd, camera,
                   kEndpointRadius*std::min(1.f, .30f+progress));
    return out;
}

std::string destinationLabel(const Vec3& p) {
    char text[96]{};
    std::snprintf(text, sizeof(text), "DESTINATION X %d Y %d Z %d",
                  static_cast<int>(std::lround(p.x)),
                  static_cast<int>(std::lround(p.y)),
                  static_cast<int>(std::lround(p.z)));
    return text;
}

bool routeStyle(const Route& route, std::uint32_t& color) {
    if (!g_mod) return false;
    switch (route.kind) {
    case RouteKind::Hostile:
        color = g_mod->hostilePathColor;
        return g_mod->hostilePathEnabled;
    case RouteKind::Normal:
        color = g_mod->normalPathColor;
        return g_mod->normalPathEnabled;
    case RouteKind::Other:
        color = g_mod->otherPathColor;
        return g_mod->otherPathEnabled;
    }
    return false;
}

// This is a worldoverlay callback, not a native detour. The shared dispatcher
// calls Minecraft's original RenderLevel once before invoking every overlay.
void renderLevelHook(void* self, void* screen, void* a3) {
    (void)a3;
    if (!g_mod || !g_mod->enabled) return;

    worldanalysis::worldoverlay::RenderContext context;
    if (!worldanalysis::worldoverlay::makeContext(self, screen, context)) return;
    void* material = worldanalysis::worldoverlay::depthMaterial();
    if (!material) return;

    std::vector<Route> routes;
    const auto now = Clock::now();
    {
        std::lock_guard lock(g_routesMutex);
        for (auto it=g_routes.begin(); it!=g_routes.end();) {
            if (std::chrono::duration<float>(now-it->second.updatedAt).count()
                > kRouteExpirySeconds) {
                it = g_routes.erase(it);
                continue;
            }
            routes.push_back(it->second);
            ++it;
        }
    }

    const float range = std::clamp(g_mod->pathRange, kMinRange, kMaxRange);
    const float rangeSq = range*range;
    for (const auto& route : routes) {
        std::uint32_t color = 0xFFFFFFFFu;
        if (!routeStyle(route, color) || route.points.size() < 2
            || distanceSq(route.points.front(), context.camera) > rangeSq) continue;

        const float elapsed = std::max(
            0.f, std::chrono::duration<float>(now-route.acquiredAt).count());
        const float progress = smoothStep(elapsed/kLineAnimationSeconds);
        Vec3 revealEnd{};
        auto geometry = visibleRoute(
            route.points, progress, context.camera, revealEnd);
        worldanalysis::worldoverlay::drawLines(
            context, material, geometry, color);

        if (g_mod->destinationText && progress >= .999f) {
            const float alpha = smoothStep(
                (elapsed-kLineAnimationSeconds)/kTextFadeSeconds);
            if (alpha > 0.f) {
                worldanalysis::worldoverlay::drawBillboardText(
                    context, material, destinationLabel(route.destination),
                    add(route.destination, Vec3{0,.22f,0}), .023f,
                    withAlpha(color, alpha), false);
            }
        }
    }
}

void installTick(void* target, void* hook, NavTickFn& original, bool& patched) {
    if (!patched && target
        && worldanalysis::hooks::install(
            target, hook, reinterpret_cast<void**>(&original)))
        patched = true;
}

void installHooks() {
    if (!g_assignPathPatched && g_assignPathTarget
        && worldanalysis::hooks::install(
            g_assignPathTarget, reinterpret_cast<void*>(assignPathHook),
            reinterpret_cast<void**>(&g_assignPathOriginal)))
        g_assignPathPatched = true;
    installTick(g_baseTickTarget, reinterpret_cast<void*>(baseTickHook),
                g_baseTickOriginal, g_baseTickPatched);
    installTick(g_genericTickTarget, reinterpret_cast<void*>(genericTickHook),
                g_genericTickOriginal, g_genericTickPatched);
    installTick(g_flyingTickTarget, reinterpret_cast<void*>(flyingTickHook),
                g_flyingTickOriginal, g_flyingTickPatched);
    installTick(g_hoverTickTarget, reinterpret_cast<void*>(hoverTickHook),
                g_hoverTickOriginal, g_hoverTickPatched);
    installTick(g_wallTickTarget, reinterpret_cast<void*>(wallTickHook),
                g_wallTickOriginal, g_wallTickPatched);
    installTick(g_waterTickTarget, reinterpret_cast<void*>(waterTickHook),
                g_waterTickOriginal, g_waterTickPatched);
}

void parseColor(const nlohmann::json& j, const char* key,
                std::uint32_t& out) {
    if (!j.contains(key) || !j[key].is_string()) return;
    const auto value = j[key].get<std::string>();
    if (value.empty() || value.front() != '#') return;
    try {
        out = static_cast<std::uint32_t>(
            std::stoul(value.substr(1), nullptr, 16));
    } catch (...) {}
}

std::string formatColor(std::uint32_t color) {
    char value[12]{};
    std::snprintf(value, sizeof(value), "#%08X", color);
    return value;
}

} // namespace

MobPathfindingModule::MobPathfindingModule()
    : Module(
        "Mob Pathfinding",
        "Captures Minecraft 26.45 native mob navigation routes available in local worlds and "
        "renders depth-tested animated o-line-o paths with an optional destination label. "
        "Hostile, normal, and other route groups are independently toggleable and colorable.") {
    showInMenu = true;
    g_mod = this;
}

MobPathfindingModule::~MobPathfindingModule() {
    if (g_mod == this) g_mod = nullptr;
}

void MobPathfindingModule::onInit() {
    auto resolve = [](worldanalysis::memory::SignatureId id) -> void* {
        const auto address = worldanalysis::memory::resolve(id);
        return address ? reinterpret_cast<void*>(address) : nullptr;
    };
    g_assignPathTarget =
        resolve(worldanalysis::memory::SignatureId::PathNavigationAssignPath);
    g_baseTickTarget =
        resolve(worldanalysis::memory::SignatureId::PathNavigationTickBase);
    g_genericTickTarget =
        resolve(worldanalysis::memory::SignatureId::PathNavigationTickGeneric);
    g_flyingTickTarget =
        resolve(worldanalysis::memory::SignatureId::PathNavigationTickFlying);
    g_hoverTickTarget =
        resolve(worldanalysis::memory::SignatureId::PathNavigationTickHover);
    g_wallTickTarget = resolve(
        worldanalysis::memory::SignatureId::PathNavigationTickWallClimber);
    g_waterTickTarget = resolve(
        worldanalysis::memory::SignatureId::PathNavigationTickWaterBound);

    if (auto a = worldanalysis::memory::resolve(
            worldanalysis::memory::SignatureId::NavigationComponentGetPath))
        g_getPath = reinterpret_cast<NavigationGetPathFn>(a);
    if (auto a = worldanalysis::memory::resolve(
            worldanalysis::memory::SignatureId::PathGetNodeCount))
        g_getNodeCount = reinterpret_cast<PathGetNodeCountFn>(a);
    if (auto a = worldanalysis::memory::resolve(
            worldanalysis::memory::SignatureId::PathGetCurrentIndex))
        g_getCurrentIndex = reinterpret_cast<PathGetCurrentIndexFn>(a);
    if (auto a = worldanalysis::memory::resolve(
            worldanalysis::memory::SignatureId::PathGetPosAtNode))
        g_getPosAtNode = reinterpret_cast<PathGetPosAtNodeFn>(a);

    // Use the same shared RenderLevel owner as InStore Viewer and Entity
    // Inventory, so route rendering does not depend on native detour order.
    worldanalysis::worldoverlay::initialize();
    worldanalysis::worldoverlay::registerRenderCallback(renderLevelHook);
    installHooks();

    worldanalysis::events::bus()
        .subscribe<worldanalysis::events::LocalPlayerTickEvent>(
            [](auto& event) {
                if (g_mod) g_mod->handleTick(event.player);
            });
}

void MobPathfindingModule::onEnable() {
    worldanalysis::worldoverlay::registerRenderCallback(renderLevelHook);
    installHooks();
    std::lock_guard lock(g_routesMutex);
    g_routes.clear();
}

void MobPathfindingModule::onDisable() {
    std::lock_guard lock(g_routesMutex);
    g_routes.clear();
}

void MobPathfindingModule::handleTick(worldanalysis::sdk::Player*) {
    const auto now = Clock::now();
    std::lock_guard lock(g_routesMutex);
    for (auto it=g_routes.begin(); it!=g_routes.end();) {
        if (std::chrono::duration<float>(now-it->second.updatedAt).count()
            > kRouteExpirySeconds)
            it = g_routes.erase(it);
        else
            ++it;
    }
}

void MobPathfindingModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    hostilePathEnabled = j.value(
        "hostilePathEnabled",
        j.value("hostileMobsEnabled", hostilePathEnabled));
    normalPathEnabled = j.value("normalPathEnabled", normalPathEnabled);
    otherPathEnabled = j.value("otherPathEnabled", otherPathEnabled);
    destinationText = j.value("destinationText", destinationText);
    pathRange = std::clamp(
        j.value("pathRange", pathRange), kMinRange, kMaxRange);
    parseColor(j, "hostilePathColor", hostilePathColor);
    parseColor(j, "normalPathColor", normalPathColor);
    parseColor(j, "otherPathColor", otherPathColor);
}

void MobPathfindingModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hostilePathEnabled"] = hostilePathEnabled;
    j["hostilePathColor"] = formatColor(hostilePathColor);
    j["normalPathEnabled"] = normalPathEnabled;
    j["normalPathColor"] = formatColor(normalPathColor);
    j["otherPathEnabled"] = otherPathEnabled;
    j["otherPathColor"] = formatColor(otherPathColor);
    j["destinationText"] = destinationText;
    j["pathRange"] = std::clamp(pathRange, kMinRange, kMaxRange);
}
