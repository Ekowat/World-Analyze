#include "analyze.hpp"

#include "worldoverlay.hpp"
#include "core/ui/ColorUtil.hpp"
#include <pl/ModMenu.hpp>
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/events/ScreenStateEvent.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/DimensionIdentity.hpp>
#include <worldanalysis/sdk/world/HitResult.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Vec2=worldanalysis::sdk::Vec2;
using Vec3=worldanalysis::sdk::Vec3;
using Clock=std::chrono::steady_clock;
using Segment=worldanalysis::worldoverlay::Segment;
using Quad=worldanalysis::worldoverlay::Quad;

struct BlockPosRaw {int x=0,y=0,z=0;};
struct BlockTarget {bool valid=false;BlockPosRaw pos{};void* dimension=nullptr;};
struct ResultCard {std::string title;std::vector<std::string> lines;Vec3 position{};int visual=0;};
struct AnalysisResult {bool valid=false;BlockPosRaw pos{};Vec3 center{};std::string name;std::string identifier;std::vector<ResultCard> cards;Clock::time_point shownAt{};Clock::time_point expires{};};
struct Shard {Vec3 origin{};Vec3 velocity{};float spin=0;Clock::time_point created{};};

enum class Phase : std::uint8_t {Idle,Analyzing,Results};

using LevelGetHitResultFn=worldanalysis::sdk::HitResult*(*)(void*);
using GetBlockFn=void*(*)(void*,const BlockPosRaw&);
using BrightnessFn=float(*)(void*,const BlockPosRaw&);
using SolidFn=bool(*)(void*,const BlockPosRaw*);
using GetBiomeFn=void*(*)(void*,const void*);
using BiomeTemperatureFn=float(*)(void*,void*,void*);

AnalyzeModule* g_mod=nullptr;
LevelGetHitResultFn g_getHitResult=nullptr;
GetBlockFn g_getBlock=nullptr;
BrightnessFn g_getBrightness=nullptr;
SolidFn g_isSolid=nullptr;
GetBiomeFn g_getBiome=nullptr;
BiomeTemperatureFn g_getTemperature=nullptr;
worldanalysis::sdk::Player* g_player=nullptr;
void* g_region=nullptr;
worldanalysis::sdk::Dimension* g_dimension=nullptr;
BlockTarget g_last{};
BlockTarget g_pendingPlaced{};
BlockTarget g_cachedAim{};
Vec3 g_cachedAimHit{};
std::string g_selectedName;
int g_pendingTicks=0;
Phase g_phase=Phase::Idle;
Clock::time_point g_analysisStarted{};
Clock::time_point g_hiddenUntil{};
Clock::time_point g_statusUntil{};
Clock::time_point g_buttonPressedAt{};
std::string g_status;
AnalysisResult g_result{};
std::vector<Shard> g_shards;
Vec3 g_panelCenter{};
Vec3 g_panelEye{};
Vec2 g_panelRotation{};
Vec3 g_viewRayOrigin{};
Vec3 g_viewRayDirection{};
bool g_viewRayValid=false;
float g_orbitAngle=0.f;
float g_orbitDirection=1.f;
bool g_orbitInitialized=false;
Clock::time_point g_orbitUpdated{};
bool g_panelValid=false;
float g_panelAppear=0.f;
std::atomic_bool g_primaryInput{false};
std::atomic_bool g_secondaryInput{false};
std::atomic_bool g_touchInput{false};
std::atomic_bool g_containerInteraction{false};
std::atomic_int g_panelAction{0};
std::mutex g_touchGestureMutex;
struct TouchGesture { bool active=false; int pointerId=-1; float startX=0.f,startY=0.f,maxMove2=0.f; Clock::time_point started{}; };
TouchGesture g_touchGesture{};
Vec3 g_analysisPanel{};
std::mutex g_mutex;

constexpr float kPi=3.14159265358979323846f;
constexpr float kPanelWidth=1.70f;
constexpr float kPanelHeight=.78f;
constexpr float kAnalyzeSeconds=4.0f;
constexpr std::size_t kBlockSourceGetBlockActorSlot=4;

bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024.f&&p.y<4096.f;}
Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(const Vec3&a,const Vec3&b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float dot(const Vec3&a,const Vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(const Vec3&a,const Vec3&b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float length(const Vec3&v){return std::sqrt(dot(v,v));}
Vec3 norm(const Vec3&v,const Vec3&fallback={1,0,0}){const float l=length(v);return l>.0001f?mul(v,1.f/l):fallback;}
float smooth(float v){v=std::clamp(v,0.f,1.f);return v*v*(3.f-2.f*v);}
std::uint32_t white(float a){return(static_cast<std::uint32_t>(std::clamp(a,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu;}
bool samePos(const BlockPosRaw&a,const BlockPosRaw&b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}

bool plausibleFunction(void* fn){return worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(fn),4);}
std::string_view blockIdentifier(const void*block){
    if(!block)return{};const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);if(!worldanalysis::worldoverlay::plausible(type))return{};
    const auto address=type+worldanalysis::sdk::offsets::BlockType::mNameInfo+worldanalysis::sdk::offsets::NameInfo::mFullName+worldanalysis::sdk::offsets::HashedString::mString;
    const auto* value=reinterpret_cast<const std::string*>(address);if(value->size()>128||(!value->empty()&&!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(value->data()),1)))return{};return{value->data(),value->size()};
}
bool airId(std::string_view id){return id.empty()||id=="minecraft:air"||id=="minecraft:cave_air"||id=="minecraft:void_air";}
std::string shortId(std::string_view id){if(id.starts_with("minecraft:"))id.remove_prefix(10);return std::string(id);}
std::string displayName(std::string_view id){if(id.starts_with("minecraft:"))id.remove_prefix(10);std::string out;bool cap=true;for(char c:id){if(out.size()>=28)break;if(c=='_'||c=='-'){if(!out.empty()&&out.back()!=' ')out.push_back(' ');cap=true;}else if(c>='a'&&c<='z'){out.push_back(cap?static_cast<char>(c-'a'+'A'):c);cap=false;}else{out.push_back(c);cap=false;}}return out.empty()?"BLOCK":out;}
std::string one(float v){char buf[32];std::snprintf(buf,sizeof(buf),"%.1f",static_cast<double>(v));return buf;}
std::string two(float v){char buf[32];std::snprintf(buf,sizeof(buf),"%.2f",static_cast<double>(v));return buf;}

void* blockAt(const BlockPosRaw&pos){return g_region&&g_getBlock?g_getBlock(g_region,pos):nullptr;}
bool blockExists(const BlockPosRaw&pos){void*b=blockAt(pos);return b&&!airId(blockIdentifier(b));}

BlockPosRaw inferAdjacent(const BlockPosRaw& block,const Vec3& hit){
    const float fx=hit.x-static_cast<float>(block.x),fy=hit.y-static_cast<float>(block.y),fz=hit.z-static_cast<float>(block.z);
    const std::array<std::pair<float,BlockPosRaw>,6> candidates{{{std::abs(fx),{-1,0,0}},{std::abs(fx-1.f),{1,0,0}},{std::abs(fy),{0,-1,0}},{std::abs(fy-1.f),{0,1,0}},{std::abs(fz),{0,0,-1}},{std::abs(fz-1.f),{0,0,1}}}};
    auto best=std::min_element(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    return{block.x+best->second.x,block.y+best->second.y,block.z+best->second.z};
}

bool currentHitBlock(BlockPosRaw& pos,Vec3& hit){
    if(!g_player||!g_getHitResult)return false;auto* level=g_player->level();auto* h=level?g_getHitResult(level):nullptr;if(!h||h->type()!=0)return false;
    pos=*reinterpret_cast<const BlockPosRaw*>(reinterpret_cast<const std::byte*>(h)+worldanalysis::sdk::offsets::HitResult::mBlock);hit=h->position();return blockExists(pos);
}

std::string biomeName(void* biome){
    if(!biome||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(biome)))return"UNKNOWN";
    const auto* value=reinterpret_cast<const std::string*>(reinterpret_cast<std::uintptr_t>(biome)+worldanalysis::sdk::offsets::Biome::mHash+sizeof(std::uint64_t));
    if(value->size()>96||(!value->empty()&&!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(value->data()),1)))return"UNKNOWN";
    return value->empty()?"UNKNOWN":displayName(*value);
}

void* blockActorAt(const BlockPosRaw& pos){
    if(!g_region||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(g_region)))return nullptr;auto** table=*reinterpret_cast<void***>(g_region);if(!table||!plausibleFunction(table[kBlockSourceGetBlockActorSlot]))return nullptr;
    using Fn=void*(*)(void*,const BlockPosRaw&);return reinterpret_cast<Fn>(table[kBlockSourceGetBlockActorSlot])(g_region,pos);
}

bool pointAir(const Vec3&p){BlockPosRaw bp{static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.y)),static_cast<int>(std::floor(p.z))};void*b=blockAt(bp);return !b||airId(blockIdentifier(b));}
bool panelClear(const Vec3& p){
    // The result cards are about 1.44 blocks wide and the orbit panel is 1.70.
    // Check a conservative world-space envelope so a camera-facing billboard
    // cannot rotate its edges into a neighboring solid block.
    const Vec3 samples[]{
        p, add(p,{.88f,0,0}), add(p,{-.88f,0,0}), add(p,{0,0,.88f}), add(p,{0,0,-.88f}),
        add(p,{0,.44f,0}), add(p,{0,-.44f,0}),
        add(p,{.62f,0,.62f}), add(p,{.62f,0,-.62f}),
        add(p,{-.62f,0,.62f}), add(p,{-.62f,0,-.62f})
    };
    for(const auto& sample:samples)if(!pointAir(sample))return false;
    return true;
}

Vec3 choosePanelPosition(const Vec3& center,const Vec3& preferred,std::size_t index){
    std::array<Vec3,12> candidates{{preferred,add(center,{1.45f,1.35f,0}),add(center,{-1.45f,1.35f,0}),add(center,{0,1.65f,1.35f}),add(center,{0,1.65f,-1.35f}),add(center,{1.55f,.20f,0}),add(center,{-1.55f,.20f,0}),add(center,{0,.45f,1.60f}),add(center,{0,.45f,-1.60f}),add(center,{1.15f,2.10f,0}),add(center,{-1.15f,2.10f,0}),add(center,{0,2.30f,0})}};
    std::rotate(candidates.begin(),candidates.begin()+static_cast<std::ptrdiff_t>(index%candidates.size()),candidates.end());
    for(const auto& p:candidates)if(panelClear(p))return p;const float nan=std::numeric_limits<float>::quiet_NaN();return{nan,nan,nan};
}

void layoutCards(const Vec3& center,std::vector<ResultCard>& cards){
    // Build a fixed camera-relative 3x3 information plane around the selected
    // block. Every card owns a unique slot; unlike the old candidate rotation,
    // no two cards can choose the same position and overlap each other.
    Vec3 forward=g_viewRayValid?g_viewRayDirection:Vec3{0,0,1};forward.y=0.f;forward=norm(forward,{0,0,1});
    const Vec3 toward=mul(forward,-1.f);const Vec3 right=norm(cross({0,1,0},toward),{1,0,0});
    struct Slot{float x,y;};
    static constexpr std::array<Slot,8> slots{{
        {-1.68f,2.05f},{0.f,2.25f},{1.68f,2.05f},
        {-1.92f,1.15f},{1.92f,1.15f},
        {-1.68f,.28f},{0.f,.24f},{1.68f,.28f}
    }};
    for(std::size_t i=0;i<cards.size()&&i<slots.size();++i){
        const auto slot=slots[i];bool placed=false;
        for(int attempt=0;attempt<5&&!placed;++attempt){
            const float towardDistance=1.15f+.52f*static_cast<float>(attempt);
            const float spread=1.f+.07f*static_cast<float>(attempt);
            Vec3 p=add(center,add(mul(right,slot.x*spread),add(Vec3{0,slot.y+.10f*static_cast<float>(attempt),0},mul(toward,towardDistance))));
            if(panelClear(p)){cards[i].position=p;placed=true;}
        }
        if(!placed){const float nan=std::numeric_limits<float>::quiet_NaN();cards[i].position={nan,nan,nan};}
    }
    cards.erase(std::remove_if(cards.begin(),cards.end(),[](const ResultCard& c){return !finite(c.position);}),cards.end());
}

