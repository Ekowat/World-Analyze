#include "techvfx.hpp"

#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Vec3 = worldanalysis::sdk::Vec3;
using Segment = worldanalysis::worldoverlay::Segment;
using Quad = worldanalysis::worldoverlay::Quad;
using Clock = std::chrono::steady_clock;

struct BlockPosRaw { int x = 0, y = 0, z = 0; };
using GetBlockFn = void* (*)(void*, const BlockPosRaw&);

constexpr float kPi = 3.14159265358979323846f;
constexpr int kEffectCount = 96;
constexpr float kOrdinarySkyCeiling = 118.0f;

struct EffectDescriptor {
    const char* key;
    bool TechVFXModule::* enabled;
};

#define E(name) {#name, &TechVFXModule::name}
constexpr std::array<EffectDescriptor, kEffectCount> kEffects{{
    E(surfaceTrace), E(cornerBrackets), E(scanGrid), E(nodeNetwork), E(microGraph), E(spectrumBars),
    E(hexPulse), E(crosshairLock), E(dataCascade), E(circuitBranches), E(rulerTicks), E(coordinateStack),
    E(binaryRain), E(radarSweep), E(concentricRings), E(orbitNodes), E(waveRibbon), E(pulseColumn),
    E(holoCube), E(holoPyramid), E(wireSphere), E(helix), E(dnaChain), E(arcGauge), E(loadBars),
    E(matrixPanel), E(triangleFan), E(reticleBurst), E(horizonTicks), E(floatingTerminal), E(skyGraph),
    E(skyLattice), E(skyArc), E(beaconSpiral), E(dataConstellation), E(packetStream), E(voxelBracket),
    E(sectorMap), E(diagnosticPanel), E(orbitalBands), E(giantWorldSphere), E(worldAxisBurst),
    E(squarePulse), E(squareTunnel), E(squareRadar), E(squareMatrix), E(squareStack), E(squareSweep),
    E(squareCorners), E(squareCrossGrid), E(squareOrbit), E(squareWave), E(squareCodePanel), E(squareHistogram),
    E(squareNodeFrame), E(squareCircuitMap), E(squareReticle), E(squareTicker), E(squareBarcode), E(squareDiagnostic),
    E(squareSignalMap), E(squareCompass), E(squareFlowMap), E(squareTimeline), E(squareScope), E(squareProfiler),
    E(squarePacketGrid), E(squareMemoryMap), E(squareClock), E(squareVectorField), E(squareHeatmap), E(squareTargetArray),
    E(squareDataWindow), E(squareFragmentField), E(squarePortal), E(squareRain), E(squareScanline), E(squareEqualizer),
    E(squareWaveform), E(squareTree), E(squareMesh), E(squareLockGrid), E(skyBillboardGrid), E(skySquareArray),
    E(skyDataWall), E(skyScanPlane), E(skyTimeline), E(skyPulseGate), E(skyFloatingFrames), E(skyMegaGraph),
    E(movingPacketRibbon), E(movingSquareTrain), E(risingDataColumn), E(lateralScanPanel), E(driftingGridCloud), E(roamingReticle)
}};
#undef E

enum class SpawnBand : std::uint8_t { Near = 0, Mid = 1, Far = 2, Horizon = 3, Sky = 4 };

struct VfxInstance {
    int type = 0;
    Vec3 position{};
    Vec3 normal{0,1,0};
    Vec3 velocity{};
    float size = 1.0f;
    float spin = 0.0f;
    SpawnBand band = SpawnBand::Near;
    std::uint32_t seed = 1;
    Clock::time_point created{};
    Clock::time_point expires{};
};

