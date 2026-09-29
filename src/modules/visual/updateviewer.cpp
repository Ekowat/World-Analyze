#include "updateviewer.hpp"

#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/Level.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Vec3 = worldanalysis::sdk::Vec3;
using AABB = worldanalysis::sdk::AABB;
using Clock = std::chrono::steady_clock;
using Segment = worldanalysis::worldoverlay::Segment;

struct BlockPosRaw { int x = 0, y = 0, z = 0; };
struct BlockKey {
    int x = 0, y = 0, z = 0;
    bool operator==(const BlockKey& other) const { return x == other.x && y == other.y && z == other.z; }
};
struct BlockKeyHash {
    std::size_t operator()(const BlockKey& p) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        auto mix = [&](int value) { h ^= static_cast<std::uint32_t>(value); h *= 1099511628211ULL; };
        mix(p.x); mix(p.y); mix(p.z); return static_cast<std::size_t>(h);
    }
};
struct KnownBlock { std::uintptr_t state = 0; std::string identifier; };
struct TechPulse {
    Vec3 position{};
    BlockKey block{};
    std::uint64_t serial = 0;
    Clock::time_point created{};
    Clock::time_point expires{};
};

struct Marker {
    Vec3 position{};
    BlockKey block{};
    std::uintptr_t actor = 0;
    bool isBlock = false;
    std::string label;
    std::uint64_t serial = 0;
    Clock::time_point created{};
    Clock::time_point expires{};
};

using GetBlockFn = void* (*)(void*, const BlockPosRaw&);
using RuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);

constexpr std::size_t kScanBudget = 1400;
constexpr std::size_t kMaxMarkers = 120;
constexpr int kMaxActors = 4096;
constexpr float kPi = 3.14159265358979323846f;

UpdateViewerModule* g_mod = nullptr;
GetBlockFn g_getBlock = nullptr;
RuntimeActorListFn g_actorList = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
void* g_region = nullptr;
void* g_dimension = nullptr;
BlockKey g_center{};
int g_radius = 18, g_vertical = 10, g_width = 37, g_height = 21;
std::size_t g_scanIndex = 0, g_scanTotal = 0;
std::unordered_map<BlockKey, KnownBlock, BlockKeyHash> g_knownBlocks;
std::unordered_set<std::uintptr_t> g_knownActors;
bool g_actorBaseline = false;
std::vector<Marker> g_markers;
std::vector<TechPulse> g_pulses;
std::uint64_t g_serial = 0;
std::uint32_t g_rng = 0x6D2B79F5u;
int g_ambientCountdown = 90;
std::mutex g_mutex;

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 3e7f && std::abs(p.z) < 3e7f && p.y > -1024.0f && p.y < 4096.0f;
}
bool validBounds(const AABB& b) {
    return finite(b.min) && finite(b.max) && b.max.x >= b.min.x && b.max.y > b.min.y && b.max.z >= b.min.z;
}
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float length(const Vec3& v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
Vec3 normalize(const Vec3& v, const Vec3& fallback = {1,0,0}) {
    const float l = length(v); return l > .0001f ? mul(v, 1.0f/l) : fallback;
}
float smoothStep(float v) { v = std::clamp(v, 0.0f, 1.0f); return v*v*(3.0f-2.0f*v); }
std::uint32_t white(float alpha) {
    return (static_cast<std::uint32_t>(std::clamp(alpha,0.0f,1.0f)*255.0f+.5f)<<24)|0x00FFFFFFu;
}

std::string_view blockIdentifier(const void* block) {
    if (!block) return {};
    const auto type = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(block) + worldanalysis::sdk::offsets::Block::mBlockType);
    if (!worldanalysis::worldoverlay::plausible(type)) return {};
    const auto address = type + worldanalysis::sdk::offsets::BlockType::mNameInfo
        + worldanalysis::sdk::offsets::NameInfo::mFullName
        + worldanalysis::sdk::offsets::HashedString::mString;
    const auto* value = reinterpret_cast<const std::string*>(address);
    if (value->size() > 128 || (!value->empty() && !value->data())) return {};
    return {value->data(), value->size()};
}
std::string shortId(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return std::string(id);
}
bool contains(std::string_view value, std::string_view needle) { return value.find(needle) != std::string_view::npos; }
bool isAir(std::string_view id) {
    return id.empty() || id == "minecraft:air" || id == "minecraft:cave_air" || id == "minecraft:void_air";
}
std::string displayName(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    std::string out; out.reserve(std::min<std::size_t>(id.size(), 20)); bool upper = true;
    for (char c : id) {
        if (out.size() >= 20) break;
        if (c == '_' || c == '-') { if (!out.empty() && out.back() != ' ') out.push_back(' '); upper = true; continue; }
        if (c >= 'a' && c <= 'z') { out.push_back(upper ? static_cast<char>(c-'a'+'A') : c); upper = false; }
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { out.push_back(c); upper = false; }
    }
    return out.empty() ? "BLOCK" : out;
}

