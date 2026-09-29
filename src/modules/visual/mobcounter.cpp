#include "mobcounter.hpp"

#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/client/ClientInstance.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/Level.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Vec2 = worldanalysis::sdk::Vec2;
using Vec3 = worldanalysis::sdk::Vec3;
using AABB = worldanalysis::sdk::AABB;
using Clock = std::chrono::steady_clock;
using Segment = worldanalysis::worldoverlay::Segment;
using Quad = worldanalysis::worldoverlay::Quad;

struct BlockPosRaw { int x = 0, y = 0, z = 0; };
struct SeenState { Clock::time_point entered{}; AABB bounds{}; std::string species; };
struct MatureMob { std::uintptr_t id = 0; AABB bounds{}; std::string species; };
struct Pulse { std::uintptr_t id = 0; AABB bounds{}; Clock::time_point created{}; std::uint32_t seed = 0; };
struct CountNotification {
    Vec3 anchor{};
    std::string species;
    int count = 0;
    int lane = 0;
    Clock::time_point created{};
    Clock::time_point expires{};
};

using RuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorHealthFn = int (*)(void*);
using SolidFn = bool (*)(void*, const BlockPosRaw*);
using GetFovFn = float (*)(void*, float, bool);

MobCounterModule* g_mod = nullptr;
RuntimeActorListFn g_actorList = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorHealthFn g_getMaxHealth = nullptr;
SolidFn g_isSolid = nullptr;
GetFovFn g_getFov = nullptr;
std::unordered_map<std::uintptr_t, SeenState> g_seen;
std::unordered_set<std::uintptr_t> g_counted;
std::deque<MatureMob> g_highlightQueue;
std::vector<Pulse> g_pulses;
std::unordered_map<std::string, int> g_counts;
std::vector<CountNotification> g_notifications;
std::mutex g_renderMutex;
Clock::time_point g_nextHighlight{};
std::uint32_t g_rng = 0x91E10DA5u;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDwellSeconds = 3.0f;
constexpr std::size_t kMaxNotifications = 10;
constexpr int kMaxActors = 4096;

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 3e7f && std::abs(p.z) < 3e7f && p.y > -1024.f && p.y < 4096.f;
}
bool validBounds(const AABB& b) {
    return finite(b.min) && finite(b.max) && b.max.x >= b.min.x && b.max.y > b.min.y && b.max.z >= b.min.z;
}
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
float len(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 norm(const Vec3& v, const Vec3& fallback = {0,0,1}) { const float l = len(v); return l > .0001f ? mul(v, 1.f/l) : fallback; }
float smooth(float v) { v = std::clamp(v, 0.f, 1.f); return v*v*(3.f - 2.f*v); }
std::uint32_t rng() { g_rng = g_rng*1664525u + 1013904223u; return g_rng; }
std::uint32_t white(float alpha) {
    return (static_cast<std::uint32_t>(std::clamp(alpha, 0.f, 1.f) * 255.f + .5f) << 24) | 0x00FFFFFFu;
}

Vec3 lookDirection(const Vec2& r) {
    const float pitch = r.x*kPi/180.f, yaw = r.y*kPi/180.f, cp = std::cos(pitch);
    return norm({-std::sin(yaw)*cp, -std::sin(pitch), std::cos(yaw)*cp});
}

std::string titleize(std::string_view value) {
    if (value.empty()) return "Mob";
    std::string out; out.reserve(std::min<std::size_t>(value.size()+4, 28)); bool cap = true;
    for (std::size_t i = 0; i < value.size() && out.size() < 28; ++i) {
        char c = value[i];
        if (c=='_' || c=='-' || c==':' || c==' ') { if (!out.empty() && out.back()!=' ') out.push_back(' '); cap=true; continue; }
        const bool upper = c>='A' && c<='Z';
        if (upper && !cap && !out.empty() && out.back()!=' ') out.push_back(' ');
        if (c>='a' && c<='z') { out.push_back(cap ? static_cast<char>(c-'a'+'A') : c); cap=false; }
        else if ((c>='A'&&c<='Z') || (c>='0'&&c<='9')) { out.push_back(c); cap=false; }
    }
    while (!out.empty() && out.back()==' ') out.pop_back();
    return out.empty() ? "Mob" : out;
}

std::string fallbackAnimal(const AABB& b, int maxHealth) {
    const float w = std::max(b.max.x-b.min.x, b.max.z-b.min.z), h = b.max.y-b.min.y;
    if (w>=.80f&&w<=1.05f&&h>=1.10f&&h<=1.45f) { if (maxHealth>=9) return "Cow"; if (maxHealth>=7) return "Sheep"; }
    if (w>=.35f&&w<=.62f&&h>=.45f&&h<=.86f) { if (maxHealth>=9) return "Cow"; if (maxHealth>=7) return "Sheep"; }
    if (w>=.50f&&w<=.74f&&h>=.55f&&h<=.86f&&maxHealth>=8&&maxHealth<=12) return "Fox";
    if (w>=.22f&&w<.48f&&h>=.24f&&h<.55f&&maxHealth>=8&&maxHealth<=12) return "Fox";
    if (w>=1.30f&&w<=1.55f&&h>=1.25f&&h<=1.65f&&maxHealth>=20) return "Horse";
    if (w>=.55f&&w<=.85f&&h>=.75f&&h<=1.15f&&maxHealth>=18) return "Pig";
    return {};
}

std::string speciesFor(void* raw, const AABB& bounds) {
    std::string type = worldanalysis::worldoverlay::actorTypeName(raw);
    if (type.size()>5 && type.ends_with("Actor")) type.resize(type.size()-5);
    const bool generic = type.empty() || type=="Actor" || type=="Mob" || type=="Animal" || type=="Entity" || type=="Monster";
    if (!generic) return titleize(type);
    int maxHealth = -1;
    if (g_getMaxHealth) { const int h = g_getMaxHealth(raw); if (h>=0 && h<10000) maxHealth=h; }
    if (auto animal=fallbackAnimal(bounds,maxHealth); !animal.empty()) return animal;
    if (type=="Monster") return "Monster";
    return "Mob";
}

std::array<Vec3,7> samples(const AABB& b) {
    const Vec3 c{(b.min.x+b.max.x)*.5f,(b.min.y+b.max.y)*.5f,(b.min.z+b.max.z)*.5f};
    const float inset=.12f;
    return {{c,{c.x,b.max.y-inset,c.z},{b.min.x+inset,c.y,b.min.z+inset},{b.max.x-inset,c.y,b.min.z+inset},
             {b.min.x+inset,c.y,b.max.z-inset},{b.max.x-inset,c.y,b.max.z-inset},{c.x,b.min.y+inset,c.z}}};
}

bool inView(const Vec3& eye, const Vec3& forward, const AABB& b, float halfFovRadians, float maxRange) {
    for (const auto& p : samples(b)) {
        const Vec3 d=sub(p,eye); const float distance=len(d);
        if (distance>.05f && distance<=maxRange && dot(norm(d),forward)>=std::cos(halfFovRadians)) return true;
    }
    return false;
}

bool clearTo(void* region, const Vec3& eye, const Vec3& target) {
    if (!g_isSolid || !region) return true;
    const Vec3 delta=sub(target,eye); const float distance=len(delta); if (distance<=.35f) return true;
    const Vec3 dir=mul(delta,1.f/distance); const float stop=std::max(0.f,distance-.28f);
    BlockPosRaw last{0x3fffffff,0x3fffffff,0x3fffffff};
    for (float t=.18f;t<stop;t+=.24f) {
        const Vec3 p=add(eye,mul(dir,t)); BlockPosRaw bp{static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.y)),static_cast<int>(std::floor(p.z))};
        if (bp.x==last.x&&bp.y==last.y&&bp.z==last.z) continue;
        last=bp; if (g_isSolid(region,&bp)) return false;
    }
    return true;
}