TechVFXModule* g_mod = nullptr;
GetBlockFn g_getBlock = nullptr;
void* g_region = nullptr;
void* g_dimension = nullptr;
std::vector<VfxInstance> g_active;
std::uint32_t g_rng = 0xA1B2C3D4u;
std::array<Clock::time_point, 5> g_nextBandSpawn{};
Clock::time_point g_lastTick{};
Clock::time_point g_lastGiant{};
std::mutex g_mutex;

Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(const Vec3&a,const Vec3&b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float dot(const Vec3&a,const Vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float len(const Vec3&v){return std::sqrt(dot(v,v));}
Vec3 normalize(const Vec3&v,const Vec3&fallback={1,0,0}){const float l=len(v);return l>.0001f?mul(v,1.f/l):fallback;}
Vec3 cross(const Vec3&a,const Vec3&b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float smoothStep(float x){x=std::clamp(x,0.f,1.f);return x*x*(3.f-2.f*x);}
std::uint32_t white(float alpha){return(static_cast<std::uint32_t>(std::clamp(alpha,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu;}
std::uint32_t faintWhite(float alpha){return white(alpha*.12f);}
std::uint32_t nextRandom(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
float rand01(){return static_cast<float>(nextRandom()&0x00FFFFFFu)/16777215.0f;}
float randRange(float a,float b){return a+(b-a)*rand01();}

bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024.f&&p.y<4096.f;}
std::string_view blockIdentifier(const void* block) {
    if (!block) return {};
    const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!worldanalysis::worldoverlay::plausible(type))return{};
    const auto address=type+worldanalysis::sdk::offsets::BlockType::mNameInfo+worldanalysis::sdk::offsets::NameInfo::mFullName+worldanalysis::sdk::offsets::HashedString::mString;
    const auto*value=reinterpret_cast<const std::string*>(address);
    if(value->size()>128||(!value->empty()&&!value->data()))return{};
    return{value->data(),value->size()};
}
bool isAir(std::string_view id){return id.empty()||id=="minecraft:air"||id=="minecraft:cave_air"||id=="minecraft:void_air";}

void basisFromNormal(const Vec3&normal,Vec3&u,Vec3&v){
    const Vec3 n=normalize(normal,{0,1,0});
    const Vec3 ref=std::abs(n.y)>.8f?Vec3{1,0,0}:Vec3{0,1,0};
    u=normalize(cross(ref,n),{1,0,0});v=normalize(cross(n,u),{0,1,0});
}
Vec3 point(const Vec3&c,const Vec3&u,const Vec3&v,float x,float y){return add(c,add(mul(u,x),mul(v,y)));}
void line(std::vector<Segment>&out,const Vec3&a,const Vec3&b){out.push_back({a,b});}
void ring(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float r,int sides,float start=0,float span=2*kPi){
    sides=std::clamp(sides,6,96);for(int i=0;i<sides;++i){const float a=start+span*i/sides,b=start+span*(i+1)/sides;line(out,point(c,u,v,std::cos(a)*r,std::sin(a)*r),point(c,u,v,std::cos(b)*r,std::sin(b)*r));}
}
void box(std::vector<Segment>&out,const Vec3&c,float s){const float h=s*.5f;Vec3 p[8]{{c.x-h,c.y-h,c.z-h},{c.x+h,c.y-h,c.z-h},{c.x+h,c.y+h,c.z-h},{c.x-h,c.y+h,c.z-h},{c.x-h,c.y-h,c.z+h},{c.x+h,c.y-h,c.z+h},{c.x+h,c.y+h,c.z+h},{c.x-h,c.y+h,c.z+h}};for(auto [a,b]:std::array<std::array<int,2>,12>{{{{0,1}},{{1,2}},{{2,3}},{{3,0}},{{4,5}},{{5,6}},{{6,7}},{{7,4}},{{0,4}},{{1,5}},{{2,6}},{{3,7}}}})line(out,p[a],p[b]);}
void pyramid(std::vector<Segment>&out,const Vec3&c,float s){const float h=s*.5f;Vec3 p[5]{{c.x-h,c.y-h,c.z-h},{c.x+h,c.y-h,c.z-h},{c.x+h,c.y-h,c.z+h},{c.x-h,c.y-h,c.z+h},{c.x,c.y+h,c.z}};for(auto [a,b]:std::array<std::array<int,2>,8>{{{{0,1}},{{1,2}},{{2,3}},{{3,0}},{{0,4}},{{1,4}},{{2,4}},{{3,4}}}})line(out,p[a],p[b]);}
void graph(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float w,float h,float phase,int points=8){Vec3 prev{};for(int i=0;i<points;++i){float x=-w*.5f+w*i/(points-1.f);float y=std::sin(phase+i*1.37f)*h*.28f+std::cos(phase*.6f+i*.71f)*h*.12f;Vec3 q=point(c,u,v,x,y);if(i)line(out,prev,q);prev=q;ring(out,q,u,v,h*.035f,8);}}
void grid(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float w,float h,int cols,int rows){for(int i=0;i<=cols;++i){float x=-w*.5f+w*i/cols;line(out,point(c,u,v,x,-h*.5f),point(c,u,v,x,h*.5f));}for(int i=0;i<=rows;++i){float y=-h*.5f+h*i/rows;line(out,point(c,u,v,-w*.5f,y),point(c,u,v,w*.5f,y));}}
void rect(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float w,float h){
    const Vec3 bl=point(c,u,v,-w*.5f,-h*.5f),br=point(c,u,v,w*.5f,-h*.5f),tr=point(c,u,v,w*.5f,h*.5f),tl=point(c,u,v,-w*.5f,h*.5f);
    line(out,bl,br);line(out,br,tr);line(out,tr,tl);line(out,tl,bl);
}
void squareCorners(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float w,float h,float arm){
    for(int sx:{-1,1})for(int sy:{-1,1}){const float x=sx*w*.5f,y=sy*h*.5f;line(out,point(c,u,v,x,y),point(c,u,v,x-sx*arm,y));line(out,point(c,u,v,x,y),point(c,u,v,x,y-sy*arm));}
}
void movingDots(std::vector<Segment>&out,const Vec3&c,const Vec3&u,const Vec3&v,float w,float h,float phase,int count){
    for(int i=0;i<count;++i){float p=std::fmod(phase*.075f+i/(float)count,1.f);float x=-w*.5f+w*p;float y=(-.5f+((i*37)%100)/100.f)*h;ring(out,point(c,u,v,x,y),u,v,std::max(.008f,h*.018f),7);}
}

bool pickExposedBlock(const Vec3&local,float minDist,float maxDist,Vec3&position,Vec3&normal){
    if(!g_region||!g_getBlock)return false;static constexpr int dirs[6][3]{{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    for(int attempt=0;attempt<28;++attempt){const float a=randRange(0,2*kPi),r=randRange(minDist,maxDist);BlockPosRaw p{static_cast<int>(std::floor(local.x+std::cos(a)*r)),static_cast<int>(std::floor(local.y+randRange(-7,8))),static_cast<int>(std::floor(local.z+std::sin(a)*r))};void*b=g_getBlock(g_region,p);if(isAir(blockIdentifier(b)))continue;float best=-1e9f;bool found=false;Vec3 bestN{};const Vec3 center{p.x+.5f,p.y+.5f,p.z+.5f},toward=sub(local,center);for(const auto&d:dirs){BlockPosRaw np{p.x+d[0],p.y+d[1],p.z+d[2]};if(!isAir(blockIdentifier(g_getBlock(g_region,np))))continue;Vec3 n{static_cast<float>(d[0]),static_cast<float>(d[1]),static_cast<float>(d[2])};float score=dot(toward,n)+randRange(-.25f,.25f);if(score>best){best=score;bestN=n;found=true;}}if(found){normal=bestN;position=add(center,mul(normal,.535f));return true;}}
    return false;
}

bool findOpenSurface(const Vec3&local,float minDist,float maxDist,Vec3&position){
    if(!g_region||!g_getBlock)return false;for(int attempt=0;attempt<12;++attempt){float a=randRange(0,2*kPi),r=randRange(minDist,maxDist);int x=static_cast<int>(std::floor(local.x+std::cos(a)*r)),z=static_cast<int>(std::floor(local.z+std::sin(a)*r));int top=static_cast<int>(std::floor(std::min(kOrdinarySkyCeiling,local.y+28.f)));int bottom=static_cast<int>(std::floor(local.y-22.f));for(int y=top;y>=bottom;--y){BlockPosRaw p{x,y,z},above{x,y+1,z};if(!isAir(blockIdentifier(g_getBlock(g_region,p)))&&isAir(blockIdentifier(g_getBlock(g_region,above)))){position={x+.5f,y+1.15f,z+.5f};return true;}}}return false;
}

bool findOpenAir(const Vec3&local,float minDist,float maxDist,SpawnBand band,float extent,Vec3&position){
    if(!g_region||!g_getBlock)return false;
    for(int attempt=0;attempt<18;++attempt){
        const float a=randRange(0.f,2.f*kPi),r=randRange(minDist,maxDist);
        float y=local.y;
        switch(band){
            case SpawnBand::Near:y+=randRange(-1.f,6.f);break;
            case SpawnBand::Mid:y+=randRange(1.f,15.f);break;
            case SpawnBand::Far:y+=randRange(3.f,28.f);break;
            case SpawnBand::Horizon:y+=randRange(6.f,38.f);break;
            case SpawnBand::Sky:{
                const float skyHigh=kOrdinarySkyCeiling-extent-1.5f;
                const float skyLow=std::min(skyHigh-1.f,std::max(local.y+18.f,70.f));
                y=randRange(skyLow,skyHigh);
            }break;
        }
        y=std::clamp(y,-56.f+extent+2.f,kOrdinarySkyCeiling-extent-1.5f);
        Vec3 candidate{local.x+std::cos(a)*r,y,local.z+std::sin(a)*r};
        BlockPosRaw c{static_cast<int>(std::floor(candidate.x)),static_cast<int>(std::floor(candidate.y)),static_cast<int>(std::floor(candidate.z))};
        if(!isAir(blockIdentifier(g_getBlock(g_region,c))))continue;
        // Give larger panels a little breathing room so their anchor does not
        // begin inside a ceiling/wall even though the center block is air.
        BlockPosRaw up{c.x,c.y+std::max(1,static_cast<int>(std::ceil(std::min(4.f,extent)))),c.z};
        if(!isAir(blockIdentifier(g_getBlock(g_region,up))))continue;
        position=candidate;return true;
    }
    return false;
}

bool tooClose(const Vec3&p,float size){std::lock_guard lock(g_mutex);for(const auto&e:g_active){const float minSep=std::max(2.0f,(size+e.size)*.8f);if(len(sub(p,e.position))<minSep)return true;}return false;}

bool isSurfaceFamily(int type){ return type >= 0 && type <= 11; }
bool isSkyFamily(int type){ return (type >= 30 && type <= 39) || (type >= 82 && type <= 89); }
bool isMovingFamily(int type){ return type >= 90 && type <= 95; }
bool isSquareFamily(int type){ return type >= 42 && type <= 81; }

bool allowedInBand(int type, SpawnBand band){
    if(type==40) return band==SpawnBand::Sky;
    if(type==41) return band==SpawnBand::Far || band==SpawnBand::Horizon || band==SpawnBand::Sky;
    if(isSkyFamily(type)) return band!=SpawnBand::Near;
    if(isMovingFamily(type)) return band!=SpawnBand::Near;
    // 2D/surface families can occur across the whole loaded-looking field.
    // Sky placement is reserved mostly for the square/panel families.
    return band!=SpawnBand::Sky || type>=42;
}

float effectWeight(int type) {
    if (type == 40) return 0.06f;
    if (isSquareFamily(type)) return 1.35f;
    if (isMovingFamily(type)) return 1.15f;
    if (isSkyFamily(type)) return 1.00f;
    if (type >= 18 && type <= 23) return 0.62f;
    if (type == 39 || type == 41) return 0.48f;
    return 1.0f;
}

int chooseEffect(SpawnBand band){
    if(!g_mod)return-1;
    float total=0.f;
    for(int i=0;i<kEffectCount;++i){const auto&d=kEffects[i];if((g_mod->*(d.enabled))&&allowedInBand(i,band))total+=effectWeight(i);}
    if(total<=0.f)return-1;
    float pick=randRange(0,total);
    for(int i=0;i<kEffectCount;++i){const auto&d=kEffects[i];if(!(g_mod->*(d.enabled))||!allowedInBand(i,band))continue;pick-=effectWeight(i);if(pick<=0.f)return i;}
    return-1;
}

void bandRange(SpawnBand band,float&minD,float&maxD){
    const float range=std::clamp(g_mod?g_mod->effectRange:288.f,96.f,384.f);
    switch(band){
        case SpawnBand::Near:minD=5.0f;maxD=std::min(range,std::max(18.f,range*.14f));break;
        case SpawnBand::Mid:minD=std::max(14.f,range*.065f);maxD=std::min(range,std::max(48.f,range*.40f));break;
        case SpawnBand::Far:minD=std::max(36.f,range*.20f);maxD=std::min(range,std::max(78.f,range*.74f));break;
        case SpawnBand::Horizon:minD=std::min(range-4.f,std::max(64.f,range*.56f));maxD=range;break;
        case SpawnBand::Sky:minD=std::max(32.f,range*.14f);maxD=range;break;
    }
}

Vec3 velocityFor(int type,SpawnBand band,float size){
    if(!isMovingFamily(type))return{};
    const float m=std::clamp(g_mod?g_mod->animationSpeed:1.f,.5f,2.5f);
    const float speed=randRange(.35f,1.25f)*m*((band==SpawnBand::Far||band==SpawnBand::Horizon)?1.25f:1.f);
    switch(type){
        case 90:{float a=randRange(0,2*kPi);return{std::cos(a)*speed,randRange(.03f,.18f)*m,std::sin(a)*speed};}
        case 91:{float a=randRange(0,2*kPi);return{std::cos(a)*speed*.75f,0,std::sin(a)*speed*.75f};}
        case 92:return{randRange(-.08f,.08f)*m,speed*.62f,randRange(-.08f,.08f)*m};
        case 93:{float a=randRange(0,2*kPi);return{std::cos(a)*speed,randRange(-.08f,.08f)*m,std::sin(a)*speed};}
        case 94:{float a=randRange(0,2*kPi);return{std::cos(a)*speed*.34f,randRange(.05f,.22f)*m,std::sin(a)*speed*.34f};}
        case 95:{float a=randRange(0,2*kPi);return{std::cos(a)*speed*.52f,randRange(-.12f,.12f)*m,std::sin(a)*speed*.52f};}
        default:return{};
    }
}

float verticalExtentFor(int type,float size){
    if(type==40)return size;
    if(isSkyFamily(type)||type==41)return size*.68f;
    if(type>=42&&type<=81)return size*.48f;
    if(isMovingFamily(type))return size*.55f;
    return size*.52f;
}

bool spawnEffect(const Vec3&local,SpawnBand band){
    if(!g_mod)return false;
    const int type=chooseEffect(band);if(type<0)return false;
    const auto now=Clock::now();
    if(type==40&&g_lastGiant.time_since_epoch().count()!=0&&std::chrono::duration<float>(now-g_lastGiant).count()<std::clamp(g_mod->giantCooldown,20.f,180.f))return false;

    float minD=0,maxD=0;bandRange(band,minD,maxD);
    if((isSkyFamily(type)||type==41)&&type!=40){const float largeMin=std::clamp((g_mod?g_mod->effectRange:288.f)*.12f,24.f,72.f);minD=std::min(maxD-2.f,std::max(minD,largeMin));}
    Vec3 pos{},normal{0,1,0};float size=1.f;
    bool found=false;

    if(type==40){
        pos=local;pos.y=local.y+18.f;
        const float cageBase=std::clamp(g_mod->effectRange*.55f,108.f,190.f);
        size=cageBase*randRange(.92f,1.08f);found=true;g_lastGiant=now;
    }else if(isSurfaceFamily(type)){
        found=pickExposedBlock(local,minD,maxD,pos,normal);
        if(!found){found=findOpenSurface(local,minD,maxD,pos);normal={0,1,0};}
    }else{
        // Floating/panel effects do not need to wait on a valid ground-column
        // search. An open-air anchor keeps the horizon populated continuously.
        const float estimated=verticalExtentFor(type,std::max(1.f,g_mod->sizeScale));
        found=findOpenAir(local,minD,maxD,band,estimated,pos);
        if(!found)found=findOpenSurface(local,minD,maxD,pos);
    }
    if(!found)return false;

    switch(band){
        case SpawnBand::Near:size=randRange(.65f,2.15f);break;
        case SpawnBand::Mid:size=randRange(.85f,3.8f);break;
        case SpawnBand::Far:size=randRange(1.15f,6.8f);break;
        case SpawnBand::Horizon:size=randRange(1.6f,8.8f);break;
        case SpawnBand::Sky:size=randRange(2.8f,10.5f);break;
    }
    if(type==40){const float cageBase=std::clamp(g_mod->effectRange*.55f,108.f,190.f);size=cageBase*randRange(.92f,1.08f);}
    else if(type==41)size*=1.8f;
    else if(isSkyFamily(type))size*=randRange(1.15f,1.75f);
    else if(isSquareFamily(type)&&rand01()<.16f)size*=randRange(1.6f,2.8f); // occasional huge 2D panels
    size*=std::clamp(g_mod->sizeScale,.5f,2.f);

    if(type!=40 && !isSurfaceFamily(type)){
        float lift=0.f;
        if(band==SpawnBand::Near)lift=randRange(.55f,4.5f);
        else if(band==SpawnBand::Mid)lift=randRange(1.5f,10.f);
        else if(band==SpawnBand::Far)lift=randRange(3.f,18.f);
        else if(band==SpawnBand::Horizon)lift=randRange(5.f,28.f);
        else lift=randRange(16.f,38.f);
        const float lowerClearance=verticalExtentFor(type,size)+.65f;
        pos.y+=std::max(lift,lowerClearance);
    }
    if(type!=40){
        const float extent=verticalExtentFor(type,size);
        pos.y=std::min(pos.y,kOrdinarySkyCeiling-extent-1.f);
        // Keep ordinary VFX out of the player's immediate body volume.
        if(len(sub(pos,local))<4.25f)return false;
        if(tooClose(pos,size*(band==SpawnBand::Near?.52f:.36f)))return false;
    }

    // Wide lifetime jitter plus per-instance phase offsets prevents the active
    // set from aging out in synchronized waves.
    float life=randRange(5.0f,18.5f);
    if(band==SpawnBand::Far||band==SpawnBand::Horizon||band==SpawnBand::Sky)life*=randRange(1.08f,1.38f);
    if(type==40)life=randRange(20.f,34.f);
    if(isMovingFamily(type))life*=randRange(1.12f,1.40f);

    VfxInstance instance{};
    instance.type=type;instance.position=pos;instance.normal=normal;instance.velocity=velocityFor(type,band,size);instance.size=size;
    instance.spin=randRange(-1.f,1.f);instance.band=band;instance.seed=nextRandom();instance.created=now;
    instance.expires=now+std::chrono::milliseconds(static_cast<int>(life*1000.f));

    std::lock_guard lock(g_mutex);
    const int cap=std::clamp(g_mod->maxActive,24,96);
    if(static_cast<int>(g_active.size())>=cap){
        // Prefer replacing the oldest effect in the same distance band so one
        // busy band cannot starve another.
        auto it=std::min_element(g_active.begin(),g_active.end(),[&](const VfxInstance&a,const VfxInstance&b){
            const bool am=a.band==band,bm=b.band==band;if(am!=bm)return am;return a.created<b.created;
        });
        if(it!=g_active.end())g_active.erase(it);
    }
    g_active.push_back(instance);
    return true;
}

constexpr std::array<float,5> kBandShares{{.18f,.24f,.24f,.20f,.14f}};

float nextBandDelay(SpawnBand band,bool failed){
    if(!g_mod)return 1.f;
    const int bi=static_cast<int>(band);
    const float rate=std::max(.10f,std::clamp(g_mod->spawnRate,1.f,18.f)*kBandShares[bi]);
    const float mean=1.f/rate;
    if(failed)return randRange(std::max(.08f,mean*.22f),std::max(.22f,mean*.58f));
    return std::clamp(mean*randRange(.42f,1.82f),.045f,1.45f);
}

void scheduleBand(SpawnBand band,Clock::time_point now,bool failed=false){
    g_nextBandSpawn[static_cast<int>(band)]=now+std::chrono::milliseconds(static_cast<int>(nextBandDelay(band,failed)*1000.f));
}

std::string labelFor(int type,std::uint32_t seed){
    static constexpr std::array<const char*,20> labels{{
        "TRACE ACTIVE","NODE SYNC","SECTOR LOCK","VECTOR OK","SIGNAL 100","GRID STABLE","PACKET FLOW","SCAN PASS","LINK READY","CLOCK SYNC",
        "FIELD MAP","CACHE OK","FRAME LOCK","BUS ACTIVE","ROUTE OK","INDEX LIVE","PHASE SYNC","ARRAY READY","STREAM OK","CORE TRACE"
    }};
    if(type==30)return"SKY TELEMETRY";if(type==31)return"LATTICE ARRAY";if(type==38)return"DIAGNOSTIC";if(type==40)return"GLOBAL FIELD";if(type==41)return"WORLD AXIS";
    if(type>=82&&type<=89)return std::array<const char*,8>{{"SKY GRID","SQUARE ARRAY","DATA WALL","SCAN PLANE","SKY TIMELINE","PULSE GATE","FRAME FIELD","MEGA GRAPH"}}[type-82];
    if(type>=90)return std::array<const char*,6>{{"PACKET VECTOR","FRAME TRAIN","DATA RISE","LATERAL SCAN","GRID DRIFT","ROAM LOCK"}}[type-90];
    if(type>=42&&type<=81&&((type-42)%5==1))return{}; // deliberately textless geometric families
    return labels[seed%labels.size()];
}

bool hasPanel(int type){
    if(type==0||type==3||type==4||type==5||type==8||type==11||type==24||type==25||type==29||type==30||type==38)return true;
    if(type>=42&&type<=81)return ((type-42)%4)!=1; // mix contained and container-free square effects
    if(type>=82&&type<=89)return type!=85&&type!=87;
    if(type>=90&&type<=95)return type==93;
    return false;
}

void buildEffect(const VfxInstance&e,const worldanalysis::worldoverlay::RenderContext&ctx,float t,std::vector<Segment>&lines,std::vector<Quad>&quads,Vec3&textPos,std::string&label){
    Vec3 u{},v{};const bool surface=e.type<=11;if(surface)basisFromNormal(e.normal,u,v);else worldanalysis::worldoverlay::billboardBasis(e.position,ctx.camera,u,v);const float s=e.size,phase=t*std::clamp(g_mod?g_mod->animationSpeed:1.f,.5f,2.5f)*2.4f+static_cast<float>(e.seed&0xFFFFu)*.00137f;const auto P=[&](float x,float y){return point(e.position,u,v,x*s,y*s);};textPos=P(0,-.62f);label=labelFor(e.type,e.seed);
    if(hasPanel(e.type)){const float w=.78f*s,h=.52f*s;Vec3 bl=P(-.45f,-.32f),br=P(.45f,-.32f),tr=P(.45f,.32f),tl=P(-.45f,.32f);quads.push_back({bl,br,tr,tl});line(lines,bl,br);line(lines,br,tr);line(lines,tr,tl);line(lines,tl,bl);}
    switch(e.type){
        case 0:{float x=-.34f+std::fmod(phase*.13f,.68f);line(lines,P(-.38f,-.22f),P(-.38f,.22f));line(lines,P(.38f,-.22f),P(.38f,.22f));line(lines,P(x,-.22f),P(x,.22f));for(int i=0;i<6;++i)ring(lines,P(-.28f+i*.11f,.12f*std::sin(phase+i)),u,v,.018f*s,7);}break;
        case 1:{float q=.34f;for(int sx:{-1,1})for(int sy:{-1,1}){Vec3 c=P(sx*q,sy*.22f);line(lines,c,P(sx*(q-.12f),sy*.22f));line(lines,c,P(sx*q,sy*(.22f-.10f)));}ring(lines,P(0,0),u,v,.12f*s,16,phase,.9f*kPi);}break;
        case 2:{grid(lines,e.position,u,v,.72f*s,.46f*s,6,4);float y=-.23f+std::fmod(phase*.06f,.46f);line(lines,P(-.36f,y),P(.36f,y));}break;
        case 3:{std::array<Vec3,7> n{};for(int i=0;i<7;++i){float a=i*2*kPi/7+phase*.08f;n[i]=P(std::cos(a)*.28f,std::sin(a)*.17f);ring(lines,n[i],u,v,.018f*s,7);}for(int i=0;i<7;++i){line(lines,n[i],n[(i+2)%7]);if(i%2==0)line(lines,n[i],P(0,0));}}break;
        case 4:{graph(lines,P(0,.02f),u,v,.70f*s,.34f*s,phase,9);line(lines,P(-.36f,-.20f),P(.36f,-.20f));}break;
        case 5:{for(int i=0;i<9;++i){float h=.05f+.23f*(.5f+.5f*std::sin(phase+i*.72f));float x=-.31f+i*.078f;line(lines,P(x,-.20f),P(x,-.20f+h));}}break;
        case 6:{for(int r=0;r<3;++r)ring(lines,e.position,u,v,(.12f+r*.09f)*s,6,phase*(r?-.12f:.16f));}break;
        case 7:{ring(lines,e.position,u,v,.24f*s,28,phase*.1f,1.45f*kPi);line(lines,P(-.34f,0),P(-.11f,0));line(lines,P(.11f,0),P(.34f,0));line(lines,P(0,-.28f),P(0,-.10f));line(lines,P(0,.10f),P(0,.28f));}break;
        case 8:{for(int i=0;i<6;++i){float y=.22f-i*.08f;float x=-.32f+std::fmod(phase*.04f+i*.11f,.62f);line(lines,P(-.34f,y),P(x,y));}}break;
        case 9:{std::array<Vec3,10> n{};for(int i=0;i<10;++i)n[i]=P(-.34f+(i%5)*.17f,-.14f+(i/5)*.28f);for(int i=0;i<10;++i){ring(lines,n[i],u,v,.012f*s,6);if(i%5<4)line(lines,n[i],n[i+1]);if(i<5&&((i+e.seed)&1))line(lines,n[i],n[i+5]);}}break;
        case 10:{line(lines,P(-.38f,0),P(.38f,0));for(int i=0;i<13;++i){float x=-.36f+i*.06f,h=(i%5==0?.10f:.055f);line(lines,P(x,-h),P(x,h));}}break;
        case 11:{line(lines,P(-.32f,.12f),P(.32f,.12f));line(lines,P(-.32f,-.03f),P(.18f,-.03f));ring(lines,P(.25f,-.03f),u,v,.028f*s,8);line(lines,P(-.32f,-.17f),P(.02f,-.17f));}break;
        case 12:{for(int col=0;col<7;++col)for(int row=0;row<5;++row)if(((col*7+row+static_cast<int>(phase*2)+e.seed)&3)==0)line(lines,P(-.30f+col*.10f,.24f-row*.11f),P(-.30f+col*.10f,.20f-row*.11f));}break;
        case 13:{ring(lines,e.position,u,v,.36f*s,32);ring(lines,e.position,u,v,.20f*s,24);float a=phase*.35f;line(lines,e.position,P(std::cos(a)*.34f,std::sin(a)*.34f));for(int i=0;i<8;++i)ring(lines,P(std::cos(i*kPi/4)*.27f,std::sin(i*kPi/4)*.27f),u,v,.014f*s,6);}break;
        case 14:{for(int r=0;r<4;++r)ring(lines,e.position,u,v,(.10f+r*.08f)*s,28,phase*.08f*(r%2?1:-1),1.65f*kPi);}break;
        case 15:{std::array<Vec3,8> n{};for(int i=0;i<8;++i){float a=phase*.18f+i*kPi/4;n[i]=P(std::cos(a)*(.22f+.04f*(i&1)),std::sin(a)*(.22f+.04f*(i&1)));ring(lines,n[i],u,v,.015f*s,6);if(i)line(lines,n[i-1],n[i]);}line(lines,n.back(),n.front());}break;
        case 16:{Vec3 prev=P(-.42f,0);for(int i=1;i<=24;++i){float x=-.42f+.84f*i/24.f,y=.15f*std::sin(i*.55f+phase);Vec3 q=P(x,y);line(lines,prev,q);prev=q;}line(lines,P(-.42f,-.22f),P(.42f,-.22f));}break;
        case 17:{for(int i=0;i<5;++i){float y=-.36f+i*.18f;float w=.16f+.18f*(.5f+.5f*std::sin(phase+i));line(lines,P(-w,y),P(w,y));ring(lines,P(w,y),u,v,.015f*s,6);}}break;
        case 18:{box(lines,e.position,.65f*s);float k=.5f+.5f*std::sin(phase);box(lines,e.position,.25f*s*(.7f+.3f*k));}break;
        case 19:{pyramid(lines,e.position,.72f*s);ring(lines,P(0,-.36f),u,v,.30f*s,24,phase*.1f);}break;
        case 20:{Vec3 x{1,0,0},y{0,1,0},z{0,0,1};for(int i=1;i<=3;++i){float r=.10f*i*s;ring(lines,e.position,x,y,r,24,phase*.07f*i);ring(lines,e.position,z,y,r,24,-phase*.05f*i);}}break;
        case 21:{Vec3 prev{};for(int i=0;i<40;++i){float a=i*.45f+phase*.25f;Vec3 q{e.position.x+std::cos(a)*.20f*s,e.position.y+(-.42f+.84f*i/39.f)*s,e.position.z+std::sin(a)*.20f*s};if(i)line(lines,prev,q);prev=q;}}break;
        case 22:{Vec3 aPrev{},bPrev{};for(int i=0;i<28;++i){float y=-.40f+.80f*i/27.f,a=i*.65f+phase*.22f;Vec3 A=P(std::cos(a)*.18f,y),B=P(-std::cos(a)*.18f,y);if(i){line(lines,aPrev,A);line(lines,bPrev,B);}if(i%3==0)line(lines,A,B);aPrev=A;bPrev=B;}}break;
        case 23:{ring(lines,e.position,u,v,.34f*s,36,-.75f*kPi,1.5f*kPi);float a=-.75f*kPi+std::fmod(phase*.18f,1.5f*kPi);line(lines,e.position,P(std::cos(a)*.32f,std::sin(a)*.32f));}break;
        case 24:{for(int i=0;i<5;++i){float y=.20f-i*.10f,w=.55f*(.25f+.75f*(.5f+.5f*std::sin(phase*.7f+i)));line(lines,P(-.30f,y),P(-.30f+w,y));}}break;
        case 25:{grid(lines,e.position,u,v,.70f*s,.40f*s,7,4);for(int i=0;i<12;++i){float x=-.30f+(i%6)*.12f,y=.15f-(i/6)*.20f;ring(lines,P(x,y),u,v,.012f*s,6);}}break;
        case 26:{for(int i=0;i<10;++i){float a=-.7f+1.4f*i/9.f;line(lines,e.position,P(std::cos(a)*.34f,std::sin(a)*.34f));}ring(lines,e.position,u,v,.34f*s,28,-.7f,1.4f);}break;
        case 27:{for(int i=0;i<16;++i){float a=i*kPi/8;float r1=.16f*s,r2=(.28f+.07f*std::sin(phase+i))*s;line(lines,point(e.position,u,v,std::cos(a)*r1,std::sin(a)*r1),point(e.position,u,v,std::cos(a)*r2,std::sin(a)*r2));}}break;
        case 28:{line(lines,P(-.45f,0),P(.45f,0));for(int i=0;i<19;++i){float x=-.45f+i*.05f,h=(i%3==0?.11f:.055f);line(lines,P(x,-h),P(x,h));}ring(lines,e.position,u,v,.09f*s,16,phase*.2f);}break;
        case 29:{for(int i=0;i<7;++i){float y=.23f-i*.075f;float x=-.34f+std::fmod((phase*.025f+i*.13f),.62f);line(lines,P(-.34f,y),P(x,y));}line(lines,P(-.34f,-.28f),P(.34f,-.28f));}break;
        case 30:{graph(lines,e.position,u,v,1.55f*s,.70f*s,phase,13);for(int i=0;i<7;++i)line(lines,P(-.78f+i*.26f,-.38f),P(-.78f+i*.26f,-.31f));}break;
        case 31:{grid(lines,e.position,u,v,1.5f*s,.85f*s,10,6);ring(lines,e.position,u,v,.18f*s,20,phase*.14f);}break;
        case 32:{ring(lines,e.position,u,v,.65f*s,44,phase*.08f,1.5f*kPi);ring(lines,e.position,u,v,.46f*s,36,-phase*.10f,1.25f*kPi);for(int i=0;i<8;++i){float a=i*kPi/4;line(lines,P(std::cos(a)*.48f,std::sin(a)*.48f),P(std::cos(a)*.62f,std::sin(a)*.62f));}}break;
        case 33:{Vec3 prev{};for(int i=0;i<52;++i){float a=i*.42f+phase*.18f,r=.16f+.010f*i,y=-.55f+.022f*i;Vec3 q{e.position.x+std::cos(a)*r*s,e.position.y+y*s,e.position.z+std::sin(a)*r*s};if(i)line(lines,prev,q);prev=q;}line(lines,{e.position.x,e.position.y-.62f*s,e.position.z},{e.position.x,e.position.y+.70f*s,e.position.z});}break;
        case 34:{std::array<Vec3,14> n{};for(int i=0;i<14;++i){float a=(e.seed%100)*.01f+i*2.399f+phase*.025f,r=.15f+.45f*((i*37+e.seed)%100)/100.f;n[i]=P(std::cos(a)*r,std::sin(a)*r*.55f);ring(lines,n[i],u,v,.018f*s,7);}for(int i=1;i<14;++i)if((i+e.seed)%3)line(lines,n[i-1],n[i]);}break;
        case 35:{for(int i=0;i<8;++i){float y=.42f-i*.12f;float p=std::fmod(phase*.06f+i*.19f,1.f);line(lines,P(-.58f,y),P(-.58f+1.16f*p,y));ring(lines,P(-.58f+1.16f*p,y),u,v,.015f*s,6);}}break;
        case 36:{box(lines,e.position,.90f*s);box(lines,e.position,.48f*s);for(int i=0;i<8;++i){float sx=(i&1)?.45f:-.45f,sy=(i&2)?.45f:-.45f;line(lines,P(sx,sy),P(sx*.62f,sy*.62f));}}break;
        case 37:{ring(lines,e.position,u,v,.62f*s,48);for(int i=0;i<12;++i){float a=i*kPi/6;float r1=.22f+.10f*(i%3),r2=.60f;line(lines,P(std::cos(a)*r1,std::sin(a)*r1),P(std::cos(a)*r2,std::sin(a)*r2));}}break;
        case 38:{grid(lines,e.position,u,v,1.25f*s,.65f*s,8,4);graph(lines,P(0,.05f),u,v,1.05f*s,.40f*s,phase,10);ring(lines,P(.48f,-.22f),u,v,.07f*s,16,phase*.2f);}break;
        case 39:{Vec3 x{1,0,0},y{0,1,0},z{0,0,1};for(int r=1;r<=3;++r){ring(lines,e.position,x,y,.20f*r*s,40,phase*.04f*r);ring(lines,e.position,z,y,.20f*r*s,40,-phase*.035f*r);}for(int i=0;i<6;++i){float a=phase*.12f+i*kPi/3;Vec3 q{e.position.x+std::cos(a)*.58f*s,e.position.y+std::sin(a*.7f)*.18f*s,e.position.z+std::sin(a)*.58f*s};ring(lines,q,u,v,.025f*s,8);}}break;
        case 40:{
            // The global cage reveals from the zenith downward instead of
            // popping into existence as a finished sphere. Its latitude /
            // longitude grid forms large square-ish cells; selected upper
            // cells carry their own giant animated diagnostic glyphs.
            constexpr int latSteps=14,lonSteps=28;
            const float r=s;
            const float reveal=smoothStep(std::clamp(t/5.8f,0.f,1.f));
            const float rowsVisible=reveal*latSteps;
            const float spin=phase*.0025f;
            auto spherePoint=[&](float row,float col){
                const float phi=kPi*.5f-kPi*(row/latSteps);
                const float theta=2.f*kPi*(col/lonSteps)+spin;
                const float cp=std::cos(phi);
                return Vec3{e.position.x+r*cp*std::cos(theta),e.position.y+r*std::sin(phi),e.position.z+r*cp*std::sin(theta)};
            };
            const int completeRows=std::clamp(static_cast<int>(std::floor(rowsVisible)),0,latSteps);
            const float partial=rowsVisible-completeRows;
            for(int row=0;row<=completeRows;++row){
                for(int col=0;col<lonSteps;++col){
                    line(lines,spherePoint(static_cast<float>(row),static_cast<float>(col)),spherePoint(static_cast<float>(row),static_cast<float>(col+1)));
                    if(row>0)line(lines,spherePoint(static_cast<float>(row-1),static_cast<float>(col)),spherePoint(static_cast<float>(row),static_cast<float>(col)));
                }
            }
            if(completeRows<latSteps&&partial>.01f&&completeRows>0){
                const float row=completeRows+partial;
                for(int col=0;col<lonSteps;++col){
                    line(lines,spherePoint(static_cast<float>(completeRows),static_cast<float>(col)),spherePoint(row,static_cast<float>(col)));
                }
            }

            // Randomized mega effects live only in the revealed upper cells.
            // Skipping the lower half avoids deliberately planting giant
            // panels below terrain while the depth buffer still occludes any
            // mountain that happens to cross an upper cell.
            const int decorRows=std::min(completeRows,latSteps/2+2);
            for(int row=1;row<decorRows;++row){
                for(int col=0;col<lonSteps;++col){
                    const std::uint32_t h=e.seed^static_cast<std::uint32_t>(row*0x45d9f3bu)^static_cast<std::uint32_t>(col*0x27d4eb2du);
                    if((h%7u)!=0u)continue;
                    const Vec3 c=spherePoint(row+.5f,col+.5f);
                    if(c.y<e.position.y+6.f)continue;
                    Vec3 n=normalize(sub(c,e.position),{0,1,0}),cu{},cv{};basisFromNormal(n,cu,cv);
                    const float cellScale=r*(.032f+.020f*static_cast<float>((h>>11u)&255u)/255.f)*(1.f+.12f*std::sin(phase*.18f+(h&31u)));
                    const int pattern=static_cast<int>((h>>5u)%6u);
                    switch(pattern){
                        case 0:rect(lines,c,cu,cv,cellScale*2.2f,cellScale*1.35f);grid(lines,c,cu,cv,cellScale*1.9f,cellScale*1.08f,5,3);break;
                        case 1:{for(int q=0;q<4;++q)rect(lines,c,cu,cv,cellScale*(.65f+q*.42f),cellScale*(.42f+q*.28f));}break;
                        case 2:graph(lines,c,cu,cv,cellScale*2.2f,cellScale*1.25f,phase+(h&255u)*.01f,9);break;
                        case 3:ring(lines,c,cu,cv,cellScale*.95f,4,phase*.025f);ring(lines,c,cu,cv,cellScale*.55f,4,-phase*.032f);break;
                        case 4:squareCorners(lines,c,cu,cv,cellScale*2.2f,cellScale*1.45f,cellScale*.38f);movingDots(lines,c,cu,cv,cellScale*1.65f,cellScale*.92f,phase+(h&63u),7);break;
                        default:rect(lines,c,cu,cv,cellScale*2.25f,cellScale*1.3f);for(int q=0;q<6;++q){const float yy=(-.45f+q*.18f)*cellScale;line(lines,point(c,cu,cv,-cellScale*.9f,yy),point(c,cu,cv,cellScale*(.25f+.12f*((q+h)%5u)),yy));}break;
                    }
                }
            }
            textPos={e.position.x,e.position.y+r*.86f,e.position.z};
        }break;
        case 41:{float r=s*.5f;line(lines,{e.position.x-r,e.position.y,e.position.z},{e.position.x+r,e.position.y,e.position.z});line(lines,{e.position.x,e.position.y-r,e.position.z},{e.position.x,e.position.y+r,e.position.z});line(lines,{e.position.x,e.position.y,e.position.z-r},{e.position.x,e.position.y,e.position.z+r});for(int axis=0;axis<3;++axis)for(int i=1;i<=8;++i){float d=r*i/8.f,h=.05f*s;if(axis==0){line(lines,{e.position.x+d,e.position.y-h,e.position.z},{e.position.x+d,e.position.y+h,e.position.z});line(lines,{e.position.x-d,e.position.y-h,e.position.z},{e.position.x-d,e.position.y+h,e.position.z});}else if(axis==1){line(lines,{e.position.x-h,e.position.y+d,e.position.z},{e.position.x+h,e.position.y+d,e.position.z});line(lines,{e.position.x-h,e.position.y-d,e.position.z},{e.position.x+h,e.position.y-d,e.position.z});}else{line(lines,{e.position.x,e.position.y-h,e.position.z+d},{e.position.x,e.position.y+h,e.position.z+d});line(lines,{e.position.x,e.position.y-h,e.position.z-d},{e.position.x,e.position.y+h,e.position.z-d});}}}break;
        case 42:{float pulse=.72f+.25f*(.5f+.5f*std::sin(phase));for(int i=0;i<4;++i)rect(lines,e.position,u,v,(.22f+i*.14f)*pulse*s,(.16f+i*.11f)*pulse*s);squareCorners(lines,e.position,u,v,.92f*s,.64f*s,.10f*s);}break;
        case 43:{for(int i=0;i<7;++i){float p=std::fmod(phase*.045f+i/7.f,1.f);float sc=.18f+.82f*p;Vec3 c=P((p-.5f)*.13f,(.5f-p)*.08f);rect(lines,c,u,v,.72f*sc*s,.50f*sc*s);}}break;
        case 44:{rect(lines,e.position,u,v,.86f*s,.58f*s);float x=-.39f+std::fmod(phase*.09f,.78f);line(lines,P(x,-.25f),P(x,.25f));float y=-.25f+std::fmod(phase*.057f,.50f);line(lines,P(-.39f,y),P(.39f,y));for(int i=0;i<6;++i){float a=(i*1.77f+e.seed*.01f);rect(lines,P(std::sin(a)*.30f,std::cos(a)*.18f),u,v,.035f*s,.035f*s);}}break;
        case 45:{grid(lines,e.position,u,v,.84f*s,.56f*s,7,5);for(int y=0;y<5;++y)for(int x=0;x<7;++x)if(((x*3+y*5+static_cast<int>(phase*2)+e.seed)&7)<2){Vec3 c=P(-.36f+x*.12f,.224f-y*.112f);rect(lines,c,u,v,.07f*s,.055f*s);}}break;
        case 46:{for(int i=0;i<6;++i){float shift=.12f*std::sin(phase*.45f+i*.8f);rect(lines,P(shift,-.22f+i*.09f),u,v,(.62f-i*.045f)*s,.065f*s);}}break;
        case 47:{rect(lines,e.position,u,v,.90f*s,.60f*s);float p=std::fmod(phase*.075f,1.f);float y=-.27f+.54f*p;line(lines,P(-.42f,y),P(.42f,y));for(int i=0;i<5;++i){float h=.07f+.17f*(.5f+.5f*std::sin(phase+i));rect(lines,P(-.34f+i*.17f,-.20f+h*.5f),u,v,.09f*s,h*s);}}break;
        case 48:{float q=.46f*(.82f+.18f*std::sin(phase*.7f));squareCorners(lines,e.position,u,v,q*2*s,q*1.35f*s,.18f*s);squareCorners(lines,e.position,u,v,q*1.35f*s,q*.90f*s,.10f*s);line(lines,P(-.16f,0),P(.16f,0));}break;
        case 49:{grid(lines,e.position,u,v,.86f*s,.58f*s,4,4);float p=std::fmod(phase*.08f,1.f);line(lines,P(-.43f+.86f*p,-.29f),P(-.43f+.86f*p,.29f));line(lines,P(-.43f,0),P(.43f,0));line(lines,P(0,-.29f),P(0,.29f));}break;
        case 50:{rect(lines,e.position,u,v,.78f*s,.52f*s);for(int i=0;i<8;++i){float a=phase*.22f+i*kPi/4;Vec3 c=P(std::cos(a)*.34f,std::sin(a)*.22f);rect(lines,c,u,v,.055f*s,.055f*s);if(i%2==0)line(lines,c,P(0,0));}}break;
        case 51:{rect(lines,e.position,u,v,.88f*s,.58f*s);Vec3 prev=P(-.40f,0);for(int i=1;i<=30;++i){float x=-.40f+.80f*i/30.f,y=.17f*std::sin(i*.55f+phase)+.055f*std::sin(i*1.7f-phase*.6f);Vec3 q=P(x,y);line(lines,prev,q);prev=q;}line(lines,P(-.40f,-.22f),P(.40f,-.22f));}break;
        case 52:{rect(lines,e.position,u,v,.90f*s,.62f*s);for(int row=0;row<7;++row){float y=.23f-row*.075f;float p=std::fmod(phase*.035f+row*.13f,1.f);line(lines,P(-.37f,y),P(-.37f+.18f+.45f*p,y));if(row%2==0)rect(lines,P(.31f,y),u,v,.08f*s,.035f*s);}}break;
        case 53:{rect(lines,e.position,u,v,.90f*s,.58f*s);for(int i=0;i<12;++i){float h=.05f+.40f*(.5f+.5f*std::sin(phase*.65f+i*.72f+e.seed*.01f));float x=-.38f+i*.069f;line(lines,P(x,-.23f),P(x,-.23f+h));}line(lines,P(-.40f,-.23f),P(.40f,-.23f));}break;
        case 54:{rect(lines,e.position,u,v,.88f*s,.60f*s);std::array<Vec3,10> n{};for(int i=0;i<10;++i){float x=-.34f+(i%5)*.17f,y=.17f-(i/5)*.34f;n[i]=P(x,y);rect(lines,n[i],u,v,.035f*s,.035f*s);}for(int i=0;i<9;++i)if(((i+e.seed)&2)==0)line(lines,n[i],n[(i+3)%10]);movingDots(lines,e.position,u,v,.70f*s,.36f*s,phase,5);}break;
        case 55:{rect(lines,e.position,u,v,.90f*s,.62f*s);for(int row=0;row<4;++row){float y=.21f-row*.14f;float x=-.37f;for(int seg=0;seg<4;++seg){float nx=x+.10f+.055f*((row+seg)%2);line(lines,P(x,y),P(nx,y));if(seg<3){float ny=y+(seg%2?.055f:-.055f);line(lines,P(nx,y),P(nx,ny));y=ny;}rect(lines,P(nx,y),u,v,.028f*s,.028f*s);x=nx;}}}break;
        case 56:{rect(lines,e.position,u,v,.82f*s,.58f*s);squareCorners(lines,e.position,u,v,.68f*s,.44f*s,.12f*s);float p=.12f+.08f*std::sin(phase*.9f);rect(lines,e.position,u,v,p*2*s,p*2*s);line(lines,P(-.34f,0),P(-.12f,0));line(lines,P(.12f,0),P(.34f,0));}break;
        case 57:{rect(lines,e.position,u,v,.92f*s,.54f*s);for(int row=0;row<4;++row){float p=std::fmod(phase*.055f+row*.21f,1.f);float x=-.40f+.80f*p;line(lines,P(-.40f,.18f-row*.12f),P(x,.18f-row*.12f));rect(lines,P(x,.18f-row*.12f),u,v,.03f*s,.03f*s);}}break;
        case 58:{rect(lines,e.position,u,v,.88f*s,.58f*s);for(int i=0;i<24;++i){float x=-.39f+i*.034f;float h=((e.seed>>(i%16))&1)?.43f:.25f;float wob=.025f*std::sin(phase+i);line(lines,P(x,-h*.5f+wob),P(x,h*.5f+wob));}}break;
        case 59:{rect(lines,e.position,u,v,.96f*s,.64f*s);grid(lines,P(-.15f,.02f),u,v,.54f*s,.38f*s,5,3);graph(lines,P(-.15f,.02f),u,v,.48f*s,.30f*s,phase,7);for(int i=0;i<4;++i){float y=.20f-i*.13f;rect(lines,P(.35f,y),u,v,.13f*s,.065f*s);}}break;
        case 60:{rect(lines,e.position,u,v,.90f*s,.60f*s);for(int row=0;row<5;++row){float y=.20f-row*.10f;float amp=.12f+.18f*(.5f+.5f*std::sin(phase*.5f+row));line(lines,P(-.36f,y),P(-.36f+amp*2.f,y));for(int j=0;j<3;++j)rect(lines,P(.12f+j*.11f,y),u,v,(.025f+.02f*((row+j+e.seed)&1))*s,.025f*s);}}break;
        case 61:{rect(lines,e.position,u,v,.78f*s,.78f*s);ring(lines,e.position,u,v,.28f*s,4,phase*.10f);for(int i=0;i<8;++i){float a=i*kPi/4;line(lines,P(std::cos(a)*.18f,std::sin(a)*.18f),P(std::cos(a)*.34f,std::sin(a)*.34f));}float a=phase*.20f;line(lines,e.position,P(std::cos(a)*.25f,std::sin(a)*.25f));}break;
        case 62:{rect(lines,e.position,u,v,.94f*s,.60f*s);for(int r=0;r<4;++r){float y=.19f-r*.13f;for(int c=0;c<6;++c){float p=std::fmod(phase*.045f+c*.13f+r*.08f,1.f);float x=-.36f+c*.12f;line(lines,P(x,y),P(x+.07f*p,y));line(lines,P(x+.07f*p,y),P(x+.045f*p,y+.025f));}}}break;
        case 63:{rect(lines,e.position,u,v,.96f*s,.56f*s);line(lines,P(-.42f,0),P(.42f,0));for(int i=0;i<9;++i){float x=-.40f+i*.10f;float h=(i%3==0?.16f:.08f);line(lines,P(x,-h),P(x,h));}float p=std::fmod(phase*.055f,1.f);rect(lines,P(-.40f+.80f*p,0),u,v,.045f*s,.20f*s);}break;
        case 64:{rect(lines,e.position,u,v,.92f*s,.62f*s);ring(lines,e.position,u,v,.22f*s,4,phase*.05f);float p=std::fmod(phase*.10f,1.f);line(lines,P(-.38f+.76f*p,-.24f),P(-.38f+.76f*p,.24f));movingDots(lines,e.position,u,v,.60f*s,.34f*s,phase,7);}break;
        case 65:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int i=0;i<7;++i){float y=.23f-i*.075f;float w=.18f+.50f*(.5f+.5f*std::sin(phase*.38f+i*.8f));rect(lines,P(-.37f+w*.25f,y),u,v,w*.5f*s,.04f*s);line(lines,P(.18f,y),P(.36f,y));}}break;
        case 66:{rect(lines,e.position,u,v,.92f*s,.62f*s);grid(lines,e.position,u,v,.78f*s,.48f*s,6,4);for(int i=0;i<6;++i){float p=std::fmod(phase*.07f+i*.17f,1.f);float x=-.32f+.64f*p,y=.18f-(i%4)*.12f;rect(lines,P(x,y),u,v,.05f*s,.035f*s);}}break;
        case 67:{rect(lines,e.position,u,v,.92f*s,.62f*s);for(int y=0;y<5;++y)for(int x=0;x<8;++x){float px=-.35f+x*.10f,py=.20f-y*.10f;if(((x+y*3+static_cast<int>(phase)+e.seed)&5)==0)rect(lines,P(px,py),u,v,.07f*s,.055f*s);else if((x+y)&1)line(lines,P(px-.025f,py),P(px+.025f,py));}}break;
        case 68:{rect(lines,e.position,u,v,.72f*s,.72f*s);for(int i=0;i<12;++i){float a=i*kPi/6;line(lines,P(std::cos(a)*.25f,std::sin(a)*.25f),P(std::cos(a)*.32f,std::sin(a)*.32f));}float a=phase*.12f,b=-phase*.04f;line(lines,e.position,P(std::cos(a)*.25f,std::sin(a)*.25f));line(lines,e.position,P(std::cos(b)*.18f,std::sin(b)*.18f));}break;
        case 69:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int y=0;y<4;++y)for(int x=0;x<7;++x){float px=-.34f+x*.11f,py=.19f-y*.13f;float a=phase*.12f+x*.7f-y*.4f;Vec3 a0=P(px,py),a1=P(px+std::cos(a)*.055f,py+std::sin(a)*.055f);line(lines,a0,a1);line(lines,a1,P(px+std::cos(a-.55f)*.035f,py+std::sin(a-.55f)*.035f));}}break;
        case 70:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int y=0;y<5;++y)for(int x=0;x<8;++x){float k=.55f+.45f*std::sin(phase*.45f+x*.8f+y*.55f);float sc=.028f+.035f*k;rect(lines,P(-.35f+x*.10f,.20f-y*.10f),u,v,sc*s,sc*s);}}break;
        case 71:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int y=0;y<3;++y)for(int x=0;x<5;++x){Vec3 c=P(-.30f+x*.15f,.16f-y*.16f);float sc=.045f+.025f*std::sin(phase+x+y);rect(lines,c,u,v,sc*s,sc*s);squareCorners(lines,c,u,v,.11f*s,.11f*s,.03f*s);}}break;
        case 72:{rect(lines,e.position,u,v,1.00f*s,.66f*s);rect(lines,P(-.18f,.08f),u,v,.52f*s,.34f*s);for(int i=0;i<5;++i)line(lines,P(.15f,.20f-i*.09f),P(.42f-.04f*(i%2),.20f-i*.09f));float p=std::fmod(phase*.065f,1.f);line(lines,P(-.40f,-.25f),P(-.40f+.80f*p,-.25f));}break;
        case 73:{for(int i=0;i<18;++i){float a=i*2.399f+phase*.055f,r=.08f+.36f*((i*37+e.seed)%100)/100.f;Vec3 c=P(std::cos(a)*r,std::sin(a)*r*.65f);float sc=.025f+.025f*((i+e.seed)%3);rect(lines,c,u,v,sc*s,sc*s);if(i%3==0)line(lines,c,P(0,0));}}break;
        case 74:{for(int i=0;i<6;++i){float a=phase*.045f+i*.25f;float sc=.20f+i*.11f;Vec3 c=P(std::sin(a)*.035f,std::cos(a)*.025f);rect(lines,c,u,v,sc*s,sc*.72f*s);}line(lines,P(-.42f,0),P(.42f,0));}break;
        case 75:{rect(lines,e.position,u,v,.90f*s,.62f*s);for(int i=0;i<12;++i){float p=std::fmod(phase*.055f+i*.083f,1.f);float x=-.37f+i*.067f,y=.27f-.54f*p;line(lines,P(x,y),P(x,y-.07f));}}break;
        case 76:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int i=0;i<10;++i)line(lines,P(-.38f,.24f-i*.053f),P(.38f,.24f-i*.053f));float p=std::fmod(phase*.085f,1.f);rect(lines,P(0,.25f-.50f*p),u,v,.80f*s,.035f*s);}break;
        case 77:{rect(lines,e.position,u,v,.92f*s,.62f*s);for(int i=0;i<16;++i){float h=.06f+.40f*(.5f+.5f*std::sin(phase*.75f+i*.52f));float x=-.38f+i*.051f;rect(lines,P(x,-.24f+h*.5f),u,v,.028f*s,h*s);}}break;
        case 78:{rect(lines,e.position,u,v,.96f*s,.62f*s);Vec3 p0=P(-.42f,0);for(int i=1;i<=42;++i){float x=-.42f+.84f*i/42.f,y=.14f*std::sin(i*.37f+phase)+.07f*std::sin(i*1.13f-phase*.33f);Vec3 q=P(x,y);line(lines,p0,q);p0=q;}for(int i=0;i<7;++i)line(lines,P(-.42f+i*.14f,-.24f),P(-.42f+i*.14f,.24f));}break;
        case 79:{rect(lines,e.position,u,v,.92f*s,.64f*s);Vec3 root=P(0,.23f);rect(lines,root,u,v,.07f*s,.04f*s);for(int i=0;i<3;++i){Vec3 a=P(-.28f+i*.28f,.05f);line(lines,root,a);rect(lines,a,u,v,.08f*s,.04f*s);for(int j=0;j<2;++j){Vec3 b=P(-.35f+i*.28f+j*.14f,-.20f);line(lines,a,b);rect(lines,b,u,v,.055f*s,.035f*s);}}}break;
        case 80:{rect(lines,e.position,u,v,.94f*s,.62f*s);for(int i=0;i<=8;++i){float x=-.40f+i*.10f;line(lines,P(x,-.25f),P(x+.18f,.25f));line(lines,P(x-.18f,-.25f),P(x,.25f));}float p=std::fmod(phase*.06f,1.f);rect(lines,P(-.36f+.72f*p,0),u,v,.06f*s,.48f*s);}break;
        case 81:{rect(lines,e.position,u,v,.96f*s,.64f*s);grid(lines,e.position,u,v,.80f*s,.50f*s,5,4);float px=-.32f+std::fmod(phase*.06f,.64f),py=.20f-.10f*((static_cast<int>(phase*.8f)+e.seed)%5);squareCorners(lines,P(px,py),u,v,.14f*s,.12f*s,.04f*s);}break;
        case 82:{grid(lines,e.position,u,v,1.55f*s,.88f*s,10,6);float p=std::fmod(phase*.045f,1.f);line(lines,P(-.72f+.1f,-.40f+.80f*p),P(.72f-.1f,-.40f+.80f*p));for(int i=0;i<8;++i)rect(lines,P(-.60f+i*.17f,.30f*std::sin(i+phase*.3f)),u,v,.07f*s,.05f*s);}break;
        case 83:{for(int row=0;row<3;++row)for(int col=0;col<5;++col){float pulse=.80f+.20f*std::sin(phase*.45f+row+col*.6f);rect(lines,P(-.60f+col*.30f,.28f-row*.28f),u,v,.22f*pulse*s,.16f*pulse*s);}movingDots(lines,e.position,u,v,1.35f*s,.65f*s,phase,9);}break;
        case 84:{rect(lines,e.position,u,v,1.65f*s,.90f*s);grid(lines,P(-.28f,.02f),u,v,.82f*s,.60f*s,8,5);graph(lines,P(-.28f,.03f),u,v,.75f*s,.48f*s,phase,11);for(int i=0;i<7;++i){float y=.31f-i*.10f;line(lines,P(.20f,y),P(.67f-.08f*((i+e.seed)%3),y));}}break;
        case 85:{for(int i=0;i<12;++i){float x=-.70f+i*.127f;line(lines,P(x,-.42f),P(x,.42f));}float p=std::fmod(phase*.055f,1.f);line(lines,P(-.75f,-.40f+.80f*p),P(.75f,-.40f+.80f*p));for(int i=0;i<6;++i)rect(lines,P(-.50f+i*.20f,.20f*std::sin(phase*.4f+i)),u,v,.06f*s,.06f*s);}break;
        case 86:{rect(lines,e.position,u,v,1.70f*s,.62f*s);line(lines,P(-.75f,0),P(.75f,0));for(int i=0;i<15;++i){float x=-.70f+i*.10f,h=i%5==0?.22f:.10f;line(lines,P(x,-h),P(x,h));}float p=std::fmod(phase*.04f,1.f);rect(lines,P(-.70f+1.40f*p,0),u,v,.07f*s,.32f*s);}break;
        case 87:{float pulse=.76f+.24f*(.5f+.5f*std::sin(phase*.55f));rect(lines,e.position,u,v,1.25f*pulse*s,.82f*pulse*s);rect(lines,e.position,u,v,.86f*pulse*s,.54f*pulse*s);squareCorners(lines,e.position,u,v,1.52f*s,.98f*s,.22f*s);}break;
        case 88:{for(int i=0;i<8;++i){float a=phase*.035f+i*.6f;Vec3 c=P(std::sin(a)*.22f,std::cos(a*.8f)*.14f);float sc=.28f+i*.12f;rect(lines,c,u,v,sc*s,sc*.62f*s);} }break;
        case 89:{rect(lines,e.position,u,v,1.85f*s,1.00f*s);grid(lines,e.position,u,v,1.62f*s,.78f*s,12,6);graph(lines,P(0,.05f),u,v,1.50f*s,.62f*s,phase,15);for(int i=0;i<5;++i)rect(lines,P(-.62f+i*.31f,-.39f),u,v,.20f*s,.06f*s);}break;
        case 90:{for(int i=0;i<10;++i){float p=std::fmod(phase*.045f+i*.10f,1.f);float x=-.65f+1.30f*p,y=.18f*std::sin(phase*.3f+i*.8f);rect(lines,P(x,y),u,v,.09f*s,.055f*s);if(i)line(lines,P(-.65f+1.30f*std::fmod(phase*.045f+(i-1)*.10f,1.f),.18f*std::sin(phase*.3f+(i-1)*.8f)),P(x,y));}}break;
        case 91:{for(int i=0;i<9;++i){float p=std::fmod(phase*.035f+i*.115f,1.f);Vec3 c=P(-.65f+1.30f*p,.08f*std::sin(i+phase*.2f));rect(lines,c,u,v,.13f*s,.13f*s);if(i%2==0)squareCorners(lines,c,u,v,.18f*s,.18f*s,.04f*s);}}break;
        case 92:{for(int i=0;i<8;++i){float p=std::fmod(phase*.04f+i*.12f,1.f);float y=-.60f+1.20f*p;rect(lines,P((i-3.5f)*.085f,y),u,v,.055f*s,.11f*s);line(lines,P((i-3.5f)*.085f,-.58f),P((i-3.5f)*.085f,.58f));}}break;
        case 93:{rect(lines,e.position,u,v,1.20f*s,.72f*s);float p=std::fmod(phase*.07f,1.f);float x=-.54f+1.08f*p;rect(lines,P(x,0),u,v,.045f*s,.60f*s);for(int i=0;i<7;++i){float y=.25f-i*.085f;line(lines,P(-.50f,y),P(.48f-.08f*((i+e.seed)%4),y));}}break;
        case 94:{grid(lines,e.position,u,v,1.35f*s,.80f*s,9,5);for(int i=0;i<12;++i){float a=i*.52f+phase*.08f;Vec3 c=P(std::cos(a)*.52f,std::sin(a*.7f)*.27f);rect(lines,c,u,v,.05f*s,.05f*s);} }break;
        case 95:{squareCorners(lines,e.position,u,v,.90f*s,.62f*s,.16f*s);float a=phase*.18f;rect(lines,P(std::cos(a)*.20f,std::sin(a)*.13f),u,v,.12f*s,.12f*s);line(lines,P(-.38f,0),P(-.16f,0));line(lines,P(.16f,0),P(.38f,0));line(lines,P(0,-.26f),P(0,-.12f));line(lines,P(0,.12f),P(0,.26f));}break;

    }
}