std::string classifyUpdate(std::string_view oldId, std::string_view newId) {
    if (!g_mod) return {};
    const std::string oldShort = shortId(oldId), newShort = shortId(newId);
    const std::string_view combined = !newShort.empty() ? std::string_view(newShort) : std::string_view(oldShort);
    const bool redstone = contains(oldShort,"redstone")||contains(newShort,"redstone")
        ||contains(oldShort,"repeater")||contains(newShort,"repeater")
        ||contains(oldShort,"comparator")||contains(newShort,"comparator")
        ||contains(oldShort,"observer")||contains(newShort,"observer")
        ||contains(oldShort,"piston")||contains(newShort,"piston")
        ||contains(oldShort,"lever")||contains(newShort,"lever")
        ||contains(oldShort,"button")||contains(newShort,"button");
    if (redstone) return g_mod->redstoneUpdates ? "REDSTONE UPDATED" : "";
    const bool crop = contains(oldShort,"sugar_cane")||contains(newShort,"sugar_cane")
        ||contains(oldShort,"wheat")||contains(newShort,"wheat")
        ||contains(oldShort,"carrot")||contains(newShort,"carrot")
        ||contains(oldShort,"potato")||contains(newShort,"potato")
        ||contains(oldShort,"beetroot")||contains(newShort,"beetroot")
        ||contains(oldShort,"cocoa")||contains(newShort,"cocoa")
        ||contains(oldShort,"stem")||contains(newShort,"stem")
        ||contains(oldShort,"sapling")||contains(newShort,"sapling");
    if (crop) {
        if (!g_mod->cropUpdates) return {};
        if (contains(oldShort,"sugar_cane") && isAir(newId)) return "SUGAR CANE BROKE";
        return isAir(newId) ? "CROP BROKE" : "CROP UPDATED";
    }
    const bool fluid = contains(oldShort,"water")||contains(newShort,"water")
        ||contains(oldShort,"lava")||contains(newShort,"lava");
    if (fluid) {
        if (!g_mod->fluidUpdates) return {};
        return contains(combined,"lava") ? "LAVA UPDATED" : "WATER UPDATED";
    }
    if (!g_mod->blockUpdates) return {};
    if (isAir(oldId) && !isAir(newId)) return "BLOCK PLACED " + displayName(newId);
    if (!isAir(oldId) && isAir(newId)) return "BLOCK BROKE " + displayName(oldId);
    return "BLOCK UPDATED " + displayName(combined);
}

void addMarker(const Vec3& position, const BlockKey& block, std::uintptr_t actor, bool isBlock, std::string label) {
    if (!g_mod || label.empty() || !finite(position)) return;
    const auto now = Clock::now();
    const auto expires = now + std::chrono::milliseconds(static_cast<int>(std::clamp(g_mod->duration,.5f,10.f)*1000.f));
    std::lock_guard lock(g_mutex);
    if (isBlock) {
        for (auto& marker : g_markers) if (marker.isBlock && marker.block == block) {
            marker.position = position; marker.label = std::move(label); marker.created = now; marker.expires = expires; return;
        }
    }
    if (g_markers.size() >= kMaxMarkers) g_markers.erase(g_markers.begin());
    g_markers.push_back({position, block, actor, isBlock,
        worldanalysis::worldoverlay::sanitizeText(label, 36), ++g_serial, now, expires});
}