bool visibleByBlocks(void* region, const Vec3& eye, const Vec3& forward, const AABB& b, float halfFov, float range) {
    const float threshold=std::cos(halfFov);
    for (const auto& p:samples(b)) {
        const Vec3 d=sub(p,eye); const float distance=len(d);
        if (distance<=.05f || distance>range || dot(norm(d),forward)<threshold) continue;
        if (clearTo(region,eye,p)) return true;
    }
    return false;
}

float resolveHalfFov() {
    float fov=86.f;
    if (g_getFov) {
        auto* ci=worldanalysis::sdk::ClientInstance::current(); auto* lr=ci?ci->levelRenderer():nullptr; auto* rp=lr?lr->playerRenderer():nullptr;
        if (rp) { const float live=g_getFov(rp,70.f,true); if (std::isfinite(live)&&live>=35.f&&live<=150.f) fov=live; }
    }
    return std::clamp(fov*.58f,28.f,70.f)*kPi/180.f;
}

Vec3 notificationAnchor(const Vec3& eye, const Vec2& rotation, int lane) {
    // Match System Notifications: choose a world-space point once at spawn.
    // The card continues to billboard toward the camera, but its position does
    // not follow the player after it appears.
    struct Lane { float lateral; float vertical; float depth; };
    static constexpr std::array<Lane,kMaxNotifications> lanes{{
        {-1.30f, .02f,3.35f},{ 1.30f, .02f,3.35f},
        {-1.42f, .48f,3.55f},{ 1.42f, .48f,3.55f},
        {-1.42f,-.48f,3.55f},{ 1.42f,-.48f,3.55f},
        {-1.68f, .82f,3.85f},{ 1.68f,-.82f,3.85f},
        {-1.92f, .18f,4.10f},{ 1.92f, .18f,4.10f}
    }};
    lane=std::clamp(lane,0,static_cast<int>(lanes.size()-1));
    const float yaw=rotation.y*kPi/180.f;
    const Vec3 forward{-std::sin(yaw),0,std::cos(yaw)};
    const Vec3 right{std::cos(yaw),0,std::sin(yaw)};
    return add(eye,add(mul(forward,lanes[lane].depth),
        add(mul(right,lanes[lane].lateral),{0,lanes[lane].vertical,0})));
}