void renderVfx(void*self,void*screen,void*){
    if(!g_mod||!g_mod->enabled||!g_mod->masterEnabled||!g_mod->keybindActive)return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color);
    worldanalysis::worldoverlay::RenderContext ctx;if(!worldanalysis::worldoverlay::makeContext(self,screen,ctx))return;
    void*mat=worldanalysis::worldoverlay::depthMaterial();if(!mat)return;
    std::vector<VfxInstance> active;{std::lock_guard lock(g_mutex);active=g_active;}
    const auto now=Clock::now();const float baseOpacity=.92f;
    for(const auto&e:active){
        if(e.expires<=now)continue;
        const float age=std::chrono::duration<float>(now-e.created).count();
        const float remaining=std::chrono::duration<float>(e.expires-now).count();
        const float alpha=baseOpacity*smoothStep(age/.36f)*smoothStep(remaining/.52f);if(alpha<.003f)continue;
        VfxInstance moved=e;
        moved.position=add(e.position,mul(e.velocity,age));
        if(e.type!=40){const float extent=verticalExtentFor(e.type,e.size);moved.position.y=std::min(moved.position.y,kOrdinarySkyCeiling-extent-.75f);}
        std::vector<Segment> lines;std::vector<Quad> quads;lines.reserve(e.type==40?3600:(e.type>=82?420:240));quads.reserve(3);
        Vec3 textPos{};std::string label;buildEffect(moved,ctx,age,lines,quads,textPos,label);
        if(!quads.empty())worldanalysis::worldoverlay::drawQuads(ctx,mat,quads,faintWhite(alpha));
        const float width=(e.band==SpawnBand::Far||e.band==SpawnBand::Horizon||e.band==SpawnBand::Sky)?1.25f:1.55f;
        worldanalysis::worldoverlay::drawLines(ctx,mat,lines,white(alpha),width);
        if(!label.empty()&&e.type!=40){float px=std::clamp(.0105f*std::sqrt(std::max(.75f,e.size)),.008f,.030f);if(e.band==SpawnBand::Far||e.band==SpawnBand::Horizon||e.band==SpawnBand::Sky)px=std::max(px,.014f);worldanalysis::worldoverlay::drawBillboardText(ctx,mat,label,textPos,px,white(alpha*.92f),false);}
        if(e.type==40)worldanalysis::worldoverlay::drawBillboardText(ctx,mat,label,textPos,.028f,white(alpha*.75f),false);
    }
}

