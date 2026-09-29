#include "worldscan.hpp"

#include "worldoverlay.hpp"
#include "core/ui/ColorUtil.hpp"
#include <pl/ModMenu.hpp>
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/DimensionIdentity.hpp>
#include <worldanalysis/sdk/world/Level.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Vec2 = worldanalysis::sdk::Vec2;
using Vec3 = worldanalysis::sdk::Vec3;
using Clock = std::chrono::steady_clock;
using Segment = worldanalysis::worldoverlay::Segment;
using Quad = worldanalysis::worldoverlay::Quad;
using Kind = worldanalysis::sdk::dimension_identity::Kind;

struct BlockPosRaw { int x=0,y=0,z=0; };
enum class MarkerKind : std::uint8_t { Passive, Hostile, LocalPlayer };
struct Dot { Vec3 world{}; MarkerKind kind=MarkerKind::Passive; };
struct ColumnSample { float height=-100000.f; std::uint32_t detailMask=0; };

using GetBlockFn = void* (*)(void*, const BlockPosRaw&);
using RuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);

WorldScanModule* g_mod=nullptr;
GetBlockFn g_getBlock=nullptr;
RuntimeActorListFn g_actorList=nullptr;
ActorIsPlayerFn g_actorIsPlayer=nullptr;
std::atomic_bool g_toggleRequested{false};
bool g_active=false;
bool g_scanLegacy=false;
Clock::time_point g_activated{};
Vec3 g_anchor{};
int g_anchorGroundY=0;
void* g_region=nullptr;
void* g_dimension=nullptr;
Kind g_kind=Kind::Unknown;
int g_centerX=0,g_centerZ=0,g_radiusBlocks=80,g_resolution=40;
int g_scanMinY=-64,g_scanMaxY=319;
std::size_t g_scanIndex=0;
std::vector<float> g_surfaceHeights;
std::vector<std::uint32_t> g_detailMasks;
std::vector<Dot> g_dots;
std::mutex g_dataMutex;
std::atomic<std::uint64_t> g_surfaceRevision{1};
constexpr float kPi=3.14159265358979323846f;
constexpr float kMissing=-100000.f;
constexpr std::size_t kLegacyColumnsPerTick=14;
constexpr std::size_t kVoxelColumnsPerTick=36;
constexpr std::size_t kMaxVoxelSegments=90000;
constexpr int kDetailDepth=31;
constexpr int kMaxActors=4096;

bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024.f&&p.y<4096.f;}
Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float smooth(float v){v=std::clamp(v,0.f,1.f);return v*v*(3.f-2.f*v);}
Vec3 horizontalLook(const Vec2&r){const float yaw=r.y*kPi/180.f;return{-std::sin(yaw),0,std::cos(yaw)};}

std::string_view blockIdentifier(const void*block){
    if(!block)return{};
    const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!worldanalysis::worldoverlay::plausible(type))return{};
    const auto address=type+worldanalysis::sdk::offsets::BlockType::mNameInfo+worldanalysis::sdk::offsets::NameInfo::mFullName+worldanalysis::sdk::offsets::HashedString::mString;
    const auto*value=reinterpret_cast<const std::string*>(address);
    if(value->size()>128||(!value->empty()&&!value->data()))return{};
    return{value->data(),value->size()};
}
bool air(std::string_view id){return id.empty()||id=="minecraft:air"||id=="minecraft:cave_air"||id=="minecraft:void_air";}
std::string_view blockIdAt(void*region,int x,int y,int z){if(!g_getBlock||!region)return{};BlockPosRaw p{x,y,z};return blockIdentifier(g_getBlock(region,p));}
bool nonAir(void*region,int x,int y,int z){return!air(blockIdAt(region,x,y,z));}

bool terrainLike(std::string_view id){
    if(id.empty())return true;
    const auto colon=id.find(':');
    const std::string_view bare=colon==std::string_view::npos?id:id.substr(colon+1);
    static constexpr std::array<std::string_view,29> terrain{{
        "grass_block","dirt","coarse_dirt","rooted_dirt","podzol","mycelium","stone","deepslate",
        "sand","red_sand","gravel","clay","mud","netherrack","crimson_nylium","warped_nylium",
        "soul_sand","soul_soil","end_stone","bedrock","snow","snow_layer","water","flowing_water",
        "lava","flowing_lava","tuff","calcite","magma"
    }};
    for(auto value:terrain)if(bare==value)return true;
    if(bare.size()>=4&&bare.ends_with("_ore"))return true;
    if(bare.find("terracotta")!=std::string_view::npos)return true;
    return false;
}