int reserveNotificationLaneLocked(Clock::time_point now) {
    g_notifications.erase(std::remove_if(g_notifications.begin(),g_notifications.end(),[&](const CountNotification& n){return n.expires<=now;}),g_notifications.end());
    std::array<bool,kMaxNotifications> used{};
    for (const auto& n:g_notifications) if (n.lane>=0&&n.lane<static_cast<int>(used.size())) used[static_cast<std::size_t>(n.lane)]=true;
    for (std::size_t i=0;i<used.size();++i) if (!used[i]) return static_cast<int>(i);
    const auto oldest=std::min_element(g_notifications.begin(),g_notifications.end(),[](const auto&a,const auto&b){return a.created<b.created;});
    const int lane=oldest==g_notifications.end()?0:oldest->lane;
    if (oldest!=g_notifications.end()) g_notifications.erase(oldest);
    return lane;
}

void updateNotificationsLocked(const std::unordered_map<std::string,int>& counts, const Vec3& eye, const Vec2& rotation, Clock::time_point now) {
    g_notifications.erase(std::remove_if(g_notifications.begin(),g_notifications.end(),[&](const CountNotification& n){
        const auto it=counts.find(n.species);
        return n.expires<=now || it==counts.end() || it->second<=0;
    }),g_notifications.end());

    // Existing cards keep the world anchor they received at creation time.
    const float duration=std::clamp(g_mod?g_mod->notificationDuration:5.f,1.f,10.f);
    for (const auto& [species,count]:counts) {
        if (count<=0) continue;
        const auto old=g_counts.find(species);
        const int previous=old==g_counts.end()?0:old->second;
        if (previous==count) continue;
        auto existing=std::find_if(g_notifications.begin(),g_notifications.end(),[&](const CountNotification& n){return n.species==species;});
        if (existing==g_notifications.end()) {
            CountNotification n{};
            n.species=species; n.count=count; n.lane=reserveNotificationLaneLocked(now);
            n.anchor=notificationAnchor(eye,rotation,n.lane); n.created=now;
            n.expires=now+std::chrono::milliseconds(static_cast<int>(duration*1000.f));
            g_notifications.push_back(std::move(n));
        } else {
            existing->count=count;
            existing->created=now;
            existing->expires=now+std::chrono::milliseconds(static_cast<int>(duration*1000.f));
        }
    }
}