void resetScanner(void* region, void* dimension, const Vec3& position, int radius) {
    const bool worldChanged = (g_dimension && g_dimension != dimension) || (g_region && g_region != region);
    g_region=region; g_dimension=dimension;
    g_center={static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)),static_cast<int>(std::floor(position.z))};
    g_radius=radius; g_vertical=std::clamp(radius/2,6,14); g_width=g_radius*2+1; g_height=g_vertical*2+1;
    g_scanTotal=static_cast<std::size_t>(g_width)*g_width*g_height; g_scanIndex=0;
    g_knownBlocks.clear();
    // Re-centering the block scanner must never reset actor provenance. Doing so
    // made a pre-existing mob become "new" merely by walking into scan range.
    if (worldChanged) { g_knownActors.clear(); g_actorBaseline=false; }
}

void scanBlocks() {
    if (!g_region || !g_getBlock || !g_scanTotal) return;
    std::size_t scanned=0;
    while (scanned++ < kScanBudget) {
        if (g_scanIndex >= g_scanTotal) g_scanIndex=0;
        const std::size_t index=g_scanIndex++, xi=index%static_cast<std::size_t>(g_width),
            yz=index/static_cast<std::size_t>(g_width), zi=yz%static_cast<std::size_t>(g_width), yi=yz/static_cast<std::size_t>(g_width);
        const BlockKey key{g_center.x+static_cast<int>(xi)-g_radius,
            g_center.y+static_cast<int>(yi)-g_vertical,g_center.z+static_cast<int>(zi)-g_radius};
        const BlockPosRaw pos{key.x,key.y,key.z};
        void* block=g_getBlock(g_region,pos); const std::uintptr_t state=reinterpret_cast<std::uintptr_t>(block);
        const std::string id(blockIdentifier(block));
        const auto [it,inserted]=g_knownBlocks.emplace(key,KnownBlock{state,id});
        if (!inserted && (it->second.state!=state || it->second.identifier!=id)) {
            const std::string label=classifyUpdate(it->second.identifier,id); it->second={state,id};
            if (!label.empty()) addMarker({key.x+.5f,key.y+.64f,key.z+.5f},key,0,true,label);
        }
        if (g_scanIndex>=g_scanTotal) break;
    }
}

void scanActors(worldanalysis::sdk::Player* player) {
    if (!g_mod || !g_mod->mobSpawns || !g_actorList || !player) return;
    auto* level=player->level(); if (!level) return; void* manager=level->actorManager(); if (!manager) return;
    const auto actors=g_actorList(manager);
    // If the actor manager is unexpectedly huge, preserve the previous baseline
    // instead of replacing it with a truncated set that would cause false spawns.
    if (actors.size()>static_cast<std::size_t>(kMaxActors)) return;
    std::unordered_set<std::uintptr_t> current; current.reserve(actors.size());
    const Vec3 local=player->position(); const float radius=std::clamp(g_mod->eventRadius,6.f,32.f), radiusSq=radius*radius;
    for (void* raw:actors) {
        if (!raw || raw==player) continue;
        const auto key=reinterpret_cast<std::uintptr_t>(raw); if (!worldanalysis::worldoverlay::plausible(key)) continue;
        const std::string type=worldanalysis::worldoverlay::actorTypeName(raw);
        if ((g_actorIsPlayer&&g_actorIsPlayer(raw)) || type.find("Player")!=std::string::npos) continue;
        auto* actor=reinterpret_cast<worldanalysis::sdk::Actor*>(raw);
        if ((actor->categories()&(0x2u|0x4u))==0) continue;
        const Vec3 position=actor->position(); if (!finite(position)) continue;
        // Track every mob currently known by the level, independent of display
        // radius. Radius only decides whether a genuinely new actor gets a marker.
        current.insert(key);
        const bool newlyInserted = g_actorBaseline && g_knownActors.find(key)==g_knownActors.end();
        if (!newlyInserted) continue;
        const Vec3 delta=sub(position,local);
        if (delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>radiusSq) continue;
        const AABB bounds=actor->bounds();
        const Vec3 marker=validBounds(bounds)?Vec3{(bounds.min.x+bounds.max.x)*.5f,bounds.max.y+.08f,(bounds.min.z+bounds.max.z)*.5f}:add(position,{0,1.f,0});
        addMarker(marker,{},key,false,"MOB SPAWNED "+(type.empty()?std::string("ENTITY"):type));
    }
    g_knownActors=std::move(current); g_actorBaseline=true;
}