void yLimits(Kind kind,float playerY,int&minY,int&maxY){
    switch(kind){
        case Kind::Overworld:minY=-64;maxY=319;break;
        case Kind::Nether:minY=0;maxY=std::clamp(static_cast<int>(std::floor(playerY))+48,48,120);break;
        case Kind::TheEnd:minY=0;maxY=255;break;
        default:minY=-64;maxY=319;break;
    }
}

bool findGroundNear(void*region,int x,int z,int aroundY,int minY,int&ground){
    const int start=std::min(aroundY+6,384),stop=std::max(minY,aroundY-64);
    for(int y=start;y>=stop;--y){if(nonAir(region,x,y,z)&&!nonAir(region,x,y+1,z)){ground=y;return true;}}
    return false;
}

bool chooseAnchor(worldanalysis::sdk::Player*player,void*region,Kind kind,Vec3&anchor,int&ground){
    const Vec3 pos=player->position(),f=horizontalLook(player->rotation());int minY=-64,maxY=319;yLimits(kind,pos.y,minY,maxY);
    for(float distance:{2.5f,3.1f,3.7f,4.3f}){
        const Vec3 p=add(pos,mul(f,distance));const int x=static_cast<int>(std::floor(p.x)),z=static_cast<int>(std::floor(p.z));int y=0;
        if(!findGroundNear(region,x,z,static_cast<int>(std::floor(pos.y)),minY,y))continue;
        if(nonAir(region,x,y+1,z)||nonAir(region,x,y+2,z))continue;
        ground=y;anchor={x+.5f,static_cast<float>(y)+1.02f,z+.5f};return true;
    }
    return false;
}

float scanLegacyColumn(void*region,int wx,int wz,int minY,int maxY){
    if(!region)return kMissing;
    for(int y=maxY;y>=minY;--y)if(nonAir(region,wx,y,wz))return static_cast<float>(y)+1.f;
    return kMissing;
}

int findVoxelTop(void*region,int wx,int wz,int minY,int maxY){
    if(!region)return std::numeric_limits<int>::min();
    // Start close to the deployed map's ground instead of blindly walking down
    // from build height. If this column is a tall mountain, the starting sample
    // is inside it and the short upward walk finds its actual top. A 64-block
    // headroom also covers normal trees and player-built surface structures.
    const int start=std::clamp(g_anchorGroundY+64,minY,maxY);
    if(nonAir(region,wx,start,wz)){
        int y=start;
        while(y<maxY&&nonAir(region,wx,y+1,wz))++y;
        return y;
    }
    for(int y=start-1;y>=minY;--y)if(nonAir(region,wx,y,wz))return y;
    return std::numeric_limits<int>::min();
}

ColumnSample scanVoxelColumn(void*region,int wx,int wz,int minY,int maxY){
    ColumnSample result{};
    const int top=findVoxelTop(region,wx,wz,minY,maxY);
    if(top==std::numeric_limits<int>::min())return result;
    result.height=static_cast<float>(top)+1.f;
    // Keep the true top block in the height shell. Beneath it, record up to 31
    // blocks of non-terrain detail (logs/leaves/building blocks/etc.). This lets
    // the miniature show trunks, walls and other above-ground structure without
    // opening caves or rendering underground terrain.
    int terrainRun=0;
    for(int depth=1;depth<=kDetailDepth&&top-depth>=minY;++depth){
        const auto id=blockIdAt(region,wx,top-depth,wz);
        if(air(id)){terrainRun=0;continue;}
        if(terrainLike(id)){
            if(++terrainRun>=2)break;
            continue;
        }
        terrainRun=0;
        result.detailMask|=(1u<<depth);
    }
    return result;
}

bool isHostileType(std::string type){
    std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    static constexpr std::string_view hostile[]={"monster","zombie","skeleton","creeper","spider","witch","pillager","vindicator","evoker","ravager","phantom","blaze","ghast","slime","magma","drowned","husk","stray","guardian","shulker","warden","silverfish","endermite","piglinbrute","hoglin","zoglin","breeze","bogged"};
    for(auto key:hostile)if(type.find(key)!=std::string::npos)return true;
    return false;
}

void resetScanGrid(const Vec3&playerPos,bool recenter){
    if(recenter){g_centerX=static_cast<int>(std::floor(playerPos.x));g_centerZ=static_cast<int>(std::floor(playerPos.z));}
    g_radiusBlocks=std::clamp(g_mod?g_mod->scanChunks:5,2,12)*16;
    g_scanLegacy=g_mod?g_mod->legacyMode:false;
    g_resolution=g_scanLegacy?std::clamp(g_mod?g_mod->surfaceResolution:40,20,64):(g_radiusBlocks*2+1);
    g_scanIndex=0;
    const std::size_t total=static_cast<std::size_t>(g_resolution)*static_cast<std::size_t>(g_resolution);
    g_surfaceHeights.assign(total,kMissing);
    g_detailMasks.assign(total,0u);
    g_surfaceRevision.fetch_add(1,std::memory_order_release);
    yLimits(g_kind,playerPos.y,g_scanMinY,g_scanMaxY);
}