void clearAll() {
    g_seen.clear(); g_counted.clear(); g_highlightQueue.clear(); g_nextHighlight={};
    std::lock_guard lock(g_renderMutex);
    g_pulses.clear(); g_counts.clear(); g_notifications.clear();
}

Vec3 rotate(Vec3 p,float ax,float ay,float az) {
    float c=std::cos(ax),s=std::sin(ax); p={p.x,p.y*c-p.z*s,p.y*s+p.z*c};
    c=std::cos(ay);s=std::sin(ay);p={p.x*c+p.z*s,p.y,-p.x*s+p.z*c};
    c=std::cos(az);s=std::sin(az);return{p.x*c-p.y*s,p.x*s+p.y*c,p.z};
}

void renderHighlights(const worldanalysis::worldoverlay::RenderContext& ctx) {
    void* mat=worldanalysis::worldoverlay::depthMaterial(); if (!mat) return;
    const auto now=Clock::now();
    const float lifetime=std::clamp(g_mod?g_mod->highlightDuration:1.f,1.f,10.f);
    std::vector<Pulse> pulses;
    {
        std::lock_guard lock(g_renderMutex);
        g_pulses.erase(std::remove_if(g_pulses.begin(),g_pulses.end(),[&](const Pulse&p){return std::chrono::duration<float>(now-p.created).count()>lifetime;}),g_pulses.end());
        pulses=g_pulses;
    }
    for (const auto&p:pulses) {
        const float age=std::chrono::duration<float>(now-p.created).count();
        const float introTime=std::min(.22f,std::max(.10f,lifetime*.16f));
        const float outroTime=std::min(.34f,std::max(.16f,lifetime*.22f));
        const float appear=smooth(age/introTime);
        const float fade=1.f-smooth((age-(lifetime-outroTime))/outroTime);
        const float alpha=std::clamp(appear*fade,0.f,1.f); if (alpha<=.01f) continue;
        const Vec3 center{(p.bounds.min.x+p.bounds.max.x)*.5f,(p.bounds.min.y+p.bounds.max.y)*.5f,(p.bounds.min.z+p.bounds.max.z)*.5f};
        const Vec3 half{std::max(.12f,(p.bounds.max.x-p.bounds.min.x)*.56f)*appear,std::max(.12f,(p.bounds.max.y-p.bounds.min.y)*.56f)*appear,std::max(.12f,(p.bounds.max.z-p.bounds.min.z)*.56f)*appear};
        const float phase=age*4.8f; const float sx=((p.seed>>0)&255)/255.f,sy=((p.seed>>8)&255)/255.f,sz=((p.seed>>16)&255)/255.f;
        const float ax=(sx-.5f)*1.4f+phase*(.55f+sx*.45f),ay=(sy-.5f)*1.8f+phase*(.70f+sy*.55f),az=(sz-.5f)*1.2f+phase*(.42f+sz*.40f);
        std::array<Vec3,8> v{};
        for (int i=0;i<8;++i) { Vec3 q{(i&1)?half.x:-half.x,(i&2)?half.y:-half.y,(i&4)?half.z:-half.z}; v[static_cast<std::size_t>(i)]=add(center,rotate(q,ax,ay,az)); }
        static constexpr int edges[12][2]={{0,1},{0,2},{1,3},{2,3},{4,5},{4,6},{5,7},{6,7},{0,4},{1,5},{2,6},{3,7}};
        std::vector<Segment> lines; lines.reserve(12); for (const auto&e:edges) lines.push_back({v[e[0]],v[e[1]]});
        worldanalysis::worldoverlay::drawLines(ctx,mat,lines,white(alpha),1.6f);
    }
}