std::vector<ResultCard> buildCards(const BlockTarget& target,worldanalysis::sdk::Player* player){
    std::vector<ResultCard> cards;void* block=blockAt(target.pos);if(!block)return cards;const auto id=blockIdentifier(block);if(airId(id))return cards;
    const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    const int legacy=worldanalysis::worldoverlay::plausible(type)?static_cast<int>(*reinterpret_cast<const std::int16_t*>(type+worldanalysis::sdk::offsets::BlockType::mId)):0;
    const auto network=*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mNetworkId);
    const bool solid=g_isSolid?g_isSolid(g_region,&target.pos):false;const float bright=g_getBrightness?g_getBrightness(g_region,target.pos):-1.f;
    void* biome=g_getBiome?g_getBiome(g_region,&target.pos):nullptr;const float temp=(biome&&g_getTemperature)?g_getTemperature(biome,g_region,const_cast<BlockPosRaw*>(&target.pos)):999.f;
    const Vec3 center{target.pos.x+.5f,target.pos.y+.5f,target.pos.z+.5f};const Vec3 pp=player?player->position():center;const float dist=length(sub(center,pp));
    const auto dim=worldanalysis::sdk::dimension_identity::displayName(worldanalysis::sdk::dimension_identity::kind(target.dimension));
    int exposed=0;std::array<std::string,6> neighbors{};const std::array<BlockPosRaw,6> offsets{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    for(std::size_t i=0;i<offsets.size();++i){BlockPosRaw p{target.pos.x+offsets[i].x,target.pos.y+offsets[i].y,target.pos.z+offsets[i].z};auto* nb=blockAt(p);auto nid=blockIdentifier(nb);if(airId(nid)){++exposed;neighbors[i]="AIR";}else neighbors[i]=shortId(nid).substr(0,16);}
    const bool hasActor=blockActorAt(target.pos)!=nullptr;const std::string full(id);
    auto addCard=[&](std::string title,std::initializer_list<std::string> lines,int visual){cards.push_back({std::move(title),std::vector<std::string>(lines),{},visual});};
    addCard("IDENTITY",{displayName(id),full},0);
    addCard("BLOCK IDS",{"LEGACY "+std::to_string(legacy),"NETWORK "+std::to_string(network)},1);
    // Exact block coordinates are intentionally omitted. Analyze only reports
    // local context for a block the player has directly interacted with.
    addCard("CONTEXT",{std::string("DIMENSION ")+std::string(dim),"LOCAL RANGE "+one(dist)+"M"},2);
    addCard("PHYSICAL",{std::string("SOLID ")+(solid?"YES":"NO"),"EXPOSED "+std::to_string(exposed)+" / 6"},3);
    addCard("ENVIRONMENT",{"BRIGHTNESS "+(bright>=0.f?two(bright):std::string("N/A")),"BIOME "+biomeName(biome)},4);
    addCard("THERMAL / ENTITY",{std::string("TEMP ")+(temp<900.f?two(temp):"N/A"),std::string("BLOCK ENTITY ")+(hasActor?"YES":"NO")},5);
    addCard("NEIGHBORS X/Y",{"E "+neighbors[0]+"  W "+neighbors[1],"UP "+neighbors[2]+"  DOWN "+neighbors[3]},6);
    addCard("NEIGHBORS Z",{"S "+neighbors[4],"N "+neighbors[5]},7);
    layoutCards(center,cards);
    return cards;
}


void rememberTarget(const BlockPosRaw& pos,const Vec3& hit){
    if(!blockExists(pos))return;
    g_last={true,pos,g_dimension};
    g_selectedName=displayName(blockIdentifier(blockAt(pos)));
    // Only arm placement tracking when the adjacent destination was actually
    // air at interaction time. This prevents an existing neighbor from
    // overwriting the block the player just selected on the following tick.
    const BlockPosRaw adjacent=inferAdjacent(pos,hit);
    if(!blockExists(adjacent)){g_pendingPlaced={true,adjacent,g_dimension};g_pendingTicks=8;}
    else{g_pendingPlaced={};g_pendingTicks=0;}
    g_status="TARGET LOCKED";g_statusUntil=Clock::now()+std::chrono::milliseconds(850);
}

void startAnalysis(){
    const auto now=Clock::now();
    g_buttonPressedAt=now;
    if(!g_last.valid||!blockExists(g_last.pos)){g_status="NO BLOCK TARGET";g_statusUntil=now+std::chrono::milliseconds(1600);return;}
    const Vec3 c{g_last.pos.x+.5f,g_last.pos.y+.5f,g_last.pos.z+.5f};
    g_analysisPanel=choosePanelPosition(c,add(c,{0,1.55f,0}),0);
    g_phase=Phase::Analyzing;g_analysisStarted=now;g_result={};
    // Immediate local acknowledgement while the world-space scan panel appears.
    g_status="ANALYZING...";g_statusUntil=now+std::chrono::milliseconds(700);
}

void shatterPanel(){
    const auto now=Clock::now();
    g_shards.clear();
    Vec3 right{},up{};worldanalysis::worldoverlay::billboardBasis(g_panelCenter,g_panelEye,right,up);
    const Vec3 normal=norm(cross(right,up),{0,0,1});
    // Cover the full panel footprint with fragments so the break effect is the
    // same size as the container instead of a tiny burst from its center.
    constexpr int cols=6,rows=4;
    for(int y=0;y<rows;++y)for(int x=0;x<cols;++x){
        const float fx=-kPanelWidth*.5f+(static_cast<float>(x)+.5f)*(kPanelWidth/cols);
        const float fy=-kPanelHeight*.5f+(static_cast<float>(y)+.5f)*(kPanelHeight/rows);
        const Vec3 origin=add(g_panelCenter,add(mul(right,fx),mul(up,fy)));
        Vec3 outward=norm(add(mul(right,fx),mul(up,fy)),right);
        const float speed=.80f+.11f*static_cast<float>((x+y)%4);
        Vec3 velocity=add(mul(outward,speed),add(mul(up,.10f+.035f*y),mul(normal,.10f*((x+y)&1?1.f:-1.f))));
        g_shards.push_back({origin,velocity,(x*1.7f+y*.9f),now});
    }
    g_hiddenUntil=now+std::chrono::milliseconds(static_cast<int>(std::clamp(g_mod?g_mod->hiddenDuration:5.f,2.f,20.f)*1000.f));
    g_panelAppear=0.f;
}

Vec3 lookDirection(const Vec2& r){const float pitch=r.x*kPi/180.f,yaw=r.y*kPi/180.f,cp=std::cos(pitch);return norm({-std::sin(yaw)*cp,-std::sin(pitch),std::cos(yaw)*cp},{0,0,1});}