void deactivate(){
    std::lock_guard lock(g_dataMutex);
    g_active=false;g_region=nullptr;g_dimension=nullptr;g_surfaceHeights.clear();g_detailMasks.clear();g_dots.clear();g_scanIndex=0;g_surfaceRevision.fetch_add(1,std::memory_order_release);
}

void activate(worldanalysis::sdk::Player*player,void*region,void*dimension){
    Vec3 anchor{};int ground=0;const Kind kind=worldanalysis::sdk::dimension_identity::kind(dimension);
    if(!chooseAnchor(player,region,kind,anchor,ground))return;
    std::lock_guard lock(g_dataMutex);
    g_active=true;g_activated=Clock::now();g_anchor=anchor;g_anchorGroundY=ground;g_region=region;g_dimension=dimension;g_kind=kind;
    resetScanGrid(player->position(),true);g_dots.clear();
}

void scanSurfaceBudget(){
    if(!g_active||!g_region||g_resolution<2)return;
    const std::size_t total=static_cast<std::size_t>(g_resolution)*static_cast<std::size_t>(g_resolution);
    const std::size_t budget=g_scanLegacy?kLegacyColumnsPerTick:kVoxelColumnsPerTick;
    bool changed=false;
    for(std::size_t work=0;work<budget;++work){
        if(g_scanIndex>=total)g_scanIndex=0;
        const std::size_t idx=g_scanIndex++,ix=idx%static_cast<std::size_t>(g_resolution),iz=idx/static_cast<std::size_t>(g_resolution);
        int wx=0,wz=0;float nextHeight=kMissing;std::uint32_t nextMask=0u;
        if(g_scanLegacy){
            const float u=static_cast<float>(ix)/static_cast<float>(g_resolution-1),v=static_cast<float>(iz)/static_cast<float>(g_resolution-1);
            wx=static_cast<int>(std::lround(static_cast<float>(g_centerX-g_radiusBlocks)+u*static_cast<float>(g_radiusBlocks*2)));
            wz=static_cast<int>(std::lround(static_cast<float>(g_centerZ-g_radiusBlocks)+v*static_cast<float>(g_radiusBlocks*2)));
            nextHeight=scanLegacyColumn(g_region,wx,wz,g_scanMinY,g_scanMaxY);
        }else{
            wx=g_centerX-g_radiusBlocks+static_cast<int>(ix);
            wz=g_centerZ-g_radiusBlocks+static_cast<int>(iz);
            const auto sample=scanVoxelColumn(g_region,wx,wz,g_scanMinY,g_scanMaxY);
            nextHeight=sample.height;nextMask=sample.detailMask;
        }
        if(g_surfaceHeights[idx]!=nextHeight||g_detailMasks[idx]!=nextMask){
            g_surfaceHeights[idx]=nextHeight;g_detailMasks[idx]=nextMask;changed=true;
        }
    }
    if(changed)g_surfaceRevision.fetch_add(1,std::memory_order_release);
}

void updateDots(worldanalysis::sdk::Player*player){
    std::vector<Dot> dots;
    if(!player)return;
    const float r=static_cast<float>(g_radiusBlocks);
    const Vec3 self=player->position();
    if(finite(self)&&std::abs(self.x-g_centerX)<=r&&std::abs(self.z-g_centerZ)<=r)dots.push_back({self,MarkerKind::LocalPlayer});
    if(!g_actorList||!player->level()){g_dots=std::move(dots);return;}
    void*manager=player->level()->actorManager();if(!manager){g_dots=std::move(dots);return;}
    auto actors=g_actorList(manager);if(actors.size()>kMaxActors)actors.resize(kMaxActors);
    for(void*raw:actors){
        if(!raw||raw==player)continue;
        if(g_actorIsPlayer&&g_actorIsPlayer(raw))continue;
        auto*a=reinterpret_cast<worldanalysis::sdk::Actor*>(raw);if((a->categories()&(0x2u|0x4u))==0)continue;
        const Vec3 p=a->position();if(!finite(p)||std::abs(p.x-g_centerX)>r||std::abs(p.z-g_centerZ)>r)continue;
        dots.push_back({p,isHostileType(worldanalysis::worldoverlay::actorTypeName(raw))?MarkerKind::Hostile:MarkerKind::Passive});
    }
    g_dots=std::move(dots);
}