void clearState(){g_region=nullptr;g_dimension=nullptr;g_lastTick={};for(auto&when:g_nextBandSpawn)when={};std::lock_guard lock(g_mutex);g_active.clear();}
} // namespace

TechVFXModule::TechVFXModule():Module("Tech VFX","A toggleable, continuously evolving depth-tested field of 96 white technical VFX. Effects use independent randomized near, mid, far, horizon and under-cloud sky schedules; individual families are simple on/off toggles and only a small set of global density/range controls remain. The rare global cage descends from above and can decorate its upper wire cells with giant animated diagnostics."){g_mod=this;showInMenu=true;}
TechVFXModule::~TechVFXModule(){if(g_mod==this)g_mod=nullptr;}
void TechVFXModule::onInit(){if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);worldanalysis::worldoverlay::initialize();worldanalysis::worldoverlay::registerRenderCallback(renderVfx);worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});}
void TechVFXModule::onEnable(){clearState();g_lastGiant=Clock::now();}
void TechVFXModule::onDisable(){clearState();}
void TechVFXModule::handleTick(worldanalysis::sdk::Player*player){
    if(!enabled||!masterEnabled||!keybindActive||!player||!g_getBlock)return;
    const Vec3 local=player->position();if(!finite(local))return;
    auto*dim=player->dimension();void*region=dim?dim->blockSource():nullptr;if(!region)return;
    const auto now=Clock::now();
    if(g_dimension!=dim||g_region!=region){
        clearState();g_dimension=dim;g_region=region;g_lastTick=now;
        // Stagger all five populations from the first frame instead of creating
        // a synchronized startup burst.
        for(int i=0;i<5;++i)g_nextBandSpawn[i]=now+std::chrono::milliseconds(static_cast<int>(randRange(.04f,.58f)*1000.f));
        return;
    }
    if(g_lastTick.time_since_epoch().count()==0){g_lastTick=now;return;}
    g_lastTick=now;
    {
        std::lock_guard lock(g_mutex);
        g_active.erase(std::remove_if(g_active.begin(),g_active.end(),[&](const VfxInstance&e){return e.expires<=now;}),g_active.end());
        const int cap=std::clamp(maxActive,24,96);
        while(static_cast<int>(g_active.size())>cap){
            auto it=std::min_element(g_active.begin(),g_active.end(),[](const VfxInstance&a,const VfxInstance&b){return a.created<b.created;});
            if(it==g_active.end())break;g_active.erase(it);
        }
    }

    // Five independent randomized clocks prevent the old all-at-once spawn /
    // all-at-once expiry rhythm. A failed horizon placement only retries that
    // horizon band and never pauses the rest of the field.
    for(int i=0;i<5;++i){
        const SpawnBand band=static_cast<SpawnBand>(i);
        if(g_nextBandSpawn[i].time_since_epoch().count()==0)g_nextBandSpawn[i]=now;
        if(now<g_nextBandSpawn[i])continue;
        bool made=false;
        for(int tries=0;tries<3&&!made;++tries)made=spawnEffect(local,band);
        scheduleBand(band,now,!made);
    }
}