void cacheViewRay(worldanalysis::sdk::Player* player){
    if(!player){g_viewRayValid=false;return;}
    Vec3 origin=add(player->position(),{0,1.62f,0});
    Vec3 direction=lookDirection(player->rotation());
    if(auto* level=player->level()){
        if(auto* h=g_getHitResult?g_getHitResult(level):nullptr){
            const Vec3 start=h->startPosition(),end=h->position();const Vec3 delta=sub(end,start);
            if(finite(start)&&finite(end)&&length(delta)>.05f){origin=start;direction=norm(delta,direction);}
        }
    }
    if(finite(origin)&&finite(direction)){g_viewRayOrigin=origin;g_viewRayDirection=direction;g_viewRayValid=true;g_panelEye=origin;}
    else g_viewRayValid=false;
}

int cachedPanelHit(){
    std::lock_guard lock(g_mutex);
    if(!g_panelValid||Clock::now()<g_hiddenUntil)return 0;
    const Vec3 origin=g_viewRayValid?g_viewRayOrigin:g_panelEye;
    const Vec3 ray=g_viewRayValid?g_viewRayDirection:lookDirection(g_panelRotation);
    if(!finite(origin)||!finite(ray))return 0;
    Vec3 right{},up{};worldanalysis::worldoverlay::billboardBasis(g_panelCenter,origin,right,up);
    const Vec3 normal=norm(cross(right,up),{0,0,1});const float denom=dot(ray,normal);
    if(std::abs(denom)<.0001f)return 0;
    const float t=dot(sub(g_panelCenter,origin),normal)/denom;if(t<0.f||t>7.f)return 0;
    const Vec3 hit=add(origin,mul(ray,t)),local=sub(hit,g_panelCenter);const float x=dot(local,right),y=dot(local,up);
    if(std::abs(x)>kPanelWidth*.5f+.06f||std::abs(y)>kPanelHeight*.5f+.06f)return 0;
    // Make the entire drawn controls interactive, with a small forgiveness
    // margin for touch/crosshair quantization. The X uses the full top-right
    // square and ANALYZE uses the whole outlined button, not a center point.
    if(x>=.52f&&x<=.84f&&y>=.12f&&y<=.41f)return 2;
    if(x>=-.64f&&x<=.64f&&y>=-.31f&&y<=.09f)return 1;
    return 0;
}

void handlePanelAction(int action){if(action==1)startAnalysis();else if(action==2)shatterPanel();}

Vec3 orbitTarget(worldanalysis::sdk::Player* player){
    const Vec3 p=player->position();const auto now=Clock::now();
    Vec3 forward=g_viewRayValid?g_viewRayDirection:lookDirection(player->rotation());forward.y=0.f;forward=norm(forward,{0,0,1});
    const float frontAngle=std::atan2(forward.x,-forward.z);
    if(!g_orbitInitialized){
        // Begin in a rear-side position. From here the panel has its own
        // world-space orbit; turning the camera no longer drags it around.
        g_orbitAngle=frontAngle+2.20f;g_orbitDirection=1.f;g_orbitUpdated=now;g_orbitInitialized=true;
    }
    float dt=std::clamp(std::chrono::duration<float>(now-g_orbitUpdated).count(),0.f,.10f);g_orbitUpdated=now;
    auto radial=[&](float angle){return Vec3{std::sin(angle),0.f,-std::cos(angle)};};
    const Vec3 currentDir=radial(g_orbitAngle);
    const bool playerLookingAtPanel=dot(currentDir,forward)>.84f;
    if(!playerLookingAtPanel){
        const float speed=.095f;float proposed=g_orbitAngle+g_orbitDirection*speed*dt;
        // Do not let passive orbit motion cross the direct-front cone. Bounce
        // along its edge instead. If the player deliberately turns to look at
        // the panel, the current position is frozen until they look away.
        if(dot(radial(proposed),forward)>.70f){g_orbitDirection=-g_orbitDirection;proposed=g_orbitAngle+g_orbitDirection*speed*dt;}
        g_orbitAngle=proposed;
    }
    const float radius=2.15f;Vec3 wanted{p.x+std::sin(g_orbitAngle)*radius,p.y+1.05f,p.z-std::cos(g_orbitAngle)*radius};
    if(panelClear(wanted))return wanted;
    for(float radiusTry:{1.65f,1.95f,2.35f})for(float yOff:{.85f,1.20f,1.55f})for(float delta:{.28f,-.28f,.58f,-.58f,.90f,-.90f}){
        const float candidateAngle=g_orbitAngle+delta;Vec3 c{p.x+std::sin(candidateAngle)*radiusTry,p.y+yOff,p.z-std::cos(candidateAngle)*radiusTry};
        if(panelClear(c)){g_orbitAngle=candidateAngle;return c;}
    }
    const float nan=std::numeric_limits<float>::quiet_NaN();return{nan,nan,nan};
}

void appendBlockBox(std::vector<Segment>& out,const Vec3& c,float grow){const float h=.5f+grow;Vec3 p[8];int n=0;for(int y:{-1,1})for(int z:{-1,1})for(int x:{-1,1})p[n++]={c.x+x*h,c.y+y*h,c.z+z*h};auto e=[&](int a,int b){out.push_back({p[a],p[b]});};e(0,1);e(2,3);e(4,5);e(6,7);e(0,2);e(1,3);e(4,6);e(5,7);e(0,4);e(1,5);e(2,6);e(3,7);}