void drawSphere(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&c,float radius,float alpha){
    std::vector<Segment> lines;constexpr int n=18;lines.reserve(n*3);
    for(int ring=0;ring<3;++ring)for(int i=0;i<n;++i){
        float a=2*kPi*i/n,b=2*kPi*(i+1)/n;Vec3 p{},q{};
        if(ring==0){p={c.x+std::cos(a)*radius,c.y+std::sin(a)*radius,c.z};q={c.x+std::cos(b)*radius,c.y+std::sin(b)*radius,c.z};}
        else if(ring==1){p={c.x,c.y+std::cos(a)*radius,c.z+std::sin(a)*radius};q={c.x,c.y+std::cos(b)*radius,c.z+std::sin(b)*radius};}
        else{p={c.x+std::cos(a)*radius,c.y,c.z+std::sin(a)*radius};q={c.x+std::cos(b)*radius,c.y,c.z+std::sin(b)*radius};}
        lines.push_back({p,q});
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,lines,(static_cast<std::uint32_t>(std::clamp(alpha,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu,1.5f);
}

Vec3 legacyPoint(const Vec3&anchor,int ground,int res,int ix,int iz,float h,float reveal){
    const float lx=(static_cast<float>(ix)/static_cast<float>(res-1)-.5f)*3.f;
    const float lz=(static_cast<float>(iz)/static_cast<float>(res-1)-.5f)*3.f;
    const float relief=std::clamp((h-(static_cast<float>(ground)+1.f))*.018f,-.65f,1.18f)*reveal;
    return{anchor.x+lx*reveal,anchor.y+.03f+relief,anchor.z+lz*reveal};
}

void renderLegacySurface(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&anchor,int ground,int res,const std::vector<float>&heights,float reveal){
    std::vector<Segment> lines;lines.reserve(static_cast<std::size_t>(res*res*2));
    for(int z=0;z<res;++z)for(int x=0;x<res;++x){
        const std::size_t i=static_cast<std::size_t>(z*res+x);if(heights[i]<=kMissing*.5f)continue;
        const Vec3 p=legacyPoint(anchor,ground,res,x,z,heights[i],reveal);
        if(x+1<res&&heights[i+1]>kMissing*.5f)lines.push_back({p,legacyPoint(anchor,ground,res,x+1,z,heights[i+1],reveal)});
        if(z+1<res&&heights[i+res]>kMissing*.5f)lines.push_back({p,legacyPoint(anchor,ground,res,x,z+1,heights[i+res],reveal)});
        if((x%6)==0&&(z%6)==0)lines.push_back({{p.x,anchor.y,p.z},p});
    }
    worldanalysis::worldoverlay::drawLines(ctx,mat,lines,0xE8FFFFFFu,1.0f);
}

Vec3 voxelCorner(const Vec3&anchor,int ground,int res,int ix,int iz,float worldY,float reveal){
    const float blockScale=3.f/static_cast<float>(res);
    const float x=(static_cast<float>(ix)-static_cast<float>(res)*.5f)*blockScale;
    const float z=(static_cast<float>(iz)-static_cast<float>(res)*.5f)*blockScale;
    return{anchor.x+x*reveal,anchor.y+.03f+(worldY-static_cast<float>(ground+1))*blockScale*reveal,anchor.z+z*reveal};
}

void appendVerticalStep(std::vector<Segment>&lines,const Vec3&anchor,int ground,int res,int x0,int z0,int x1,int z1,int lowTop,int highTop,float reveal){
    if(highTop<=lowTop)return;
    const float lowY=static_cast<float>(lowTop+1),highY=static_cast<float>(highTop+1);
    lines.push_back({voxelCorner(anchor,ground,res,x0,z0,lowY,reveal),voxelCorner(anchor,ground,res,x0,z0,highY,reveal)});
    lines.push_back({voxelCorner(anchor,ground,res,x1,z1,lowY,reveal),voxelCorner(anchor,ground,res,x1,z1,highY,reveal)});
    for(int y=lowTop+1;y<=highTop;++y){
        const float wy=static_cast<float>(y+1);
        lines.push_back({voxelCorner(anchor,ground,res,x0,z0,wy,reveal),voxelCorner(anchor,ground,res,x1,z1,wy,reveal)});
        if(lines.size()>=kMaxVoxelSegments)return;
    }
}

void appendDetailCube(std::vector<Segment>&lines,const Vec3&anchor,int ground,int res,int ix,int iz,int blockY,float reveal){
    if(lines.size()+12>=kMaxVoxelSegments)return;
    const float s=3.f/static_cast<float>(res)*reveal;
    const float x0=anchor.x+(static_cast<float>(ix)-static_cast<float>(res)*.5f)*s;
    const float z0=anchor.z+(static_cast<float>(iz)-static_cast<float>(res)*.5f)*s;
    const float y0=anchor.y+.03f+(static_cast<float>(blockY-ground-1))*3.f/static_cast<float>(res)*reveal;
    const float x1=x0+s,z1=z0+s,y1=y0+s;
    const std::array<Vec3,8>v{{{x0,y0,z0},{x1,y0,z0},{x0,y1,z0},{x1,y1,z0},{x0,y0,z1},{x1,y0,z1},{x0,y1,z1},{x1,y1,z1}}};
    static constexpr int edges[12][2]={{0,1},{0,2},{1,3},{2,3},{4,5},{4,6},{5,7},{6,7},{0,4},{1,5},{2,6},{3,7}};
    for(const auto&e:edges)lines.push_back({v[e[0]],v[e[1]]});
}

std::vector<Segment> buildVoxelSurfaceSegments(const Vec3&anchor,int ground,int res,const std::vector<float>&heights,const std::vector<std::uint32_t>&details,float reveal){
    std::vector<Segment> lines;
    lines.reserve(std::min<std::size_t>(kMaxVoxelSegments,static_cast<std::size_t>(res)*static_cast<std::size_t>(res)));
    const int missing=std::numeric_limits<int>::min();
    auto topAt=[&](int x,int z)->int{
        if(x<0||z<0||x>=res||z>=res)return missing;
        const float h=heights[static_cast<std::size_t>(z*res+x)];
        return h<=kMissing*.5f?missing:static_cast<int>(std::lround(h-1.f));
    };
    auto addXRun=[&](int x0,int x1,int z,int top){
        if(top==missing||x1<=x0||lines.size()>=kMaxVoxelSegments)return;
        const float y=static_cast<float>(top+1);
        lines.push_back({voxelCorner(anchor,ground,res,x0,z,y,reveal),voxelCorner(anchor,ground,res,x1,z,y,reveal)});
    };
    auto addZRun=[&](int x,int z0,int z1,int top){
        if(top==missing||z1<=z0||lines.size()>=kMaxVoxelSegments)return;
        const float y=static_cast<float>(top+1);
        lines.push_back({voxelCorner(anchor,ground,res,x,z0,y,reveal),voxelCorner(anchor,ground,res,x,z1,y,reveal)});
    };

    // Merge collinear top-grid edges across flat runs.  The rendered grid is
    // visually identical (every block boundary still exists because the
    // perpendicular grid crosses it), but a flat 161x161 surface drops from
    // ~52k tiny segments to only a few hundred long segments.
    for(int z=0;z<res&&lines.size()<kMaxVoxelSegments;++z){
        for(int x=0;x<res;){
            const int top=topAt(x,z);if(top==missing){++x;continue;}
            const int begin=x;while(x<res&&topAt(x,z)==top)++x;addXRun(begin,x,z,top);
        }
        for(int x=0;x<res;){
            const int top=topAt(x,z);if(top==missing||!(z==res-1||topAt(x,z+1)!=top)){++x;continue;}
            const int begin=x;++x;while(x<res&&topAt(x,z)==top&&(z==res-1||topAt(x,z+1)!=top))++x;addXRun(begin,x,z+1,top);
        }
    }
    for(int x=0;x<res&&lines.size()<kMaxVoxelSegments;++x){
        for(int z=0;z<res;){
            const int top=topAt(x,z);if(top==missing){++z;continue;}
            const int begin=z;while(z<res&&topAt(x,z)==top)++z;addZRun(x,begin,z,top);
        }
        for(int z=0;z<res;){
            const int top=topAt(x,z);if(top==missing||!(x==res-1||topAt(x+1,z)!=top)){++z;continue;}
            const int begin=z;++z;while(z<res&&topAt(x,z)==top&&(x==res-1||topAt(x+1,z)!=top))++z;addZRun(x+1,begin,z,top);
        }
    }

    // Height transitions need subdivided vertical faces so cliffs, walls and
    // roofs retain the one-block grid in the Y direction.
    for(int z=0;z<res&&lines.size()<kMaxVoxelSegments;++z)for(int x=0;x<res&&lines.size()<kMaxVoxelSegments;++x){
        const int top=topAt(x,z);if(top==missing)continue;
        const int east=topAt(x+1,z);if(east!=missing&&east!=top)appendVerticalStep(lines,anchor,ground,res,x+1,z,x+1,z+1,std::min(top,east),std::max(top,east),reveal);
        const int south=topAt(x,z+1);if(south!=missing&&south!=top)appendVerticalStep(lines,anchor,ground,res,x,z+1,x+1,z+1,std::min(top,south),std::max(top,south),reveal);

        const std::uint32_t mask=details.empty()?0u:details[static_cast<std::size_t>(z*res+x)];
        for(int depth=1;depth<=kDetailDepth&&lines.size()<kMaxVoxelSegments;++depth){
            if((mask&(1u<<depth))==0u)continue;
            appendDetailCube(lines,anchor,ground,res,x,z,top-depth,reveal);
        }
    }
    return lines;
}

Vec3 markerPoint(const Vec3&anchor,int ground,int centerX,int centerZ,int radius,int res,bool legacy,const std::vector<float>&heights,const Vec3&world,float reveal){
    if(legacy){
        const float nx=(world.x-static_cast<float>(centerX))/static_cast<float>(radius),nz=(world.z-static_cast<float>(centerZ))/static_cast<float>(radius);
        int ix=std::clamp(static_cast<int>((nx*.5f+.5f)*(res-1)),0,res-1),iz=std::clamp(static_cast<int>((nz*.5f+.5f)*(res-1)),0,res-1);
        float h=heights[static_cast<std::size_t>(iz*res+ix)];if(h<=kMissing*.5f)h=static_cast<float>(ground)+1.f;
        return legacyPoint(anchor,ground,res,ix,iz,h,reveal);
    }
    const int ix=std::clamp(static_cast<int>(std::floor(world.x))-centerX+radius,0,res-1);
    const int iz=std::clamp(static_cast<int>(std::floor(world.z))-centerZ+radius,0,res-1);
    float h=heights[static_cast<std::size_t>(iz*res+ix)];if(h<=kMissing*.5f)h=static_cast<float>(ground)+1.f;
    const float s=3.f/static_cast<float>(res);
    Vec3 p=voxelCorner(anchor,ground,res,ix,iz,h,reveal);
    p.x+=s*.5f*reveal;p.z+=s*.5f*reveal;
    return p;
}

void drawFilledMarker(const worldanalysis::worldoverlay::RenderContext&ctx,void*mat,const Vec3&center,float baseRadius,std::uint32_t color,float phase,float reveal){
    const float pulse=.84f+.22f*(.5f+.5f*std::sin(phase));
    const float radius=baseRadius*pulse*std::max(.3f,reveal);
    constexpr int sides=20;
    constexpr int strips=9;
    // QUADS cannot represent a triangle fan by repeating the center vertex: that
    // produces degenerate faces on Bedrock's tessellator and was why the old
    // marker looked like an empty ring.  Fill the disk with narrow real quads.
    std::vector<Quad> fill;fill.reserve(strips);
    for(int i=0;i<strips;++i){
        const float z0=-radius+2.f*radius*static_cast<float>(i)/strips;
        const float z1=-radius+2.f*radius*static_cast<float>(i+1)/strips;
        const float zm=(z0+z1)*.5f;
        const float half=std::sqrt(std::max(0.f,radius*radius-zm*zm));
        // Counter-clockwise from above (+Y normal).  The depth-tested material
        // culls the opposite winding, which made the previous fill disappear
        // while the line ring remained visible.
        fill.push_back({{center.x-half,center.y,center.z+z0},{center.x-half,center.y,center.z+z1},
                        {center.x+half,center.y,center.z+z1},{center.x+half,center.y,center.z+z0}});
    }
    std::vector<Segment> ring;ring.reserve(sides);
    for(int i=0;i<sides;++i){
        const float a=2.f*kPi*static_cast<float>(i)/sides,b=2.f*kPi*static_cast<float>(i+1)/sides;
        ring.push_back({{center.x+std::cos(a)*radius,center.y,center.z+std::sin(a)*radius},
                        {center.x+std::cos(b)*radius,center.y,center.z+std::sin(b)*radius}});
    }
    const std::uint32_t filled=(0xF2u<<24)|(color&0x00FFFFFFu);
    worldanalysis::worldoverlay::drawQuads(ctx,mat,fill,filled);
    worldanalysis::worldoverlay::drawLines(ctx,mat,ring,(0xFFu<<24)|(color&0x00FFFFFFu),1.4f);
}

void renderWorld(void*levelRenderer,void*screen,void*){
    if(!g_mod||!g_mod->enabled)return;
    bool active=false,legacy=false;Clock::time_point activated{};Vec3 anchor{};int ground=0,centerX=0,centerZ=0,radius=1,res=0;
    std::vector<float> heights;std::vector<Dot>dots;
    const std::uint64_t revision=g_surfaceRevision.load(std::memory_order_acquire);
    {
        std::lock_guard lock(g_dataMutex);active=g_active;if(!active)return;legacy=g_scanLegacy;activated=g_activated;anchor=g_anchor;ground=g_anchorGroundY;
        centerX=g_centerX;centerZ=g_centerZ;radius=g_radiusBlocks;res=g_resolution;heights=g_surfaceHeights;dots=g_dots;
    }
    worldanalysis::worldoverlay::RenderContext ctx{};if(!worldanalysis::worldoverlay::makeContext(levelRenderer,screen,ctx))return;
    void*mat=worldanalysis::worldoverlay::depthMaterial();if(!mat)return;
    worldanalysis::worldoverlay::ColorScope scope(g_mod->color);
    const float age=std::chrono::duration<float>(Clock::now()-activated).count();float ballAlpha=1.f;
    if(age<.92f){
        float y=anchor.y+2.0f;
        if(age<.56f){const float t=smooth(age/.56f);y=anchor.y+2.f*(1.f-t)+.18f*t;}
        else{const float t=(age-.56f)/.36f;y=anchor.y+.18f+std::sin(std::clamp(t,0.f,1.f)*kPi)*.52f;ballAlpha=1.f-.18f*t;}
        drawSphere(ctx,mat,{anchor.x,y,anchor.z},.16f,ballAlpha);return;
    }
    const float reveal=smooth((age-.92f)/.55f);if(reveal<1.f)drawSphere(ctx,mat,{anchor.x,anchor.y+.14f,anchor.z},.16f*(1.f-reveal),1.f-reveal);
    if(res<2||heights.size()!=static_cast<std::size_t>(res)*static_cast<std::size_t>(res))return;
    if(legacy)renderLegacySurface(ctx,mat,anchor,ground,res,heights,reveal);
    else{
        struct Cache{std::uint64_t revision=0;int res=0,ground=0;float reveal=-1.f;Vec3 anchor{};Clock::time_point built{};std::vector<Segment> lines;};
        static Cache cache;
        const auto now=Clock::now();
        const bool revealChanged=std::abs(cache.reveal-reveal)>.025f;
        const bool stale=cache.built.time_since_epoch().count()==0||std::chrono::duration<float>(now-cache.built).count()>=.24f;
        if(cache.revision!=revision||cache.res!=res||cache.ground!=ground||revealChanged){
            if(stale||reveal<.999f||cache.lines.empty()){
                std::vector<std::uint32_t> details;
                {std::lock_guard lock(g_dataMutex);if(g_active&&g_resolution==res)details=g_detailMasks;}
                cache.lines=buildVoxelSurfaceSegments(anchor,ground,res,heights,details,reveal);
                cache.revision=revision;cache.res=res;cache.ground=ground;cache.reveal=reveal;cache.anchor=anchor;cache.built=now;
            }
        }
        if(!cache.lines.empty())worldanalysis::worldoverlay::drawLines(ctx,mat,cache.lines,0xE8FFFFFFu,1.0f);
    }

    const float markerBase=legacy?.055f:std::clamp(3.f/static_cast<float>(res)*1.9f,.021f,.060f);
    worldanalysis::worldoverlay::ColorScope semantic("#FFFFFF");
    for(std::size_t i=0;i<dots.size();++i){
        const auto&d=dots[i];
        if(std::abs(d.world.x-centerX)>radius||std::abs(d.world.z-centerZ)>radius)continue;
        Vec3 p=markerPoint(anchor,ground,centerX,centerZ,radius,res,legacy,heights,d.world,reveal);p.y+=.020f;
        std::uint32_t color=0xFF45E875u;
        if(d.kind==MarkerKind::Hostile)color=0xFFFF4B4Bu;
        else if(d.kind==MarkerKind::LocalPlayer)color=0xFFB66CFFu;
        drawFilledMarker(ctx,mat,p,markerBase,color,age*5.5f+static_cast<float>(i)*.73f,reveal);
    }
}

} // namespace

WorldScanModule::WorldScanModule()
    : Module("World Scan","Adds a launcher-native on-screen HUD button that deploys a live 3x3-block depth-tested miniature. Legacy Mode is the default low-overhead sampled heightmap. Turning Legacy Mode off enables the denser block-by-block miniature with cached geometry, including above-ground detail such as trees and structures. Passive mobs pulse green, hostile mobs red, and your local player pulses purple.") {
    g_mod=this;showInMenu=true;exposeBackgroundStyle=true;
}
WorldScanModule::~WorldScanModule(){if(g_mod==this)g_mod=nullptr;}
void WorldScanModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorManagerList))g_actorList=reinterpret_cast<RuntimeActorListFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorIsPlayer))g_actorIsPlayer=reinterpret_cast<ActorIsPlayerFn>(a);
    worldanalysis::worldoverlay::initialize();worldanalysis::worldoverlay::registerRenderCallback(renderWorld);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod&&g_mod->enabled)g_mod->handleTick(e.player);});
}
void WorldScanModule::onEnable(){g_toggleRequested.store(false);deactivate();}
void WorldScanModule::onDisable(){g_toggleRequested.store(false);deactivate();}
void WorldScanModule::registerLauncherButton(){
    constexpr std::string_view buttonId="worldanalysis.World Scan.button";
    pl::modmenu::unregisterButton(buttonId);
    const std::uint32_t fg=worldanalysis::ui::parseColorOr(color,0xFFFFFFFFu);
    const std::uint32_t bg=worldanalysis::ui::parseColorOr(backgroundColor,0xFF000000u);
    const float opacity=std::clamp(backgroundOpacity*buttonOpacity,0.05f,1.0f);
    const std::uint32_t normal=worldanalysis::ui::withAlpha(bg,opacity*.82f);
    const std::uint32_t active=(static_cast<std::uint32_t>(std::clamp(opacity*.92f,0.f,1.f)*255.f+.5f)<<24)|(fg&0x00FFFFFFu);
    const std::uint32_t border=worldanalysis::ui::withAlpha(fg,.92f);
    const std::uint32_t activeText=0xFF000000u|(bg&0x00FFFFFFu);
    mButtonRegistered=pl::modmenu::ButtonBuilder(std::string(buttonId),"World Scan")
        .moduleId(moduleId).label("SCAN").behavior(pl::modmenu::ButtonBehavior::Click).defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent).styleColors(normal,active,border)
        .textColor(worldanalysis::ui::withAlpha(fg,1.0f)).activeTextColor(activeText)
        .sizeScale(std::clamp(buttonScale,.5f,2.f),std::clamp(buttonScale,.5f,2.f))
        .onEvent([](std::string_view,pl::modmenu::ButtonEvent event,float){if(event==pl::modmenu::ButtonEvent::Click&&g_mod&&g_mod->enabled)g_toggleRequested.store(true);})
        .registerButton();
}