void renderNotifications(const worldanalysis::worldoverlay::RenderContext& ctx) {
    void* mat=worldanalysis::worldoverlay::throughWallMaterial(); if (!mat) return;
    const auto now=Clock::now();
    std::vector<CountNotification> notes;
    {
        std::lock_guard lock(g_renderMutex);
        g_notifications.erase(std::remove_if(g_notifications.begin(),g_notifications.end(),[&](const CountNotification& n){return n.expires<=now;}),g_notifications.end());
        notes=g_notifications;
    }
    for (const auto& note:notes) {
        const float age=std::chrono::duration<float>(now-note.created).count();
        const float remaining=std::chrono::duration<float>(note.expires-now).count();
        const float intro=smooth(age/.30f),fade=smooth(remaining/.42f),alpha=intro*fade;
        if (alpha<=.001f) continue;
        Vec3 right{},up{}; worldanalysis::worldoverlay::billboardBasis(note.anchor,ctx.camera,right,up);
        const float halfW=.66f*intro,halfH=.25f*intro;
        const Vec3 bl=add(note.anchor,add(mul(right,-halfW),mul(up,-halfH)));
        const Vec3 br=add(note.anchor,add(mul(right, halfW),mul(up,-halfH)));
        const Vec3 tr=add(note.anchor,add(mul(right, halfW),mul(up, halfH)));
        const Vec3 tl=add(note.anchor,add(mul(right,-halfW),mul(up, halfH)));
        worldanalysis::worldoverlay::drawQuads(ctx,mat,{{bl,br,tr,tl}},static_cast<std::uint32_t>(alpha*42.f)<<24);
        worldanalysis::worldoverlay::drawLines(ctx,mat,{{bl,br},{br,tr},{tr,tl},{tl,bl}},white(alpha),2.1f);

        std::string title=note.species;
        std::transform(title.begin(),title.end(),title.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
        const std::string body="VISIBLE: "+std::to_string(note.count);
        const float textAlpha=alpha*smooth((intro-.22f)/.78f);
        const float titlePx=worldanalysis::worldoverlay::fitBillboardTextPixelSize(title,1.10f,.0180f,.0085f);
        const float bodyPx=worldanalysis::worldoverlay::fitBillboardTextPixelSize(body,1.10f,.0150f,.0080f);
        worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,title,add(note.anchor,mul(up,.090f)),right,up,titlePx,white(textAlpha),false);
        worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,body,add(note.anchor,mul(up,-.075f)),right,up,bodyPx,white(textAlpha),false);

        const float pulse=.5f+.5f*std::sin(age*7.f);
        const float extent=.34f*intro;
        const Vec3 a=add(note.anchor,add(mul(right,-extent),mul(up,-.185f)));
        const Vec3 b=add(note.anchor,add(mul(right, extent),mul(up,-.185f)));
        const Vec3 p=add(a,mul(sub(b,a),std::clamp(.18f+.82f*pulse,0.f,1.f)));
        worldanalysis::worldoverlay::drawLines(ctx,mat,{{a,p}},white(.72f*alpha),1.5f);
    }
}

void renderMobCounter(void* levelRenderer,void* screen,void*) {
    if (!g_mod||!g_mod->enabled) return;
    worldanalysis::worldoverlay::RenderContext ctx{};
    if (!worldanalysis::worldoverlay::makeContext(levelRenderer,screen,ctx)) return;
    worldanalysis::worldoverlay::ColorScope scope(g_mod->color,g_mod->backgroundColor,g_mod->backgroundOpacity);
    renderNotifications(ctx);
    renderHighlights(ctx);
}
} // namespace