void drawMainPanel(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat){
    std::lock_guard lock(g_mutex);const auto now=Clock::now();
    if(finite(ctx.camera))g_panelEye=ctx.camera;

    // Fast full-size shatter: fragment origins cover the entire old panel and
    // disperse for less than half a second.
    for(const auto& s:g_shards){
        const float age=std::chrono::duration<float>(now-s.created).count();if(age>.46f)continue;
        const float a=smooth(1.f-age/.46f);
        const Vec3 pos=add(s.origin,add(mul(s.velocity,age),Vec3{0,-.30f*age*age,0}));
        Vec3 r{},u{};worldanalysis::worldoverlay::billboardBasis(pos,ctx.camera,r,u);
        const float q=(.070f-.035f*(age/.46f))*(1.f+.18f*std::sin(age*18.f+s.spin));
        const float twist=s.spin+age*10.f;
        Vec3 d=norm(add(mul(r,std::cos(twist)),mul(u,std::sin(twist))),r);
        Vec3 e=norm(add(mul(r,-std::sin(twist)),mul(u,std::cos(twist))),u);
        worldanalysis::worldoverlay::drawLines(ctx,mat,{{add(pos,mul(d,-q)),add(pos,mul(d,q))},{add(pos,mul(e,-q*.70f)),add(pos,mul(e,q*.70f))}},white(a),1.6f);
    }
    if(g_mod&&g_mod->onScreenButton)return;
    if(!g_panelValid||now<g_hiddenUntil)return;

    const float alpha=smooth(g_panelAppear);Vec3 right{},up{};worldanalysis::worldoverlay::billboardBasis(g_panelCenter,ctx.camera,right,up);
    const float w=kPanelWidth*.5f*alpha,h=kPanelHeight*.5f*alpha;auto p=[&](float x,float y){return add(g_panelCenter,add(mul(right,x),mul(up,y)));};
    worldanalysis::worldoverlay::drawQuads(ctx,mat,{{p(-w,-h),p(w,-h),p(w,h),p(-w,h)}},(static_cast<std::uint32_t>(alpha*32.f)<<24));

    const float pressAge=std::chrono::duration<float>(now-g_buttonPressedAt).count();
    const float press=(pressAge>=0.f&&pressAge<.24f)?smooth(1.f-pressAge/.24f):0.f;
    const float bx=.56f-.035f*press,by=.145f-.025f*press,buttonY=-.105f-.012f*press;
    std::vector<Segment> lines{{p(-w,-h),p(w,-h)},{p(w,-h),p(w,h)},{p(w,h),p(-w,h)},{p(-w,h),p(-w,-h)}};
    const float pulse=.02f+.02f*std::sin(std::chrono::duration<float>(now.time_since_epoch()).count()*2.4f);
    lines.push_back({p(-bx,buttonY-by),p(bx,buttonY-by)});lines.push_back({p(bx,buttonY-by),p(bx,buttonY+by)});lines.push_back({p(bx,buttonY+by),p(-bx,buttonY+by)});lines.push_back({p(-bx,buttonY+by),p(-bx,buttonY-by)});
    lines.push_back({p(.59f,.18f),p(.77f,.36f)});lines.push_back({p(.77f,.18f),p(.59f,.36f)});
    for(int i=0;i<5;++i){const float x=-.55f+i*.27f;lines.push_back({p(x,.20f+pulse),p(x+.08f,.20f+pulse)});}
    if(press>0.f){const float sweep=-bx+2.f*bx*press;lines.push_back({p(sweep,buttonY-by+.035f),p(sweep,buttonY+by-.035f)});}
    worldanalysis::worldoverlay::drawLines(ctx,mat,lines,white(alpha),1.7f+press*.45f);
    if(press>0.f){worldanalysis::worldoverlay::drawQuads(ctx,mat,{{p(-bx,buttonY-by),p(bx,buttonY-by),p(bx,buttonY+by),p(-bx,buttonY+by)}},(static_cast<std::uint32_t>(alpha*(28.f+22.f*press))<<24));}

    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,"ANALYZE",p(0,.24f),right,up,.014f,white(alpha),false);
    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,press>0.f?"ANALYZING":"ANALYZE",p(0,buttonY-.003f),right,up,.012f,white(alpha),false);
    std::string target=g_last.valid?("TARGET "+(g_selectedName.empty()?std::string("LOCKED"):g_selectedName)):"TARGET NONE";
    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,target,p(0,.095f),right,up,.0078f,white(alpha*.72f),false);
    if(!g_status.empty()&&now<g_statusUntil)worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,g_status,p(0,-.34f),right,up,.008f,white(alpha),false);
}

void drawSelectedBlock(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat){
    std::lock_guard lock(g_mutex);if(!g_last.valid)return;const float t=std::chrono::duration<float>(Clock::now().time_since_epoch()).count();
    const Vec3 c{g_last.pos.x+.5f,g_last.pos.y+.5f,g_last.pos.z+.5f};const float pulse=.018f+.012f*(.5f+.5f*std::sin(t*3.2f));std::vector<Segment> lines;appendBlockBox(lines,c,pulse);
    // Animated corner ticks make the current Analyze target obvious without a
    // filled overlay or through-wall material.
    const float q=.16f+.025f*std::sin(t*4.4f);const float h=.515f+pulse;
    for(int sx:{-1,1})for(int sy:{-1,1})for(int sz:{-1,1}){
        Vec3 corner{c.x+sx*h,c.y+sy*h,c.z+sz*h};lines.push_back({corner,add(corner,{sx*-q,0,0})});lines.push_back({corner,add(corner,{0,sy*-q,0})});lines.push_back({corner,add(corner,{0,0,sz*-q})});
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,lines,white(.82f),1.55f);
}

void drawAnalyzing(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat){
    std::lock_guard lock(g_mutex);if(g_phase!=Phase::Analyzing||!g_last.valid)return;const auto now=Clock::now();const float age=std::chrono::duration<float>(now-g_analysisStarted).count();const float progress=std::clamp(age/kAnalyzeSeconds,0.f,1.f);const Vec3 c{g_last.pos.x+.5f,g_last.pos.y+.5f,g_last.pos.z+.5f};
    std::vector<Segment> lines;appendBlockBox(lines,c,.030f+.035f*std::sin(age*4.f));const float ring=.78f+.16f*std::sin(age*2.7f);for(int i=0;i<40;++i){const float a=i*2.f*kPi/40.f,b=(i+1)*2.f*kPi/40.f;if(static_cast<float>(i)>progress*40.f)break;lines.push_back({{c.x+std::cos(a)*ring,c.y+1.02f,c.z+std::sin(a)*ring},{c.x+std::cos(b)*ring,c.y+1.02f,c.z+std::sin(b)*ring}});}worldanalysis::worldoverlay::drawLines(ctx,mat,lines,white(.90f),1.7f);
    Vec3 panel=g_analysisPanel;if(finite(panel)){Vec3 r{},u{};worldanalysis::worldoverlay::billboardBasis(panel,ctx.camera,r,u);auto p=[&](float x,float y){return add(panel,add(mul(r,x),mul(u,y)));};const int dots=static_cast<int>(age*3.f)%4;std::string label="ANALYZING"+std::string(static_cast<std::size_t>(dots),'.');
        std::vector<Segment> ui{{p(-.58f,-.24f),p(.58f,-.24f)},{p(.58f,-.24f),p(.58f,.24f)},{p(.58f,.24f),p(-.58f,.24f)},{p(-.58f,.24f),p(-.58f,-.24f)},{p(-.50f,-.15f),p(-.50f+progress, -.15f)}};
        const float sweep=-.46f+std::fmod(age*.62f,.92f);ui.push_back({p(sweep,-.10f),p(sweep,.10f)});worldanalysis::worldoverlay::drawLines(ctx,mat,ui,white(.92f),1.55f);
        worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,label,p(0,.08f),r,u,.0125f,white(1.f),false);worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,std::to_string(static_cast<int>(progress*100.f))+"%",p(0,-.05f),r,u,.0095f,white(.86f),false);
    }
    const float scan=-.48f+std::fmod(age*.42f,.96f);worldanalysis::worldoverlay::drawLines(ctx,mat,{{{c.x-.52f,c.y+.5f+scan,c.z-.53f},{c.x+.52f,c.y+.5f+scan,c.z-.53f}}},white(.72f),1.3f);
}