void TechVFXModule::loadConfig(const nlohmann::json&j){
    Module::loadConfig(j);
    spawnRate=std::clamp(j.value("spawnRate",spawnRate),1.f,18.f);
    maxActive=std::clamp(j.value("maxActive",maxActive),24,96);
    effectRange=std::clamp(j.value("effectRange",j.value("farRange",effectRange)),96.f,384.f);
    sizeScale=std::clamp(j.value("sizeScale",sizeScale),.5f,2.5f);
    animationSpeed=std::clamp(j.value("animationSpeed",animationSpeed),.5f,2.5f);
    giantCooldown=std::clamp(j.value("giantCooldown",giantCooldown),30.f,240.f);
#define LOAD_BOOL(field) field=j.value(#field,field);
    LOAD_BOOL(surfaceTrace) LOAD_BOOL(cornerBrackets) LOAD_BOOL(scanGrid) LOAD_BOOL(nodeNetwork) LOAD_BOOL(microGraph) LOAD_BOOL(spectrumBars) LOAD_BOOL(hexPulse) LOAD_BOOL(crosshairLock) LOAD_BOOL(dataCascade) LOAD_BOOL(circuitBranches) LOAD_BOOL(rulerTicks) LOAD_BOOL(coordinateStack) LOAD_BOOL(binaryRain) LOAD_BOOL(radarSweep) LOAD_BOOL(concentricRings) LOAD_BOOL(orbitNodes) LOAD_BOOL(waveRibbon) LOAD_BOOL(pulseColumn) LOAD_BOOL(holoCube) LOAD_BOOL(holoPyramid) LOAD_BOOL(wireSphere) LOAD_BOOL(helix) LOAD_BOOL(dnaChain) LOAD_BOOL(arcGauge) LOAD_BOOL(loadBars) LOAD_BOOL(matrixPanel) LOAD_BOOL(triangleFan) LOAD_BOOL(reticleBurst) LOAD_BOOL(horizonTicks) LOAD_BOOL(floatingTerminal) LOAD_BOOL(skyGraph) LOAD_BOOL(skyLattice) LOAD_BOOL(skyArc) LOAD_BOOL(beaconSpiral) LOAD_BOOL(dataConstellation) LOAD_BOOL(packetStream) LOAD_BOOL(voxelBracket) LOAD_BOOL(sectorMap) LOAD_BOOL(diagnosticPanel) LOAD_BOOL(orbitalBands) LOAD_BOOL(giantWorldSphere) LOAD_BOOL(worldAxisBurst) LOAD_BOOL(squarePulse) LOAD_BOOL(squareTunnel) LOAD_BOOL(squareRadar) LOAD_BOOL(squareMatrix) LOAD_BOOL(squareStack) LOAD_BOOL(squareSweep) LOAD_BOOL(squareCorners) LOAD_BOOL(squareCrossGrid) LOAD_BOOL(squareOrbit) LOAD_BOOL(squareWave) LOAD_BOOL(squareCodePanel) LOAD_BOOL(squareHistogram) LOAD_BOOL(squareNodeFrame) LOAD_BOOL(squareCircuitMap) LOAD_BOOL(squareReticle) LOAD_BOOL(squareTicker) LOAD_BOOL(squareBarcode) LOAD_BOOL(squareDiagnostic) LOAD_BOOL(squareSignalMap) LOAD_BOOL(squareCompass) LOAD_BOOL(squareFlowMap) LOAD_BOOL(squareTimeline) LOAD_BOOL(squareScope) LOAD_BOOL(squareProfiler) LOAD_BOOL(squarePacketGrid) LOAD_BOOL(squareMemoryMap) LOAD_BOOL(squareClock) LOAD_BOOL(squareVectorField) LOAD_BOOL(squareHeatmap) LOAD_BOOL(squareTargetArray) LOAD_BOOL(squareDataWindow) LOAD_BOOL(squareFragmentField) LOAD_BOOL(squarePortal) LOAD_BOOL(squareRain) LOAD_BOOL(squareScanline) LOAD_BOOL(squareEqualizer) LOAD_BOOL(squareWaveform) LOAD_BOOL(squareTree) LOAD_BOOL(squareMesh) LOAD_BOOL(squareLockGrid) LOAD_BOOL(skyBillboardGrid) LOAD_BOOL(skySquareArray) LOAD_BOOL(skyDataWall) LOAD_BOOL(skyScanPlane) LOAD_BOOL(skyTimeline) LOAD_BOOL(skyPulseGate) LOAD_BOOL(skyFloatingFrames) LOAD_BOOL(skyMegaGraph) LOAD_BOOL(movingPacketRibbon) LOAD_BOOL(movingSquareTrain) LOAD_BOOL(risingDataColumn) LOAD_BOOL(lateralScanPanel) LOAD_BOOL(driftingGridCloud) LOAD_BOOL(roamingReticle)
#undef LOAD_BOOL
}
void TechVFXModule::saveConfig(nlohmann::json&j){
    Module::saveConfig(j);
    j["spawnRate"]=spawnRate;j["maxActive"]=maxActive;j["effectRange"]=effectRange;j["sizeScale"]=sizeScale;j["animationSpeed"]=animationSpeed;j["giantCooldown"]=giantCooldown;
#define SAVE_BOOL(field) j[#field]=field;
    SAVE_BOOL(surfaceTrace) SAVE_BOOL(cornerBrackets) SAVE_BOOL(scanGrid) SAVE_BOOL(nodeNetwork) SAVE_BOOL(microGraph) SAVE_BOOL(spectrumBars) SAVE_BOOL(hexPulse) SAVE_BOOL(crosshairLock) SAVE_BOOL(dataCascade) SAVE_BOOL(circuitBranches) SAVE_BOOL(rulerTicks) SAVE_BOOL(coordinateStack) SAVE_BOOL(binaryRain) SAVE_BOOL(radarSweep) SAVE_BOOL(concentricRings) SAVE_BOOL(orbitNodes) SAVE_BOOL(waveRibbon) SAVE_BOOL(pulseColumn) SAVE_BOOL(holoCube) SAVE_BOOL(holoPyramid) SAVE_BOOL(wireSphere) SAVE_BOOL(helix) SAVE_BOOL(dnaChain) SAVE_BOOL(arcGauge) SAVE_BOOL(loadBars) SAVE_BOOL(matrixPanel) SAVE_BOOL(triangleFan) SAVE_BOOL(reticleBurst) SAVE_BOOL(horizonTicks) SAVE_BOOL(floatingTerminal) SAVE_BOOL(skyGraph) SAVE_BOOL(skyLattice) SAVE_BOOL(skyArc) SAVE_BOOL(beaconSpiral) SAVE_BOOL(dataConstellation) SAVE_BOOL(packetStream) SAVE_BOOL(voxelBracket) SAVE_BOOL(sectorMap) SAVE_BOOL(diagnosticPanel) SAVE_BOOL(orbitalBands) SAVE_BOOL(giantWorldSphere) SAVE_BOOL(worldAxisBurst) SAVE_BOOL(squarePulse) SAVE_BOOL(squareTunnel) SAVE_BOOL(squareRadar) SAVE_BOOL(squareMatrix) SAVE_BOOL(squareStack) SAVE_BOOL(squareSweep) SAVE_BOOL(squareCorners) SAVE_BOOL(squareCrossGrid) SAVE_BOOL(squareOrbit) SAVE_BOOL(squareWave) SAVE_BOOL(squareCodePanel) SAVE_BOOL(squareHistogram) SAVE_BOOL(squareNodeFrame) SAVE_BOOL(squareCircuitMap) SAVE_BOOL(squareReticle) SAVE_BOOL(squareTicker) SAVE_BOOL(squareBarcode) SAVE_BOOL(squareDiagnostic) SAVE_BOOL(squareSignalMap) SAVE_BOOL(squareCompass) SAVE_BOOL(squareFlowMap) SAVE_BOOL(squareTimeline) SAVE_BOOL(squareScope) SAVE_BOOL(squareProfiler) SAVE_BOOL(squarePacketGrid) SAVE_BOOL(squareMemoryMap) SAVE_BOOL(squareClock) SAVE_BOOL(squareVectorField) SAVE_BOOL(squareHeatmap) SAVE_BOOL(squareTargetArray) SAVE_BOOL(squareDataWindow) SAVE_BOOL(squareFragmentField) SAVE_BOOL(squarePortal) SAVE_BOOL(squareRain) SAVE_BOOL(squareScanline) SAVE_BOOL(squareEqualizer) SAVE_BOOL(squareWaveform) SAVE_BOOL(squareTree) SAVE_BOOL(squareMesh) SAVE_BOOL(squareLockGrid) SAVE_BOOL(skyBillboardGrid) SAVE_BOOL(skySquareArray) SAVE_BOOL(skyDataWall) SAVE_BOOL(skyScanPlane) SAVE_BOOL(skyTimeline) SAVE_BOOL(skyPulseGate) SAVE_BOOL(skyFloatingFrames) SAVE_BOOL(skyMegaGraph) SAVE_BOOL(movingPacketRibbon) SAVE_BOOL(movingSquareTrain) SAVE_BOOL(risingDataColumn) SAVE_BOOL(lateralScanPanel) SAVE_BOOL(driftingGridCloud) SAVE_BOOL(roamingReticle)
#undef SAVE_BOOL
}