std::uint32_t nextRandom(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
void maybeSpawnTechPulse(const Vec3& local){
    if(!g_mod||!g_mod->ambientTechAnimations||!g_region||!g_getBlock)return;
    if(--g_ambientCountdown>0)return;
    g_ambientCountdown=100+static_cast<int>(nextRandom()%160u);
    const int radius=std::clamp(static_cast<int>(std::lround(g_mod->eventRadius)),6,24);
    static constexpr int dirs[6][3]{{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    for(int attempt=0;attempt<40;++attempt){
        BlockKey p{static_cast<int>(std::floor(local.x))+static_cast<int>(nextRandom()%static_cast<unsigned>(radius*2+1))-radius,
                   static_cast<int>(std::floor(local.y))+static_cast<int>(nextRandom()%13u)-6,
                   static_cast<int>(std::floor(local.z))+static_cast<int>(nextRandom()%static_cast<unsigned>(radius*2+1))-radius};
        void*b=g_getBlock(g_region,{p.x,p.y,p.z});const auto id=blockIdentifier(b);if(isAir(id))continue;
        Vec3 normal{};bool exposed=false;float best=-1e9f;const Vec3 center{p.x+.5f,p.y+.5f,p.z+.5f};
        const Vec3 towardPlayer{sub(local,center)};
        for(const auto&d:dirs){void*n=g_getBlock(g_region,{p.x+d[0],p.y+d[1],p.z+d[2]});if(!isAir(blockIdentifier(n)))continue;const Vec3 candidate{static_cast<float>(d[0]),static_cast<float>(d[1]),static_cast<float>(d[2])};const float score=towardPlayer.x*candidate.x+towardPlayer.y*candidate.y+towardPlayer.z*candidate.z;if(score>best){best=score;normal=candidate;exposed=true;}}
        if(!exposed)continue;const Vec3 pos{p.x+.5f+normal.x*.53f,p.y+.5f+normal.y*.53f,p.z+.5f+normal.z*.53f};
        bool collides=false;{std::lock_guard lock(g_mutex);for(const auto&pulse:g_pulses){const Vec3 d=sub(pos,pulse.position);if(d.x*d.x+d.y*d.y+d.z*d.z<12.f){collides=true;break;}}if(!collides){const auto now=Clock::now();g_pulses.push_back({pos,p,++g_serial,now,now+std::chrono::milliseconds(2800)});if(g_pulses.size()>3)g_pulses.erase(g_pulses.begin());}}
        if(!collides)return;
    }
}

void appendRing(std::vector<Segment>& out,const Vec3& center,const Vec3& right,float radius) {
    constexpr int sides=20;
    for(int i=0;i<sides;++i){const float a=2.f*kPi*i/sides,b=2.f*kPi*(i+1)/sides;
        out.push_back({add(center,add(mul(right,std::cos(a)*radius),Vec3{0,std::sin(a)*radius,0})),
                       add(center,add(mul(right,std::cos(b)*radius),Vec3{0,std::sin(b)*radius,0}))});}
}

void renderEvents(void* self, void* screen, void* a3) {
    (void)a3; if (!g_mod || !g_mod->enabled) return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color);
    worldanalysis::worldoverlay::RenderContext context;
    if (!worldanalysis::worldoverlay::makeContext(self,screen,context)) return;
    // Update Viewer is intentionally never visible through blocks.
    void* material=worldanalysis::worldoverlay::depthMaterial(); if (!material) return;
    std::vector<Marker> markers; std::vector<TechPulse> pulses; { std::lock_guard lock(g_mutex); markers=g_markers; pulses=g_pulses; }
    const auto now=Clock::now();
    for (const auto& marker:markers) {
        if (marker.expires<=now) continue;
        const float age=std::chrono::duration<float>(now-marker.created).count(),
            remaining=std::chrono::duration<float>(marker.expires-now).count();
        const float intro=smoothStep(age/.38f),fade=smoothStep(remaining/.34f),alpha=intro*fade; if(alpha<=.001f) continue;
        const Vec3 start=marker.position,toCamera=normalize({context.camera.x-start.x,0,context.camera.z-start.z},{0,0,1}),
            right=normalize({toCamera.z,0,-toCamera.x},{1,0,0});
        const int side=(marker.serial&1u)?1:-1; const float lane=static_cast<float>((marker.serial/2u)%4u);
        const float horizontal=marker.isBlock?.90f+lane*.12f:1.30f+lane*.12f,
            vertical=marker.isBlock?.52f+lane*.16f:1.02f+lane*.16f;
        const Vec3 fullEnd=add(start,add(mul(right,static_cast<float>(side)*horizontal),Vec3{0,vertical,0})),
            end=add(start,mul(sub(fullEnd,start),intro));
        std::vector<Segment> callout{{start,end}}; appendRing(callout,start,right,.055f); if(intro>.02f) appendRing(callout,end,right,.055f*std::min(1.f,.3f+intro));
        worldanalysis::worldoverlay::drawLines(context,material,callout,white(alpha),2.2f);
        Vec3 textRight{},textUp{}; worldanalysis::worldoverlay::billboardBasis(fullEnd,context.camera,textRight,textUp);
        const float fitted=worldanalysis::worldoverlay::fitBillboardTextPixelSize(marker.label,1.45f,.0175f,.0080f);
        worldanalysis::worldoverlay::drawBillboardTextOriented(context,material,marker.label,
            add(fullEnd,mul(textUp,.17f)),textRight,textUp,fitted,white(alpha*smoothStep((intro-.55f)/.45f)),false);
    }
    for(const auto& pulse:pulses){
        if(pulse.expires<=now)continue;const float age=std::chrono::duration<float>(now-pulse.created).count(),remaining=std::chrono::duration<float>(pulse.expires-now).count();const float alpha=smoothStep(age/.22f)*smoothStep(remaining/.30f);if(alpha<=.001f)continue;
        Vec3 right{},up{};worldanalysis::worldoverlay::billboardBasis(pulse.position,context.camera,right,up);const float phase=age*4.6f+static_cast<float>(pulse.serial%7u);
        std::vector<Segment> tech;const float h=.28f,w=.40f,scan=-h+std::fmod(age*.33f,h*2.f);auto q=[&](float x,float y){return add(pulse.position,add(mul(right,x),mul(up,y)));};
        tech.insert(tech.end(),{{q(-w,-h),q(-w+.12f,-h)},{q(-w,-h),q(-w,-h+.12f)},{q(w,-h),q(w-.12f,-h)},{q(w,-h),q(w,-h+.12f)},{q(-w,h),q(-w+.12f,h)},{q(-w,h),q(-w,h-.12f)},{q(w,h),q(w-.12f,h)},{q(w,h),q(w,h-.12f)},{q(-w*.88f,scan),q(w*.88f,scan)}});
        for(int i=0;i<6;++i){const float a=phase+i*(2.f*kPi/6.f);const Vec3 a0=q(std::cos(a)*.18f,std::sin(a)*.10f),a1=q(std::cos(a)*.25f,std::sin(a)*.14f);tech.push_back({a0,a1});}
        worldanalysis::worldoverlay::drawLines(context,material,tech,white(alpha),1.5f);
        const std::array<std::string,3> text{{"> TRACE BLOCK","> HASH "+std::to_string(static_cast<unsigned>((pulse.block.x*73856093u)^(pulse.block.y*19349663u)^(pulse.block.z*83492791u))),"> SYNC OK"}};
        for(std::size_t i=0;i<text.size();++i){if(age<.35f+.34f*i)continue;worldanalysis::worldoverlay::drawBillboardTextOriented(context,material,text[i],add(pulse.position,add(mul(right,.54f),mul(up,.18f-static_cast<float>(i)*.12f))),right,up,.009f,white(alpha),false);}
    }
}

void clearState() {
    g_region=nullptr; g_dimension=nullptr; g_scanIndex=g_scanTotal=0; g_knownBlocks.clear(); g_knownActors.clear(); g_actorBaseline=false;
    std::lock_guard lock(g_mutex); g_markers.clear(); g_pulses.clear(); g_ambientCountdown=90;
}
} // namespace

UpdateViewerModule::UpdateViewerModule()
    : Module("Update Viewer", "Depth-tested client-side callouts for useful nearby world changes: block/redstone/crop/fluid updates and newly observed mob spawns, plus an optional ambient technical trace animation on exposed nearby blocks. Nothing in this module renders through blocks.") {
    g_mod=this; showInMenu=true;
}
UpdateViewerModule::~UpdateViewerModule(){if(g_mod==this)g_mod=nullptr;}
void UpdateViewerModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorManagerList))g_actorList=reinterpret_cast<RuntimeActorListFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorIsPlayer))g_actorIsPlayer=reinterpret_cast<ActorIsPlayerFn>(a);
    worldanalysis::worldoverlay::initialize(); worldanalysis::worldoverlay::registerRenderCallback(renderEvents);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});
}
void UpdateViewerModule::onEnable(){clearState();worldanalysis::worldoverlay::registerRenderCallback(renderEvents);}
void UpdateViewerModule::onDisable(){clearState();}
void UpdateViewerModule::handleTick(worldanalysis::sdk::Player* player){
    if(!enabled||!player||!g_getBlock)return; const Vec3 position=player->position(); if(!finite(position))return;
    auto* dimension=player->dimension(); void* region=dimension?dimension->blockSource():nullptr; if(!region)return;
    const int radius=static_cast<int>(std::lround(std::clamp(eventRadius,6.f,32.f)));
    const BlockKey nowPos{static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)),static_cast<int>(std::floor(position.z))};
    const int recenterDistance=std::max(4,radius/3);
    const bool recenter=!g_region||g_region!=region||g_dimension!=dimension||radius!=g_radius
        ||std::abs(nowPos.x-g_center.x)>recenterDistance||std::abs(nowPos.y-g_center.y)>recenterDistance||std::abs(nowPos.z-g_center.z)>recenterDistance;
    if(recenter)resetScanner(region,dimension,position,radius); scanBlocks(); scanActors(player); maybeSpawnTechPulse(position);
    const auto now=Clock::now(); std::lock_guard lock(g_mutex);
    g_markers.erase(std::remove_if(g_markers.begin(),g_markers.end(),[&](const Marker&m){return m.expires<=now;}),g_markers.end());
    g_pulses.erase(std::remove_if(g_pulses.begin(),g_pulses.end(),[&](const TechPulse&p){return p.expires<=now;}),g_pulses.end());
}
void UpdateViewerModule::loadConfig(const nlohmann::json&j){
    Module::loadConfig(j);blockUpdates=j.value("blockUpdates",blockUpdates);redstoneUpdates=j.value("redstoneUpdates",redstoneUpdates);
    cropUpdates=j.value("cropUpdates",cropUpdates);fluidUpdates=j.value("fluidUpdates",fluidUpdates);mobSpawns=j.value("mobSpawns",mobSpawns);ambientTechAnimations=j.value("ambientTechAnimations",ambientTechAnimations);
    duration=std::clamp(j.value("duration",duration),.5f,10.f);eventRadius=std::clamp(j.value("eventRadius",eventRadius),6.f,32.f);
}
void UpdateViewerModule::saveConfig(nlohmann::json&j){
    Module::saveConfig(j);j["blockUpdates"]=blockUpdates;j["redstoneUpdates"]=redstoneUpdates;j["cropUpdates"]=cropUpdates;
    j["fluidUpdates"]=fluidUpdates;j["mobSpawns"]=mobSpawns;j["ambientTechAnimations"]=ambientTechAnimations;j["duration"]=duration;j["eventRadius"]=eventRadius;
}