void drawCard(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const ResultCard&card,std::size_t index,float alpha,float age){
    Vec3 r{},u{};worldanalysis::worldoverlay::billboardBasis(card.position,ctx.camera,r,u);const float intro=smooth((age-static_cast<float>(index)*.09f)/.30f);if(intro<=.001f)return;const float w=.72f*intro,h=.31f*intro;auto p=[&](float x,float y){return add(card.position,add(mul(r,x),mul(u,y)));};worldanalysis::worldoverlay::drawQuads(ctx,mat,{{p(-w,-h),p(w,-h),p(w,h),p(-w,h)}},(static_cast<std::uint32_t>(alpha*28.f)<<24));std::vector<Segment> seg{{p(-w,-h),p(w,-h)},{p(w,-h),p(w,h)},{p(w,h),p(-w,h)},{p(-w,h),p(-w,-h)}};
    const float phase=age*2.8f+static_cast<float>(card.visual)*.61f;const float cx=-w+.18f,cy=-h+.105f;
    switch(card.visual%8){
        case 0:{ // DNA helix
            Vec3 prevA{},prevB{};bool have=false;for(int i=0;i<7;++i){const float x=cx-.13f+i*.043f;const float wave=std::sin(phase+i*.92f)*.052f;Vec3 a=p(x,cy+wave),b=p(x,cy-wave);if(have){seg.push_back({prevA,a});seg.push_back({prevB,b});}if((i&1)==0)seg.push_back({a,b});prevA=a;prevB=b;have=true;}break;
        }
        case 1:{ // animated linked diamonds
            for(int j=0;j<2;++j){const float ox=(j? .055f:-.055f)+.012f*std::sin(phase+j);const float sy=.070f;Vec3 a=p(cx+ox,cy+sy),b=p(cx+ox+.070f,cy),c=p(cx+ox,cy-sy),d=p(cx+ox-.070f,cy);seg.insert(seg.end(),{{a,b},{b,c},{c,d},{d,a}});}break;
        }
        case 2:{ // compass/range sweep
            const float rr=.095f;seg.push_back({p(cx-rr,cy),p(cx+rr,cy)});seg.push_back({p(cx,cy-rr),p(cx,cy+rr)});const float a=phase*.72f;seg.push_back({p(cx,cy),p(cx+std::cos(a)*rr,cy+std::sin(a)*rr)});for(int i=0;i<4;++i){const float q=i*kPi*.5f;seg.push_back({p(cx+std::cos(q)*rr*.78f,cy+std::sin(q)*rr*.78f),p(cx+std::cos(q)*rr,cy+std::sin(q)*rr)});}break;
        }
        case 3:{ // physical lattice
            for(int i=-1;i<=1;++i){const float d=i*.065f;seg.push_back({p(cx-.105f,cy+d),p(cx+.105f,cy+d)});seg.push_back({p(cx+d,cy-.105f),p(cx+d,cy+.105f)});}const float scan=-.09f+std::fmod(phase*.045f,.18f);seg.push_back({p(cx-.11f,cy+scan),p(cx+.11f,cy+scan)});break;
        }
        case 4:{ // environmental waveform + orbit
            Vec3 prev=p(cx-.12f,cy);for(int i=1;i<=6;++i){const float x=cx-.12f+i*.04f,y=cy+std::sin(phase+i*.9f)*.055f;Vec3 cur=p(x,y);seg.push_back({prev,cur});prev=cur;}const float a=phase;seg.push_back({p(cx,cy),p(cx+std::cos(a)*.105f,cy+std::sin(a)*.075f)});break;
        }
        case 5:{ // thermometer / entity pulse
            seg.push_back({p(cx-.025f,cy-.095f),p(cx-.025f,cy+.090f)});seg.push_back({p(cx+.025f,cy-.095f),p(cx+.025f,cy+.090f)});const float fill=-.075f+.15f*(.5f+.5f*std::sin(phase*.75f));seg.push_back({p(cx-.016f,cy-.075f),p(cx-.016f,cy+fill)});seg.push_back({p(cx+.016f,cy-.075f),p(cx+.016f,cy+fill)});for(int i=0;i<4;++i){const float a=phase+i*kPi*.5f;seg.push_back({p(cx+.10f,cy),p(cx+.10f+std::cos(a)*.05f,cy+std::sin(a)*.05f)});}break;
        }
        case 6:{ // neighbor node network
            const std::array<Vec3,5> n{{p(cx,cy),p(cx-.105f,cy+.065f),p(cx+.105f,cy+.065f),p(cx-.105f,cy-.065f),p(cx+.105f,cy-.065f)}};for(int i=1;i<5;++i){seg.push_back({n[0],n[i]});const float q=.018f+.006f*std::sin(phase+i);seg.push_back({add(n[i],mul(r,-q)),add(n[i],mul(r,q))});seg.push_back({add(n[i],mul(u,-q)),add(n[i],mul(u,q))});}break;
        }
        default:{ // chain / vertical neighbor links
            for(int j=0;j<3;++j){const float y=cy+(j-1)*.065f;const float sway=.018f*std::sin(phase+j);Vec3 a=p(cx-.085f+sway,y),b=p(cx,y+.035f),c=p(cx+.085f-sway,y);seg.push_back({a,b});seg.push_back({b,c});if(j<2)seg.push_back({c,p(cx-.085f-sway,y+.065f)});}break;
        }
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,seg,white(alpha*intro),1.4f);
    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,card.title,p(.10f,.20f),r,u,.0092f,white(alpha*intro),false);float y=.07f;for(const auto& line:card.lines){const float px=worldanalysis::worldoverlay::fitBillboardTextPixelSize(line,1.28f,.0083f,.0060f);worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,line,p(.10f,y),r,u,px,white(alpha*intro*.88f),false);y-=.12f;}
}