MobCounterModule::MobCounterModule()
    : Module("Mob Counter","Counts non-player mobs that remain inside your current POV for three seconds. Each species spawns a colorable in-world notification for the configured duration, while newly counted mobs receive rapid sequential depth-tested rotating cube highlights with an adjustable 1-10 second lifetime.") {
    g_mod=this; showInMenu=true; exposeBackgroundStyle=true;
}
MobCounterModule::~MobCounterModule(){if(g_mod==this)g_mod=nullptr;}
void MobCounterModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorManagerList))g_actorList=reinterpret_cast<RuntimeActorListFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorIsPlayer))g_actorIsPlayer=reinterpret_cast<ActorIsPlayerFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetMaxHealth))g_getMaxHealth=reinterpret_cast<ActorHealthFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceIsSolidBlockingBlock))g_isSolid=reinterpret_cast<SolidFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::GetFov))g_getFov=reinterpret_cast<GetFovFn>(a);
    worldanalysis::worldoverlay::initialize();
    worldanalysis::worldoverlay::registerRenderCallback(renderMobCounter);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod&&g_mod->enabled)g_mod->handleTick(e.player);});
}
void MobCounterModule::onEnable(){clearAll();}
void MobCounterModule::onDisable(){clearAll();}
void MobCounterModule::handleTick(worldanalysis::sdk::Player*player){
    if(!enabled||!player||!g_actorList){clearAll();return;}
    auto*level=player->level(); auto*dimension=player->dimension(); void*region=dimension?dimension->blockSource():nullptr; void*manager=level?level->actorManager():nullptr;
    if(!manager||!region){clearAll();return;}
    const Vec3 feet=player->position(); if(!finite(feet)){clearAll();return;}
    const Vec3 eye=add(feet,{0,1.62f,0}),forward=lookDirection(player->rotation()); const Vec2 rotation=player->rotation();
    const float halfFov=resolveHalfFov(),range=std::clamp(countRange,8.f,96.f); const auto now=Clock::now();
    std::vector<void*> actors=g_actorList(manager); if(actors.size()>kMaxActors)actors.resize(kMaxActors);
    std::unordered_set<std::uintptr_t> currentlyVisible; std::vector<MatureMob> matured;
    for(void*raw:actors){
        if(!raw||raw==player)continue;
        if(g_actorIsPlayer&&g_actorIsPlayer(raw))continue;
        auto*actor=reinterpret_cast<worldanalysis::sdk::Actor*>(raw); if((actor->categories()&(0x2u|0x4u))==0)continue;
        const AABB b=actor->bounds(); if(!validBounds(b))continue;
        if(!inView(eye,forward,b,halfFov,range)||!visibleByBlocks(region,eye,forward,b,halfFov,range))continue;
        const auto id=reinterpret_cast<std::uintptr_t>(raw); currentlyVisible.insert(id);
        auto it=g_seen.find(id);
        if(it==g_seen.end()){SeenState s;s.entered=now;s.bounds=b;s.species=speciesFor(raw,b);it=g_seen.emplace(id,std::move(s)).first;} else it->second.bounds=b;
        if(std::chrono::duration<float>(now-it->second.entered).count()>=kDwellSeconds)matured.push_back({id,b,it->second.species});
    }
    for(auto it=g_seen.begin();it!=g_seen.end();){if(!currentlyVisible.contains(it->first)){g_counted.erase(it->first);it=g_seen.erase(it);}else++it;}
    g_highlightQueue.erase(std::remove_if(g_highlightQueue.begin(),g_highlightQueue.end(),[&](const MatureMob&m){return !currentlyVisible.contains(m.id);}),g_highlightQueue.end());
    std::unordered_map<std::string,int> counts;
    for(const auto&m:matured){++counts[m.species];if(g_counted.insert(m.id).second)g_highlightQueue.push_back(m);}
    if(g_nextHighlight.time_since_epoch().count()==0)g_nextHighlight=now;
    bool makePulse=false; MatureMob pulseMob{};
    if(now>=g_nextHighlight&&!g_highlightQueue.empty()){pulseMob=g_highlightQueue.front();g_highlightQueue.pop_front();makePulse=true;g_nextHighlight=now+std::chrono::milliseconds(115);}
    const float lifetime=std::clamp(highlightDuration,1.f,10.f);
    {
        std::lock_guard lock(g_renderMutex);
        updateNotificationsLocked(counts,eye,rotation,now);
        if(makePulse)g_pulses.push_back({pulseMob.id,pulseMob.bounds,now,rng()});
        g_pulses.erase(std::remove_if(g_pulses.begin(),g_pulses.end(),[&](const Pulse&p){return !currentlyVisible.contains(p.id)||std::chrono::duration<float>(now-p.created).count()>lifetime;}),g_pulses.end());
        g_counts=std::move(counts);
    }
}
void MobCounterModule::loadConfig(const nlohmann::json&j){
    Module::loadConfig(j);
    countRange=std::clamp(j.value("countRange",countRange),8.f,96.f);
    notificationDuration=std::clamp(j.value("notificationDuration",notificationDuration),1.f,10.f);
    highlightDuration=std::clamp(j.value("highlightDuration",highlightDuration),1.f,10.f);
}
void MobCounterModule::saveConfig(nlohmann::json&j){
    Module::saveConfig(j);
    j["countRange"]=countRange;
    j["notificationDuration"]=notificationDuration;
    j["highlightDuration"]=highlightDuration;
}