void WorldScanModule::handleTick(worldanalysis::sdk::Player*player){
    if(!enabled||!player||!g_getBlock){deactivate();return;}
    auto*dimension=player->dimension();void*region=dimension?dimension->blockSource():nullptr;if(!dimension||!region){deactivate();return;}
    if(g_toggleRequested.exchange(false)){
        bool active=false;{std::lock_guard lock(g_dataMutex);active=g_active;}
        if(active)deactivate();else activate(player,region,dimension);return;
    }
    std::lock_guard lock(g_dataMutex);if(!g_active)return;
    if(g_region!=region||g_dimension!=dimension){g_active=false;g_surfaceHeights.clear();g_detailMasks.clear();g_dots.clear();return;}
    const Vec3 p=player->position();if(!finite(p))return;
    const int desiredRadius=std::clamp(scanChunks,2,12)*16;
    const bool desiredLegacy=legacyMode;
    const int desiredRes=desiredLegacy?std::clamp(surfaceResolution,20,64):(desiredRadius*2+1);
    if(desiredRadius!=g_radiusBlocks||desiredRes!=g_resolution||desiredLegacy!=g_scanLegacy)resetScanGrid(p,false);
    scanSurfaceBudget();updateDots(player);
}
void WorldScanModule::loadConfig(const nlohmann::json&j){
    Module::loadConfig(j);
    buttonScale=std::clamp(j.value("buttonScale",buttonScale),.5f,2.f);
    buttonOpacity=std::clamp(j.value("buttonOpacity",buttonOpacity),.05f,1.f);
    scanChunks=std::clamp(j.value("scanChunks",scanChunks),2,12);
    surfaceResolution=std::clamp(j.value("surfaceResolution",surfaceResolution),20,64);
    legacyMode=j.value("legacyMode",legacyMode);
}
void WorldScanModule::saveConfig(nlohmann::json&j){
    Module::saveConfig(j);
    j["buttonScale"]=buttonScale;j["buttonOpacity"]=buttonOpacity;j["scanChunks"]=scanChunks;
    j["surfaceResolution"]=surfaceResolution;j["legacyMode"]=legacyMode;
}