void drawResults(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat){
    std::lock_guard lock(g_mutex);if(g_phase!=Phase::Results||!g_result.valid)return;const auto now=Clock::now();const float age=std::chrono::duration<float>(now-g_result.shownAt).count(),remain=std::chrono::duration<float>(g_result.expires-now).count();if(remain<=0)return;const float alpha=smooth(std::min(age/.30f,remain/.45f));
    std::vector<Segment> box;appendBlockBox(box,g_result.center,.035f+.02f*std::sin(age*2.f));worldanalysis::worldoverlay::drawLines(ctx,mat,box,white(alpha*.72f),1.4f);for(std::size_t i=0;i<g_result.cards.size();++i){drawCard(ctx,mat,g_result.cards[i],i,alpha,age);worldanalysis::worldoverlay::drawLines(ctx,mat,{{g_result.center,g_result.cards[i].position}},white(alpha*.28f),.8f);}
}

void renderAnalyze(void* levelRenderer,void* screen,void*){if(!g_mod||!g_mod->enabled)return;worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color,g_mod->backgroundColor,g_mod->backgroundOpacity);worldanalysis::worldoverlay::RenderContext ctx{};if(!worldanalysis::worldoverlay::makeContext(levelRenderer,screen,ctx))return;void*mat=worldanalysis::worldoverlay::depthMaterial();if(!mat)return;drawSelectedBlock(ctx,mat);drawMainPanel(ctx,mat);drawAnalyzing(ctx,mat);drawResults(ctx,mat);}

void resetState(){
    {std::lock_guard lock(g_mutex);g_player=nullptr;g_region=nullptr;g_dimension=nullptr;g_last={};g_pendingPlaced={};g_cachedAim={};g_cachedAimHit={};g_selectedName.clear();g_pendingTicks=0;g_phase=Phase::Idle;g_result={};g_shards.clear();g_panelCenter={};g_panelEye={};g_panelRotation={};g_viewRayOrigin={};g_viewRayDirection={};g_viewRayValid=false;g_orbitAngle=0.f;g_orbitDirection=1.f;g_orbitInitialized=false;g_orbitUpdated={};g_panelValid=false;g_panelAppear=0.f;g_status.clear();g_buttonPressedAt={};g_primaryInput=false;g_secondaryInput=false;g_touchInput=false;g_containerInteraction=false;g_panelAction=0;g_analysisPanel={};}
    {std::lock_guard touchLock(g_touchGestureMutex);g_touchGesture={};}
}
} // namespace


bool queryAnalyzePanelPosition(worldanalysis::sdk::Vec3& out){
    std::lock_guard lock(g_mutex);
    if(!g_mod||!g_mod->enabled||g_mod->onScreenButton||!g_panelValid||Clock::now()<g_hiddenUntil)return false;
    out=g_panelCenter;
    return finite(out);
}

AnalyzeModule::AnalyzeModule():Module("Analyze","Maintains the last block you deliberately interacted with and performs a four-second animated technical scan on demand. The floating world Analyze panel can optionally be replaced by a launcher-native on-screen ANALYZE button; scan results remain depth-tested and local-only."){g_mod=this;showInMenu=true;exposeBackgroundStyle=true;}
AnalyzeModule::~AnalyzeModule(){pl::modmenu::unregisterButton("worldanalysis.Analyze.button");if(g_mod==this)g_mod=nullptr;}
void AnalyzeModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::LevelGetHitResult))g_getHitResult=reinterpret_cast<LevelGetHitResultFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBrightness))g_getBrightness=reinterpret_cast<BrightnessFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceIsSolidBlockingBlock))g_isSolid=reinterpret_cast<SolidFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBiome))g_getBiome=reinterpret_cast<GetBiomeFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BiomeGetTemperature))g_getTemperature=reinterpret_cast<BiomeTemperatureFn>(a);
    worldanalysis::worldoverlay::initialize();worldanalysis::worldoverlay::registerRenderCallback(renderAnalyze);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});
    worldanalysis::events::bus().subscribe<worldanalysis::events::ScreenStateEvent>([](auto&e){if(g_mod&&g_mod->enabled&&e.screen==worldanalysis::events::ScreenKind::Container&&e.phase==worldanalysis::events::ScreenPhase::Opened)g_containerInteraction.store(true,std::memory_order_release);});
}
void AnalyzeModule::onEnable(){resetState();registerLauncherButton();}
void AnalyzeModule::onDisable(){resetState();}

bool AnalyzeModule::onMouseEvent(int button,bool isDown){
    if(!enabled||!isDown)return false;
    // Mouse/controller primary/secondary transitions are real action buttons.
    // Observe them without consuming the event so Minecraft keeps full camera
    // and gameplay control even when the crosshair is over the panel.
    if(button==1||button==2){
        const int action=onScreenButton?0:cachedPanelHit();if(action)g_panelAction.store(action,std::memory_order_release);
        if(button==1)g_primaryInput=true;else g_secondaryInput=true;
    }
    return false;
}
bool AnalyzeModule::onTouchInput(int action,int pointerId,float x,float y){
    if(!enabled)return false;
    // Never consume Android touch input. Consuming a pointer stream is what
    // previously locked camera look while the Analyze billboard was targeted.
    // A panel action is accepted only after a short stationary tap completes;
    // camera drags/movement gestures can never trigger the buttons.
    if(action==0){std::lock_guard lock(g_touchGestureMutex);g_touchGesture={true,pointerId,x,y,0.f,Clock::now()};return false;}
    std::lock_guard lock(g_touchGestureMutex);if(!g_touchGesture.active||g_touchGesture.pointerId!=pointerId)return false;
    const float dx=x-g_touchGesture.startX,dy=y-g_touchGesture.startY;g_touchGesture.maxMove2=std::max(g_touchGesture.maxMove2,dx*dx+dy*dy);
    if(action==1){
        const float age=std::chrono::duration<float>(Clock::now()-g_touchGesture.started).count();
        if(age<=.32f&&g_touchGesture.maxMove2<=12.f*12.f){const int hit=onScreenButton?0:cachedPanelHit();if(hit)g_panelAction.store(hit,std::memory_order_release);else g_touchInput=true;}
        g_touchGesture={};
    }else if(action==3){g_touchGesture={};}
    return false;
}

