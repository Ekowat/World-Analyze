#include "analysismenu.hpp"

#include "analyze.hpp"
#include "itemutils.hpp"
#include "worldoverlay.hpp"
#include "../ModuleRegistry.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/DimensionIdentity.hpp>
#include <worldanalysis/sdk/world/HitResult.hpp>
#include <worldanalysis/sdk/world/Weather.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Vec2=worldanalysis::sdk::Vec2;
using Vec3=worldanalysis::sdk::Vec3;
using Clock=std::chrono::steady_clock;
using Segment=worldanalysis::worldoverlay::Segment;
using Quad=worldanalysis::worldoverlay::Quad;

enum class Page : std::uint8_t { Home, Player, Time, Equipment, Environment, Systems };

struct TouchGesture { bool active=false; int pointer=-1; float x=0,y=0,maxMove2=0; Clock::time_point started{}; };
struct Shard { Vec3 origin{},velocity{}; float spin=0; Clock::time_point created{}; };
struct Telemetry {
    float hp=-1,maxHp=-1,hunger=-1,saturation=-1,exhaustion=-1;
    int xpLevel=-1,armorPoints=0;
    std::string mainHand="EMPTY",offHand="EMPTY";
    std::array<std::string,5> armorNames{};
    int rawTime=std::numeric_limits<int>::min(),day=0,tick=0;
    std::string clock="--:--",phase="UNKNOWN";
    std::string dimension="UNKNOWN",weather="UNKNOWN",biome="UNKNOWN";
    float rain=0,lightning=0,brightness=-1,temperature=999;
    int enabledModules=0,totalModules=0;
    std::vector<std::string> moduleNames;
};

using LevelGetHitResultFn=worldanalysis::sdk::HitResult*(*)(void*);
using ActorGetHealthFn=float(*)(void*);
using ActorGetMaxHealthFn=int(*)(void*);
using AttributesGetByNameFn=void*(*)(void*,const std::string*);
using ActorGetEquipmentItemFn=void*(*)(void*,int);
using GetBlockFn=void*(*)(void*,const struct BlockPosRaw&);
using BrightnessFn=float(*)(void*,const struct BlockPosRaw&);
using GetBiomeFn=void*(*)(void*,const void*);
using BiomeTemperatureFn=float(*)(void*,void*,void*);

struct BlockPosRaw { int x=0,y=0,z=0; };

AnalysisMenuModule* g_mod=nullptr;
LevelGetHitResultFn g_getHitResult=nullptr;
ActorGetHealthFn g_getHealth=nullptr;
ActorGetMaxHealthFn g_getMaxHealth=nullptr;
AttributesGetByNameFn g_getAttribute=nullptr;
ActorGetEquipmentItemFn g_getEquipment=nullptr;
GetBlockFn g_getBlock=nullptr;
BrightnessFn g_getBrightness=nullptr;
GetBiomeFn g_getBiome=nullptr;
BiomeTemperatureFn g_getTemperature=nullptr;

constexpr std::uint32_t kAttributesHash=0xFD3B0613u;
constexpr std::size_t kAttributesStride=0x58;
constexpr float kPi=3.14159265358979323846f;
constexpr float kCompactW=.94f,kCompactH=.34f;
constexpr float kHomeW=1.72f,kHomeH=1.30f,kTabW=1.92f,kTabH=1.32f;

std::mutex g_mutex;
std::mutex g_touchMutex;
worldanalysis::sdk::Player* g_player=nullptr;
Telemetry g_data{};
Page g_page=Page::Home;
Page g_previousPage=Page::Home;
Page g_targetPage=Page::Home;
bool g_compact=true;
bool g_transitionFromCompact=false;
Clock::time_point g_transitionStarted{};
float g_transition=1.f;
Vec3 g_panel{};
Vec3 g_camera{};
Vec3 g_viewOrigin{},g_viewDirection{};
bool g_viewValid=false,g_panelValid=false,g_orbitInit=false;
float g_orbit=0,g_orbitDir=-1.f,g_appear=0;
Clock::time_point g_orbitUpdated{},g_hiddenUntil{},g_animChanged{};
int g_cornerVisual=0;
std::uint32_t g_rng=0xA3B14C27u;
std::atomic_int g_action{0};
TouchGesture g_touch{};
std::vector<Shard> g_shards;

bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024&&p.y<4096;}
Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(const Vec3&a,const Vec3&b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float dot(const Vec3&a,const Vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(const Vec3&a,const Vec3&b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float len(const Vec3&v){return std::sqrt(dot(v,v));}
Vec3 norm(const Vec3&v,const Vec3&fallback={0,0,1}){float l=len(v);return l>.0001f?mul(v,1.f/l):fallback;}
float smooth(float v){v=std::clamp(v,0.f,1.f);return v*v*(3.f-2.f*v);}
std::uint32_t white(float a){return(static_cast<std::uint32_t>(std::clamp(a,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu;}
std::uint32_t rnd(){g_rng=g_rng*1664525u+1013904223u;return g_rng;}
float rnd01(){return static_cast<float>((rnd()>>8)&0xFFFFFFu)/16777215.f;}
std::string one(float v){char b[32];std::snprintf(b,sizeof(b),"%.1f",static_cast<double>(v));return b;}
std::string pct(float a,float b){if(b<=0)return"N/A";return std::to_string(static_cast<int>(std::clamp(a/b,0.f,1.f)*100.f+.5f))+"%";}

bool plausible(std::uintptr_t p,std::size_t align=alignof(void*)){return worldanalysis::worldoverlay::plausible(p,align);}
void* runtimeComponent(void* raw,std::uint32_t typeHash,std::size_t stride){
    if(!raw||!stride)return nullptr;constexpr std::uint64_t noNode=std::numeric_limits<std::uint64_t>::max();const auto actor=reinterpret_cast<std::uintptr_t>(raw);
    const auto registry=*reinterpret_cast<const std::uintptr_t*>(actor+0x10);const std::uint32_t entity=*reinterpret_cast<const std::uint32_t*>(actor+0x18);if(!plausible(registry))return nullptr;
    const auto bb=*reinterpret_cast<const std::uintptr_t*>(registry+0x38),be=*reinterpret_cast<const std::uintptr_t*>(registry+0x40),nodes=*reinterpret_cast<const std::uintptr_t*>(registry+0x50),sentinel=*reinterpret_cast<const std::uintptr_t*>(registry+0x58);
    if(!plausible(bb)||!plausible(be)||be<=bb||!plausible(nodes))return nullptr;const std::size_t buckets=(be-bb)>>3;if(!buckets||buckets>(1u<<20))return nullptr;
    std::uint64_t idx=*reinterpret_cast<const std::uint64_t*>(bb+(((buckets-1)&typeHash)<<3));std::uintptr_t storage=0;
    for(std::size_t guard=0;idx!=noNode&&guard<512;++guard){if(idx>(1u<<24))return nullptr;const auto node=nodes+static_cast<std::uintptr_t>(idx)*0x20;if(!plausible(node))return nullptr;if(*reinterpret_cast<const std::uint32_t*>(node+8)==typeHash){if(node==sentinel)return nullptr;storage=*reinterpret_cast<const std::uintptr_t*>(node+0x10);break;}idx=*reinterpret_cast<const std::uint64_t*>(node);}
    if(!plausible(storage))return nullptr;const auto sb=*reinterpret_cast<const std::uintptr_t*>(storage+8),se=*reinterpret_cast<const std::uintptr_t*>(storage+0x10);if(!plausible(sb)||!plausible(se)||se<sb)return nullptr;const std::size_t page=(static_cast<std::size_t>(entity)>>11)&0x7F;if(page>=((se-sb)>>3))return nullptr;const auto sparse=*reinterpret_cast<const std::uintptr_t*>(sb+page*8);if(!plausible(sparse))return nullptr;const std::size_t slot=(static_cast<std::size_t>(entity)&0x3FFFFu)&0x7FFu;const std::uint32_t packed=*reinterpret_cast<const std::uint32_t*>(sparse+slot*4);if((packed^(entity&0xFFFC0000u))>0x3FFFEu)return nullptr;const auto dense=*reinterpret_cast<const std::uintptr_t*>(storage+0x50);if(!plausible(dense))return nullptr;const auto densePage=*reinterpret_cast<const std::uintptr_t*>(dense+((static_cast<std::size_t>(packed)>>4)&0x3FF8u));if(!plausible(densePage))return nullptr;const auto comp=densePage+static_cast<std::size_t>(packed&0x7Fu)*stride;return plausible(comp,4)?reinterpret_cast<void*>(comp):nullptr;
}
bool attribute(void* player,const char* canonical,const char* shortName,float lo,float hi,float& out){if(!g_getAttribute)return false;void*a=runtimeComponent(player,kAttributesHash,kAttributesStride);if(!a)return false;std::string c(canonical),s(shortName);void*i=g_getAttribute(a,&c);if(!i)i=g_getAttribute(a,&s);if(!i||!plausible(reinterpret_cast<std::uintptr_t>(i),4))return false;float v=*reinterpret_cast<const float*>(static_cast<const std::byte*>(i)+0x7C);if(!std::isfinite(v)||v<lo||v>hi)return false;out=v;return true;}
int levelTime(worldanalysis::sdk::Level* level){if(!level||!plausible(reinterpret_cast<std::uintptr_t>(level)))return std::numeric_limits<int>::min();auto**t=*reinterpret_cast<void***>(level);constexpr auto slot=worldanalysis::sdk::offsets::VTable::Level_getTime;if(!t||!plausible(reinterpret_cast<std::uintptr_t>(t[slot]),4))return std::numeric_limits<int>::min();using Fn=int(*)(void*);int v=reinterpret_cast<Fn>(t[slot])(level);return(v>-2000000000&&v<2000000000)?v:std::numeric_limits<int>::min();}
Vec3 lookDir(const Vec2&r){float p=r.x*kPi/180.f,y=r.y*kPi/180.f,cp=std::cos(p);return norm({-std::sin(y)*cp,-std::sin(p),std::cos(y)*cp});}
std::string dimensionName(void*d){using K=worldanalysis::sdk::dimension_identity::Kind;switch(worldanalysis::sdk::dimension_identity::kind(d)){case K::Overworld:return"OVERWORLD";case K::Nether:return"NETHER";case K::TheEnd:return"THE END";default:return"UNKNOWN";}}
std::string biomeName(void*b){if(!b||!plausible(reinterpret_cast<std::uintptr_t>(b)))return"UNKNOWN";const auto*v=reinterpret_cast<const std::string*>(reinterpret_cast<std::uintptr_t>(b)+worldanalysis::sdk::offsets::Biome::mHash+sizeof(std::uint64_t));if(v->empty()||v->size()>96)return"UNKNOWN";std::string s=*v;if(s.starts_with("minecraft:"))s.erase(0,10);for(char&c:s)if(c=='_')c=' ';for(bool cap=true;char&c:s){if(c==' '){cap=true;continue;}if(cap&&c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');cap=false;}return s;}

int armorValue(std::string name){for(char&c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));auto has=[&](std::string_view s){return name.find(s)!=std::string::npos;};int material=0;if(has("leather"))material=1;else if(has("gold"))material=2;else if(has("chain"))material=3;else if(has("iron"))material=4;else if(has("diamond")||has("netherite"))material=5;else if(has("turtle"))return 2;else return 0;if(has("helmet"))return material==1?1:2+(material==5);if(has("chestplate"))return material==1?3:(material<=3?5:(material==4?6:8));if(has("leggings"))return material==1?2:(material==2?3:(material==3?4:(material==4?5:6)));if(has("boots"))return material<=3?1:(material==4?2:3);return 0;}

bool pointAir(void*region,const Vec3&p){if(!region||!g_getBlock)return true;BlockPosRaw bp{static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.y)),static_cast<int>(std::floor(p.z))};void*b=g_getBlock(region,bp);if(!b)return true;const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(b)+worldanalysis::sdk::offsets::Block::mBlockType);if(!plausible(type))return true;const auto addr=type+worldanalysis::sdk::offsets::BlockType::mNameInfo+worldanalysis::sdk::offsets::NameInfo::mFullName+worldanalysis::sdk::offsets::HashedString::mString;const auto*s=reinterpret_cast<const std::string*>(addr);if(s->size()>128)return false;return s->empty()||*s=="minecraft:air"||*s=="minecraft:cave_air"||*s=="minecraft:void_air";}
bool clearPanel(void*region,const Vec3&p){for(const Vec3&o:std::array<Vec3,13>{{{0,0,0},{.84f,0,0},{-.84f,0,0},{0,0,.84f},{0,0,-.84f},{0,.56f,0},{0,-.56f,0},{.60f,0,.60f},{.60f,0,-.60f},{-.60f,0,.60f},{-.60f,0,-.60f},{0,.34f,.52f},{0,-.34f,-.52f}}})if(!pointAir(region,add(p,o)))return false;return true;}

void cacheRay(worldanalysis::sdk::Player*p){if(!p){g_viewValid=false;return;}Vec3 origin=add(p->position(),{0,1.62f,0}),dir=lookDir(p->rotation());if(auto*l=p->level())if(auto*h=g_getHitResult?g_getHitResult(l):l->storedHitResult()){Vec3 a=h->startPosition(),b=h->position(),d=sub(b,a);if(finite(a)&&finite(b)&&len(d)>.05f){origin=a;dir=norm(d,dir);}}g_viewOrigin=origin;g_viewDirection=dir;g_viewValid=finite(origin)&&finite(dir);}

Vec3 orbitTarget(worldanalysis::sdk::Player* p) {
    const Vec3 pos = p->position();
    const auto now = Clock::now();
    Vec3 forward = g_viewValid ? g_viewDirection : lookDir(p->rotation());
    forward.y = 0.f;
    forward = norm(forward, {0,0,1});
    const float front = std::atan2(forward.x, -forward.z);
    auto radial=[](float a){return Vec3{std::sin(a),0,-std::cos(a)};};

    if (!g_orbitInit) {
        // Mirror Analyze's initial side so both panels naturally begin apart.
        g_orbit = front - 2.20f;
        g_orbitDir = -1.f;
        g_orbitUpdated = now;
        g_orbitInit = true;
    }

    const float dt = std::clamp(std::chrono::duration<float>(now-g_orbitUpdated).count(),0.f,.10f);
    g_orbitUpdated = now;
    const bool playerLookingAtPanel = dot(radial(g_orbit), forward) > .84f;
    if (!playerLookingAtPanel) {
        const float speed = .095f;
        float proposed = g_orbit + g_orbitDir * speed * dt;
        // Match Analyze exactly: bounce before the passive orbit crosses the
        // player's direct-front cone, but freeze if the player turns to look.
        if (dot(radial(proposed), forward) > .70f) {
            g_orbitDir = -g_orbitDir;
            proposed = g_orbit + g_orbitDir * speed * dt;
        }
        g_orbit = proposed;
    }

    void* region = p->dimension() ? p->dimension()->blockSource() : nullptr;
    Vec3 analyze{};
    const bool hasAnalyze = queryAnalyzePanelPosition(analyze);

    auto acceptable=[&](const Vec3& c){
        // The menu is intentionally much smaller than Analyze, so it only needs
        // enough separation to keep the two physical billboards from touching.
        return clearPanel(region,c) && (!hasAnalyze || len(sub(c,analyze)) >= 1.35f);
    };

    // Preferred placement is compact and close to the player. Keep the same
    // Analyze-style orbit behavior, but use a smaller radius so text remains
    // readable without making the panel physically large.
    for (const auto& rv : std::array<std::pair<float,float>,5>{{
            {1.55f,1.06f},{1.78f,1.06f},{1.95f,1.18f},
            {1.55f,1.48f},{1.82f,1.48f}}}) {
        Vec3 c{pos.x+std::sin(g_orbit)*rv.first,pos.y+rv.second,pos.z-std::cos(g_orbit)*rv.first};
        if (acceptable(c)) return c;
    }

    // Only geometry/panel separation may cause an angular detour; camera look
    // direction never pushes the menu away.
    for (float delta : std::array<float,8>{.28f,-.28f,.52f,-.52f,.78f,-.78f,1.02f,-1.02f}) {
        const float a = g_orbit + delta;
        Vec3 c{pos.x+std::sin(a)*1.68f,pos.y+1.16f,pos.z-std::cos(a)*1.68f};
        if (acceptable(c)) { g_orbit=a; return c; }
    }
    const float nan=std::numeric_limits<float>::quiet_NaN();
    return {nan,nan,nan};
}

float currentWidth(){
    const float t=smooth(g_transition);
    if(g_compact)return kCompactW;
    if(g_transitionFromCompact&&g_transition<1.f)return kCompactW+(kHomeW-kCompactW)*t;
    if(g_page==Page::Home&&g_previousPage!=Page::Home)return kTabW+(kHomeW-kTabW)*t;
    if(g_page!=Page::Home&&g_previousPage==Page::Home)return kHomeW+(kTabW-kHomeW)*t;
    return g_page==Page::Home?kHomeW:kTabW;
}
float currentHeight(){
    const float t=smooth(g_transition);
    if(g_compact)return kCompactH;
    if(g_transitionFromCompact&&g_transition<1.f)return kCompactH+(kHomeH-kCompactH)*t;
    if(g_page==Page::Home&&g_previousPage!=Page::Home)return kTabH+(kHomeH-kTabH)*t;
    if(g_page!=Page::Home&&g_previousPage==Page::Home)return kHomeH+(kTabH-kHomeH)*t;
    return g_page==Page::Home?kHomeH:kTabH;
}

int panelHit(){
    std::lock_guard lock(g_mutex);
    if(!g_panelValid||Clock::now()<g_hiddenUntil)return 0;
    Vec3 origin=g_viewValid?g_viewOrigin:g_camera,ray=g_viewValid?g_viewDirection:Vec3{0,0,1};
    if(!finite(origin)||!finite(ray))return 0;
    Vec3 r{},u{};worldanalysis::worldoverlay::billboardBasis(g_panel,origin,r,u);
    Vec3 n=norm(cross(r,u));float denom=dot(ray,n);if(std::abs(denom)<.0001f)return 0;
    float t=dot(sub(g_panel,origin),n)/denom;if(t<0||t>6)return 0;
    Vec3 local=sub(add(origin,mul(ray,t)),g_panel);float x=dot(local,r),y=dot(local,u);
    float w=currentWidth()*.5f,h=currentHeight()*.5f;
    if(std::abs(x)>w+.07f||std::abs(y)>h+.07f)return 0;
    if(g_compact){
        if(std::abs(x)<w+.04f&&std::abs(y)<h+.04f)return 97;
        return 0;
    }
    const float closeX=w-.12f,closeY=h-.12f;
    if(std::abs(x-closeX)<.10f&&std::abs(y-closeY)<.10f)return 99;
    if(g_page!=Page::Home){
        const float menuX=-w+.22f,menuY=h-.17f;
        if(std::abs(x-menuX)<.20f&&std::abs(y-menuY)<.095f)return 98;
        return 0;
    }
    const std::array<std::pair<float,float>,5> c{{
        {-.38f,.08f},{.38f,.08f},{-.38f,-.16f},{.38f,-.16f},{0.f,-.36f}}};
    const std::array<std::pair<float,float>,5> half{{
        {.29f,.082f},{.29f,.082f},{.29f,.082f},{.29f,.082f},{.34f,.078f}}};
    for(int i=0;i<5;++i)if(std::abs(x-c[i].first)<half[i].first&&std::abs(y-c[i].second)<half[i].second)return i+1;
    return 0;
}

void switchPage(Page p){g_transitionFromCompact=false;g_previousPage=g_page;g_targetPage=p;g_transitionStarted=Clock::now();g_transition=0.f;g_page=p;g_cornerVisual=static_cast<int>(rnd()%6);}
void openMenu(){g_hiddenUntil={};g_appear=std::max(g_appear,.35f);g_previousPage=Page::Home;g_page=Page::Home;g_targetPage=Page::Home;g_compact=false;g_transitionFromCompact=true;g_transitionStarted=Clock::now();g_transition=0.f;g_cornerVisual=static_cast<int>(rnd()%6);}
void shatter(){
    const auto now=Clock::now();
    g_shards.clear();
    const float w=currentWidth(),h=currentHeight();
    Vec3 right{},up{};const Vec3 eye=g_viewValid?g_viewOrigin:g_camera;worldanalysis::worldoverlay::billboardBasis(g_panel,eye,right,up);
    const Vec3 normal=norm(cross(right,up),{0,0,1});
    constexpr int cols=5,rows=3;
    for(int yy=0;yy<rows;++yy)for(int xx=0;xx<cols;++xx){
        const float fx=-w*.5f+(xx+.5f)*(w/cols),fy=-h*.5f+(yy+.5f)*(h/rows);
        const Vec3 origin=add(g_panel,add(mul(right,fx),mul(up,fy)));
        const Vec3 outward=norm(add(mul(right,fx),mul(up,fy)),right);
        const float speed=.72f+.08f*((xx+yy)%4);
        const Vec3 velocity=add(mul(outward,speed),add(mul(up,.08f+.03f*yy),mul(normal,.08f*((xx+yy)&1?1.f:-1.f))));
        g_shards.push_back({origin,velocity,xx*1.4f+yy*.8f,now});
    }
    g_hiddenUntil=now+std::chrono::milliseconds(static_cast<int>(std::clamp(g_mod?g_mod->hiddenDuration:5.f,3.f,600.f)*1000.f));
    g_appear=0;g_compact=true;g_transitionFromCompact=false;g_page=Page::Home;g_previousPage=Page::Home;g_targetPage=Page::Home;g_transition=1.f;
}
void handleAction(int a){if(a==99){shatter();return;}if(a==97){openMenu();return;}if(a==98){switchPage(Page::Home);return;}if(a>=1&&a<=5)switchPage(static_cast<Page>(a));}

void sample(worldanalysis::sdk::Player*p){Telemetry d{};if(g_getHealth){float v=g_getHealth(p);if(std::isfinite(v)&&v>=0&&v<100000)d.hp=v;}if(g_getMaxHealth){int v=g_getMaxHealth(p);if(v>0&&v<100000)d.maxHp=static_cast<float>(v);}float v=0;if(attribute(p,"minecraft:player.hunger","player.hunger",0,20.1f,v))d.hunger=v;if(attribute(p,"minecraft:player.saturation","player.saturation",0,20.1f,v))d.saturation=v;if(attribute(p,"minecraft:player.exhaustion","player.exhaustion",0,8.1f,v))d.exhaustion=v;if(attribute(p,"minecraft:player.level","player.level",0,1000000,v))d.xpLevel=static_cast<int>(std::floor(v+.001f));
    if(g_getEquipment){auto describe=[&](int slot){void*s=g_getEquipment(p,slot);auto snap=worldanalysis::items::inspect(s);return snap.valid()?snap.name:std::string("EMPTY");};d.mainHand=describe(0);d.offHand=describe(1);for(int i=0;i<5;++i){d.armorNames[i]=describe(i+2);d.armorPoints+=armorValue(d.armorNames[i]);}d.armorPoints=std::clamp(d.armorPoints,0,20);}
    d.rawTime=levelTime(p->level());if(d.rawTime!=std::numeric_limits<int>::min()){int t=d.rawTime%24000;if(t<0)t+=24000;d.tick=t;d.day=std::max(0,d.rawTime/24000)+1;int minutes=((t+6000)%24000)*1440/24000;char b[16];std::snprintf(b,sizeof(b),"%02d:%02d",minutes/60,minutes%60);d.clock=b;d.phase=t<6000?"MORNING":t<12000?"MIDDAY":t<18000?"EVENING":"NIGHT";}
    auto*dim=p->dimension();d.dimension=dimensionName(dim);if(dim&&dim->weather()){auto*w=dim->weather();d.rain=std::clamp(std::max(w->rainLevel(),w->targetRainLevel()),0.f,1.f);d.lightning=std::clamp(std::max(w->lightningLevel(),w->targetLightningLevel()),0.f,1.f);d.weather=d.lightning>.15f?"THUNDER":d.rain>.15f?"RAIN":"CLEAR";}
    if(dim){void*region=dim->blockSource();Vec3 pos=p->position();BlockPosRaw bp{static_cast<int>(std::floor(pos.x)),static_cast<int>(std::floor(pos.y)),static_cast<int>(std::floor(pos.z))};if(g_getBrightness&&region){float b=g_getBrightness(region,bp);if(std::isfinite(b)&&b>=0&&b<100)d.brightness=b;}if(g_getBiome&&region){void*bio=g_getBiome(region,&bp);d.biome=biomeName(bio);if(g_getTemperature&&bio){float tmp=g_getTemperature(bio,region,&bp);if(std::isfinite(tmp)&&tmp>-20&&tmp<20)d.temperature=tmp;}}}
    const auto&mods=ModuleRegistry::get().modules();d.totalModules=static_cast<int>(mods.size());for(auto*m:mods)if(m->enabled){++d.enabledModules;if(d.moduleNames.size()<5)d.moduleNames.push_back(m->name);}g_data=std::move(d);
}

void addRect(std::vector<Segment>&s,const Vec3&c,const Vec3&r,const Vec3&u,float x,float y,float w,float h){auto p=[&](float a,float b){return add(c,add(mul(r,a),mul(u,b)));};s.insert(s.end(),{{p(x-w,y-h),p(x+w,y-h)},{p(x+w,y-h),p(x+w,y+h)},{p(x+w,y+h),p(x-w,y+h)},{p(x-w,y+h),p(x-w,y-h)}});}
void drawCornerAnimation(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&c,const Vec3&r,const Vec3&u,float alpha,float t,float w,float h){
    std::vector<Segment>s;
    // Dedicated footer bay: the decorative animation can no longer overlap
    // buttons, page content, MENU, or the close control.
    const float x=-w*.5f+.19f,y=-h*.5f+.13f;
    switch(g_cornerVisual%6){
        case 0: for(int i=0;i<5;++i){float q=i*.038f,yy=std::sin(t*3+i)*.030f;s.push_back({add(c,add(mul(r,x+q-.075f),mul(u,y+yy))),add(c,add(mul(r,x+q-.045f),mul(u,y-yy))) });} break;
        case 1: for(int i=0;i<4;++i){float a=t*1.7f+i*kPi*.5f;s.push_back({add(c,add(mul(r,x),mul(u,y))),add(c,add(mul(r,x+std::cos(a)*.085f),mul(u,y+std::sin(a)*.052f))) });} break;
        case 2: for(int i=0;i<3;++i)addRect(s,c,r,u,x,y,.025f+i*.028f,.018f+i*.020f); break;
        case 3: for(int i=0;i<5;++i){float bx=x-.080f+i*.040f,bh=.018f+.050f*(.5f+.5f*std::sin(t*2.4f+i));s.push_back({add(c,add(mul(r,bx),mul(u,y-.035f))),add(c,add(mul(r,bx),mul(u,y-.035f+bh))) });} break;
        case 4:{Vec3 center=add(c,add(mul(r,x),mul(u,y)));for(int i=0;i<5;++i){float a=i*2*kPi/5+t*.6f;Vec3 n=add(center,add(mul(r,std::cos(a)*.085f),mul(u,std::sin(a)*.050f)));s.push_back({center,n});}}break;
        default:{float scan=-.075f+std::fmod(t*.075f,.15f);addRect(s,c,r,u,x,y,.085f,.052f);s.push_back({add(c,add(mul(r,x+scan),mul(u,y-.052f))),add(c,add(mul(r,x+scan),mul(u,y+.052f))) });}break;
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,s,white(alpha*.72f),1.15f);
}

void drawGraph(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&c,const Vec3&r,const Vec3&u,float alpha,float t){std::vector<Segment>s;std::array<Vec3,9>p{};for(int i=0;i<9;++i){float x=-.82f+i*.205f;float phase=(i/8.f)*2*kPi;float y=.02f+.18f*std::sin(phase-kPi*.5f);p[i]=add(c,add(mul(r,x),mul(u,y)));if(i)s.push_back({p[i-1],p[i]});float q=.022f;s.push_back({add(p[i],mul(r,-q)),add(p[i],mul(r,q))});s.push_back({add(p[i],mul(u,-q)),add(p[i],mul(u,q))});}worldanalysis::worldoverlay::drawLines(ctx,mat,s,white(alpha*.72f),1.25f);if(g_data.rawTime!=std::numeric_limits<int>::min()){float f=g_data.tick/24000.f*8.f;int i=std::clamp(static_cast<int>(f),0,7);float a=f-i;Vec3 q=add(p[i],mul(sub(p[i+1],p[i]),a));std::vector<Segment>d{{add(q,mul(r,-.04f)),add(q,mul(r,.04f))},{add(q,mul(u,-.04f)),add(q,mul(u,.04f))}};worldanalysis::worldoverlay::drawLines(ctx,mat,d,white(alpha),2.f);}}

void text(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,std::string_view s,const Vec3&c,const Vec3&r,const Vec3&u,float x,float y,float px,float a){worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,s,add(c,add(mul(r,x),mul(u,y))),r,u,px,white(a),false);}
void lineList(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const std::vector<std::string>&lines,const Vec3&c,const Vec3&r,const Vec3&u,float alpha){float y=.24f;for(const auto&s:lines){const float px=worldanalysis::worldoverlay::fitBillboardTextPixelSize(s,1.58f,.0085f,.0058f);text(ctx,mat,s,c,r,u,0,y,px,alpha);y-=.128f;}}

void drawPage(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&c,const Vec3&r,const Vec3&u,float alpha,float age){
    if(g_page==Page::Home)return;
    std::vector<std::string>lines;std::string title;
    switch(g_page){
        case Page::Player:title="PLAYER";lines={"HP  "+(g_data.hp>=0?one(g_data.hp)+" / "+one(g_data.maxHp):"N/A")+"   "+pct(g_data.hp,g_data.maxHp),"FOOD  "+(g_data.hunger>=0?one(g_data.hunger)+" / 20":"N/A"),"SATURATION  "+(g_data.saturation>=0?one(g_data.saturation):"N/A"),"XP LEVEL  "+(g_data.xpLevel>=0?std::to_string(g_data.xpLevel):"N/A"),"ARMOR  "+std::to_string(g_data.armorPoints)+" / 20"};break;
        case Page::Time:title="TIME";lines={"CLOCK  "+g_data.clock,"MC DAY  "+std::to_string(g_data.day),"DAY TICK  "+std::to_string(g_data.tick),"PHASE  "+g_data.phase};break;
        case Page::Equipment:title="EQUIPMENT";lines={"MAIN  "+g_data.mainHand,"OFFHAND  "+g_data.offHand,"ARMOR POINTS  "+std::to_string(g_data.armorPoints)+" / 20"};for(const auto&n:g_data.armorNames)if(n!="EMPTY"&&lines.size()<6)lines.push_back(n);break;
        case Page::Environment:title="ENVIRONMENT";lines={"DIMENSION  "+g_data.dimension,"WEATHER  "+g_data.weather,"BIOME  "+g_data.biome,"LIGHT  "+(g_data.brightness>=0?one(g_data.brightness):"N/A"),"TEMPERATURE  "+(g_data.temperature<900?one(g_data.temperature):"N/A")};break;
        case Page::Systems:title="SYSTEMS";lines={"ACTIVE  "+std::to_string(g_data.enabledModules)+" / "+std::to_string(g_data.totalModules)};for(auto&s:g_data.moduleNames)lines.push_back(s);break;
        default:break;
    }
    text(ctx,mat,title,c,r,u,0,.49f,.0116f,alpha);
    if(g_page==Page::Time){drawGraph(ctx,mat,add(c,mul(u,.015f)),r,u,alpha,age);float y=-.28f;for(const auto&s:lines){text(ctx,mat,s,c,r,u,0,y,.0077f,alpha);y-=.105f;}}
    else lineList(ctx,mat,lines,c,r,u,alpha);
}

void drawHome(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&c,const Vec3&r,const Vec3&u,float alpha){
    static const std::array<const char*,5>names{"PLAYER","TIME","EQUIPMENT","ENVIRONMENT","SYSTEMS"};
    const std::array<std::pair<float,float>,5>pos{{{-.38f,.08f},{.38f,.08f},{-.38f,-.16f},{.38f,-.16f},{0.f,-.36f}}};
    const std::array<std::pair<float,float>,5>half{{{.29f,.082f},{.29f,.082f},{.29f,.082f},{.29f,.082f},{.34f,.078f}}};
    std::vector<Segment>s;
    for(int i=0;i<5;++i){addRect(s,c,r,u,pos[i].first,pos[i].second,half[i].first,half[i].second);text(ctx,mat,names[i],c,r,u,pos[i].first,pos[i].second-.008f,.0077f,alpha);}
    worldanalysis::worldoverlay::drawLines(ctx,mat,s,white(alpha*.86f),1.3f);
    text(ctx,mat,"ANALYSIS MENU",c,r,u,0,.49f,.0116f,alpha);
    text(ctx,mat,"LOCAL TECHNICAL OVERVIEW",c,r,u,0,.30f,.0064f,alpha*.70f);
}

void render(void*lr,void*screen,void*){
    if(!g_mod||!g_mod->enabled)return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color,g_mod->backgroundColor,g_mod->backgroundOpacity);
    worldanalysis::worldoverlay::RenderContext ctx{};if(!worldanalysis::worldoverlay::makeContext(lr,screen,ctx))return;
    void*mat=worldanalysis::worldoverlay::depthMaterial();if(!mat)return;
    std::lock_guard lock(g_mutex);auto now=Clock::now();if(finite(ctx.camera))g_camera=ctx.camera;
    for(const auto&s:g_shards){const float age=std::chrono::duration<float>(now-s.created).count();if(age>.42f)continue;const float a=smooth(1.f-age/.42f);const Vec3 pos=add(s.origin,add(mul(s.velocity,age),Vec3{0,-.28f*age*age,0}));Vec3 sr{},su{};worldanalysis::worldoverlay::billboardBasis(pos,ctx.camera,sr,su);const float q=(.055f-.026f*(age/.42f))*(1.f+.15f*std::sin(age*20.f+s.spin));const float tw=s.spin+age*11.f;Vec3 d=norm(add(mul(sr,std::cos(tw)),mul(su,std::sin(tw))),sr),e=norm(add(mul(sr,-std::sin(tw)),mul(su,std::cos(tw))),su);worldanalysis::worldoverlay::drawLines(ctx,mat,{{add(pos,mul(d,-q)),add(pos,mul(d,q))},{add(pos,mul(e,-q*.70f)),add(pos,mul(e,q*.70f))}},white(a),1.5f);}
    if(!g_panelValid||now<g_hiddenUntil)return;
    float alpha=smooth(g_appear),w=currentWidth(),h=currentHeight();Vec3 r{},u{};worldanalysis::worldoverlay::billboardBasis(g_panel,ctx.camera,r,u);
    auto p=[&](float x,float y){return add(g_panel,add(mul(r,x),mul(u,y)));};
    worldanalysis::worldoverlay::drawQuads(ctx,mat,{{p(-w*.5f,-h*.5f),p(w*.5f,-h*.5f),p(w*.5f,h*.5f),p(-w*.5f,h*.5f)}},(static_cast<std::uint32_t>(alpha*31.f)<<24));
    std::vector<Segment>outline;addRect(outline,g_panel,r,u,0,0,w*.5f,h*.5f);
    if(!g_compact){
        // Header/footer separators reserve clean zones for navigation and animation.
        outline.push_back({p(-w*.5f+.08f,h*.5f-.25f),p(w*.5f-.08f,h*.5f-.25f)});
        outline.push_back({p(-w*.5f+.08f,-h*.5f+.20f),p(w*.5f-.08f,-h*.5f+.20f)});
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,outline,white(alpha),1.55f);

    const float t=std::chrono::duration<float>(now.time_since_epoch()).count();
    if(g_compact){
        text(ctx,mat,"MENU",g_panel,r,u,0,-.006f,.0095f,alpha);
        std::vector<Segment>pulse;const float inset=.055f+.012f*(.5f+.5f*std::sin(t*3.2f));addRect(pulse,g_panel,r,u,0,0,w*.5f-inset,h*.5f-inset);worldanalysis::worldoverlay::drawLines(ctx,mat,pulse,white(alpha*.45f),1.0f);
        return;
    }

    const float expandedAlpha=alpha*(g_transitionFromCompact?smooth(g_transition):1.f);
    drawCornerAnimation(ctx,mat,g_panel,r,u,expandedAlpha,t,w,h);

    const float closeX=w*.5f-.12f,closeY=h*.5f-.12f;
    std::vector<Segment>x;addRect(x,g_panel,r,u,closeX,closeY,.065f,.065f);
    x.push_back({p(closeX-.032f,closeY-.032f),p(closeX+.032f,closeY+.032f)});
    x.push_back({p(closeX+.032f,closeY-.032f),p(closeX-.032f,closeY+.032f)});
    worldanalysis::worldoverlay::drawLines(ctx,mat,x,white(expandedAlpha*.92f),1.30f);

    const float content=smooth(std::clamp(g_transition/.45f,0.f,1.f));
    if(g_page==Page::Home)drawHome(ctx,mat,g_panel,r,u,alpha*content);
    else{
        const float menuX=-w*.5f+.22f,menuY=h*.5f-.17f;
        std::vector<Segment>b;addRect(b,g_panel,r,u,menuX,menuY,.18f,.080f);
        worldanalysis::worldoverlay::drawLines(ctx,mat,b,white(alpha*.82f),1.2f);
        text(ctx,mat,"MENU",g_panel,r,u,menuX,menuY-.005f,.0072f,alpha);
        drawPage(ctx,mat,g_panel,r,u,alpha*content,t);
    }
}

void reset(){std::lock_guard lock(g_mutex);g_player=nullptr;g_data={};g_page=Page::Home;g_previousPage=Page::Home;g_targetPage=Page::Home;g_compact=true;g_transitionFromCompact=false;g_transition=1;g_panel={};g_camera={};g_viewOrigin={};g_viewDirection={};g_viewValid=false;g_panelValid=false;g_orbitInit=false;g_orbit=0;g_orbitDir=-1;g_appear=0;g_hiddenUntil={};g_animChanged=Clock::now();g_cornerVisual=static_cast<int>(rnd()%6);g_action=0;g_shards.clear();}
}

AnalysisMenuModule::AnalysisMenuModule():Module("Analysis Menu","A local telemetry console with animated Player, Time, Equipment, Environment and Systems views. The compact launcher and expanded console remain depth-tested and local-only."){g_mod=this;showInMenu=true;exposeBackgroundStyle=true;}
AnalysisMenuModule::~AnalysisMenuModule(){if(g_mod==this)g_mod=nullptr;}
void AnalysisMenuModule::onInit(){if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::LevelGetHitResult))g_getHitResult=reinterpret_cast<LevelGetHitResultFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetHealth))g_getHealth=reinterpret_cast<ActorGetHealthFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetMaxHealth))g_getMaxHealth=reinterpret_cast<ActorGetMaxHealthFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::AttributesGetInstanceByName))g_getAttribute=reinterpret_cast<AttributesGetByNameFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetEquipmentItem))g_getEquipment=reinterpret_cast<ActorGetEquipmentItemFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBrightness))g_getBrightness=reinterpret_cast<BrightnessFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBiome))g_getBiome=reinterpret_cast<GetBiomeFn>(a);if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BiomeGetTemperature))g_getTemperature=reinterpret_cast<BiomeTemperatureFn>(a);worldanalysis::items::initialize();worldanalysis::worldoverlay::initialize();worldanalysis::worldoverlay::registerRenderCallback(render);worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});}
void AnalysisMenuModule::onEnable(){reset();}
void AnalysisMenuModule::onDisable(){reset();}
bool AnalysisMenuModule::onMouseEvent(int button,bool down){if(!enabled||!down)return false;if(button==1||button==2){int a=panelHit();if(a)g_action.store(a,std::memory_order_release);}return false;}
bool AnalysisMenuModule::onTouchInput(int action,int pointer,float x,float y){if(!enabled)return false;std::lock_guard lock(g_touchMutex);if(action==0){g_touch={true,pointer,x,y,0,Clock::now()};return false;}if(!g_touch.active||g_touch.pointer!=pointer)return false;float dx=x-g_touch.x,dy=y-g_touch.y;g_touch.maxMove2=std::max(g_touch.maxMove2,dx*dx+dy*dy);if(action==1){float age=std::chrono::duration<float>(Clock::now()-g_touch.started).count();if(age<=.32f&&g_touch.maxMove2<=144.f){int a=panelHit();if(a)g_action.store(a,std::memory_order_release);}g_touch={};}else if(action==3)g_touch={};return false;}
void AnalysisMenuModule::handleTick(worldanalysis::sdk::Player*p){if(!enabled||!p)return;std::lock_guard lock(g_mutex);g_player=p;cacheRay(p);Vec3 target=orbitTarget(p);if(finite(target)){if(!g_panelValid){g_panel=target;g_panelValid=true;}else g_panel=add(g_panel,mul(sub(target,g_panel),.095f));}else g_panelValid=false;auto now=Clock::now();if(now>=g_hiddenUntil&&g_panelValid)g_appear=std::min(1.f,g_appear+.07f);if(g_transition<1.f)g_transition=std::min(1.f,std::chrono::duration<float>(now-g_transitionStarted).count()/.45f);if(std::chrono::duration<float>(now-g_animChanged).count()>5.5f+rnd01()*2.f){g_cornerVisual=static_cast<int>(rnd()%6);g_animChanged=now;}int a=g_action.exchange(0,std::memory_order_acq_rel);if(a)handleAction(a);sample(p);}
void AnalysisMenuModule::loadConfig(const nlohmann::json&j){Module::loadConfig(j);hiddenDuration=std::clamp(j.value("hiddenDuration",hiddenDuration),3.f,600.f);}
void AnalysisMenuModule::saveConfig(nlohmann::json&j){Module::saveConfig(j);j["hiddenDuration"]=hiddenDuration;}