void AnalyzeModule::handleTick(worldanalysis::sdk::Player* player){
    if(!enabled||!player||!g_getHitResult||!g_getBlock)return;std::lock_guard lock(g_mutex);g_player=player;g_dimension=player->dimension();g_region=g_dimension?g_dimension->blockSource():nullptr;if(!g_region)return;
    const auto now=Clock::now();const Vec3 pp=player->position();if(!finite(pp))return;
    g_panelRotation=player->rotation();cacheViewRay(player);

    // Keep one tick of aim history. Input can arrive between player ticks; the
    // previous verified hit gives the interaction capture a reliable fallback
    // instead of requiring the user to press attack/use repeatedly.
    const BlockTarget previousAim=g_cachedAim;const Vec3 previousHit=g_cachedAimHit;BlockPosRaw aimPos{};Vec3 aimHit{};
    if(currentHitBlock(aimPos,aimHit)){g_cachedAim={true,aimPos,g_dimension};g_cachedAimHit=aimHit;}else{g_cachedAim={};g_cachedAimHit={};}

    const Vec3 target=orbitTarget(player);if(finite(target)){if(!g_panelValid){g_panelCenter=target;g_panelValid=true;}else g_panelCenter=add(g_panelCenter,mul(sub(target,g_panelCenter),.10f));}else g_panelValid=false;if(!g_viewRayValid)g_panelEye=add(pp,{0,1.62f,0});if(now>=g_hiddenUntil&&g_panelValid)g_panelAppear=std::min(1.f,g_panelAppear+.075f);
    g_shards.erase(std::remove_if(g_shards.begin(),g_shards.end(),[&](const Shard&s){return std::chrono::duration<float>(now-s.created).count()>.50f;}),g_shards.end());
    const int panelAction=g_panelAction.exchange(0,std::memory_order_acq_rel);if(panelAction)handlePanelAction(panelAction);

    const bool interacted=g_primaryInput.exchange(false)||g_secondaryInput.exchange(false)||g_touchInput.exchange(false)||g_containerInteraction.exchange(false);
    if(interacted){
        if(g_cachedAim.valid)rememberTarget(g_cachedAim.pos,g_cachedAimHit);
        else if(previousAim.valid&&previousAim.dimension==g_dimension)rememberTarget(previousAim.pos,previousHit);
    }
    if(g_pendingTicks>0&&g_pendingPlaced.valid){--g_pendingTicks;if(blockExists(g_pendingPlaced.pos)){g_last=g_pendingPlaced;g_selectedName=displayName(blockIdentifier(blockAt(g_last.pos)));g_pendingPlaced={};g_pendingTicks=0;g_status="PLACED TARGET";g_statusUntil=now+std::chrono::milliseconds(850);}else if(g_pendingTicks==0)g_pendingPlaced={};}

    if(g_last.valid&&g_last.dimension!=g_dimension){g_last={};g_selectedName.clear();g_phase=Phase::Idle;g_result={};}
    else if(g_last.valid&&!blockExists(g_last.pos)&&g_phase!=Phase::Analyzing){g_last={};g_selectedName.clear();g_result={};g_status="TARGET REMOVED";g_statusUntil=now+std::chrono::milliseconds(1200);}
    if(g_phase==Phase::Analyzing&&std::chrono::duration<float>(now-g_analysisStarted).count()>=kAnalyzeSeconds){
        if(!g_last.valid||!blockExists(g_last.pos)){g_phase=Phase::Idle;g_status="TARGET CHANGED";g_statusUntil=now+std::chrono::milliseconds(1600);}
        else{auto cards=buildCards(g_last,player);if(cards.empty()){g_phase=Phase::Idle;g_status="NO DATA";g_statusUntil=now+std::chrono::milliseconds(1600);}else{void*b=blockAt(g_last.pos);const auto id=blockIdentifier(b);g_result.valid=true;g_result.pos=g_last.pos;g_result.center={g_last.pos.x+.5f,g_last.pos.y+.5f,g_last.pos.z+.5f};g_result.identifier=std::string(id);g_result.name=displayName(id);g_result.cards=std::move(cards);g_result.shownAt=now;g_result.expires=now+std::chrono::milliseconds(static_cast<int>(std::clamp(resultDuration,3.f,60.f)*1000.f));g_phase=Phase::Results;}}
    }
    if(g_phase==Phase::Results&&g_result.expires<=now){g_phase=Phase::Idle;g_result={};}
}

void AnalyzeModule::registerLauncherButton(){
    constexpr std::string_view buttonId="worldanalysis.Analyze.button";
    pl::modmenu::unregisterButton(buttonId);
    mButtonRegistered=false;
    if(!onScreenButton)return;
    const std::uint32_t fg=worldanalysis::ui::parseColorOr(color,0xFFFFFFFFu);
    const std::uint32_t bg=worldanalysis::ui::parseColorOr(backgroundColor,0xFF000000u);
    const float opacity=std::clamp(backgroundOpacity,0.08f,1.0f);
    const std::uint32_t normal=worldanalysis::ui::withAlpha(bg,opacity*.84f);
    const std::uint32_t active=(static_cast<std::uint32_t>(std::clamp(opacity*.94f,0.f,1.f)*255.f+.5f)<<24)|(fg&0x00FFFFFFu);
    mButtonRegistered=pl::modmenu::ButtonBuilder(std::string(buttonId),"Analyze")
        .moduleId(moduleId).label("ANALYZE").behavior(pl::modmenu::ButtonBehavior::Click).defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent).styleColors(normal,active,worldanalysis::ui::withAlpha(fg,.92f))
        .textColor(worldanalysis::ui::withAlpha(fg,1.0f)).activeTextColor(0xFF000000u|(bg&0x00FFFFFFu))
        .onEvent([](std::string_view,pl::modmenu::ButtonEvent event,float){
            if(event==pl::modmenu::ButtonEvent::Click&&g_mod&&g_mod->enabled)g_panelAction.store(1,std::memory_order_release);
        }).registerButton();
}

void AnalyzeModule::loadConfig(const nlohmann::json&j){Module::loadConfig(j);hiddenDuration=std::clamp(j.value("hiddenDuration",hiddenDuration),2.f,20.f);resultDuration=std::clamp(j.value("resultDuration",resultDuration),3.f,60.f);onScreenButton=j.value("onScreenButton",onScreenButton);}
void AnalyzeModule::saveConfig(nlohmann::json&j){Module::saveConfig(j);j["hiddenDuration"]=hiddenDuration;j["resultDuration"]=resultDuration;j["onScreenButton"]=onScreenButton;}
