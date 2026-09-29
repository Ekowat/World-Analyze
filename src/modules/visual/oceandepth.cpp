#include "oceandepth.hpp"

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Vec3 = worldanalysis::sdk::Vec3;
using Clock = std::chrono::steady_clock;
using Kind = worldanalysis::sdk::dimension_identity::Kind;

struct BlockPosRaw { int x=0,y=0,z=0; };
struct SurfaceColumn { int x=0,z=0,surfaceY=0; };
struct SearchOffset { int dx=0,dz=0,d2=0; };
using GetBlockFn = void* (*)(void*, const BlockPosRaw&);

OceanDepthModule* g_mod = nullptr;
GetBlockFn g_getBlock = nullptr;
std::mutex g_mutex;
std::atomic_bool g_toggleRequested{false};

void* g_region = nullptr;
void* g_dimension = nullptr;
Kind g_kind = Kind::Unknown;
int g_minY = -64;
int g_maxY = 319;

std::vector<SearchOffset> g_searchOffsets;
std::size_t g_searchIndex = 0;
int g_searchCenterX = 0;
int g_searchCenterZ = 0;
int g_searchTopY = 0;
int g_searchBottomY = 0;
int g_searchY = 0;
bool g_searchColumnActive = false;

// Independent nearest-water probe. This keeps running after a body has been
// measured, allowing the module to replace the old result as soon as a closer
// connected body becomes the player's nearest water source.
std::vector<SearchOffset> g_probeOffsets;
std::size_t g_probeIndex = 0;
int g_probeCenterX = 0;
int g_probeCenterZ = 0;
int g_probeTopY = 0;
int g_probeBottomY = 0;
int g_probeY = 0;
bool g_probeColumnActive = false;
int g_probeCooldownTicks = 0;

std::deque<SurfaceColumn> g_frontier;
std::unordered_set<std::uint64_t> g_seenColumns;
std::unordered_map<std::uint64_t, int> g_surfaceColumns;
std::uint64_t g_totalWaterDepth = 0;
std::size_t g_depthColumns = 0;
bool g_floodActive = false;
bool g_floodComplete = false;
Clock::time_point g_scanStarted{};

bool g_panelVisible = false;
Vec3 g_panelAnchor{};
float g_averageDepth = 0.0f;
std::size_t g_knownColumns = 0;
bool g_panelScanning = false;
int g_anchorRefreshTicks = 0;
int g_noNearbyTicks = 0;

constexpr std::size_t kSearchQueriesPerTick = 1800;
constexpr std::size_t kProbeQueriesPerTick = 900;
constexpr std::size_t kFloodColumnsPerTick = 18;
constexpr std::size_t kMaxSurfaceColumns = 262144;

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 3e7f && std::abs(p.z) < 3e7f
        && p.y > -1024.f && p.y < 4096.f;
}
Vec3 add(const Vec3& a,const Vec3& b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(const Vec3& a,float s){return{a.x*s,a.y*s,a.z*s};}
float smooth(float v){v=std::clamp(v,0.f,1.f);return v*v*(3.f-2.f*v);}
std::uint32_t white(float a){return(static_cast<std::uint32_t>(std::clamp(a,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu;}

std::uint64_t columnKey(int x,int z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
        | static_cast<std::uint32_t>(z);
}
void unpackColumn(std::uint64_t key,int& x,int& z) {
    x = static_cast<std::int32_t>(key >> 32);
    z = static_cast<std::int32_t>(key & 0xFFFFFFFFu);
}

void yLimits(Kind kind,int& minY,int& maxY) {
    switch(kind) {
        case Kind::Overworld: minY=-64; maxY=319; break;
        case Kind::Nether: minY=0; maxY=127; break;
        case Kind::TheEnd: minY=0; maxY=255; break;
        default: minY=-64; maxY=319; break;
    }
}

std::string_view blockIdentifier(const void* block) {
    if(!block) return {};
    const auto type=*reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!worldanalysis::worldoverlay::plausible(type)) return {};
    const auto address=type+worldanalysis::sdk::offsets::BlockType::mNameInfo
        +worldanalysis::sdk::offsets::NameInfo::mFullName
        +worldanalysis::sdk::offsets::HashedString::mString;
    const auto* value=reinterpret_cast<const std::string*>(address);
    if(!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(value))
        || value->size()>128 || (!value->empty()&&!value->data())) return {};
    return {value->data(),value->size()};
}
bool isWaterId(std::string_view id) {
    if(id.empty()) return false;
    const auto colon=id.find(':');
    const auto bare=colon==std::string_view::npos?id:id.substr(colon+1);
    return bare=="water" || bare=="flowing_water";
}
bool waterAt(void* region,int x,int y,int z) {
    if(!g_getBlock||!region||y<g_minY||y>g_maxY) return false;
    const BlockPosRaw p{x,y,z};
    return isWaterId(blockIdentifier(g_getBlock(region,p)));
}

void clearScanLocked() {
    g_searchOffsets.clear();
    g_searchIndex=0;
    g_searchColumnActive=false;
    g_probeOffsets.clear();
    g_probeIndex=0;
    g_probeColumnActive=false;
    g_probeCooldownTicks=0;
    g_frontier.clear();
    g_seenColumns.clear();
    g_surfaceColumns.clear();
    g_totalWaterDepth=0;
    g_depthColumns=0;
    g_floodActive=false;
    g_floodComplete=false;
    g_panelVisible=false;
    g_panelAnchor={};
    g_averageDepth=0;
    g_knownColumns=0;
    g_panelScanning=false;
    g_anchorRefreshTicks=0;
    g_noNearbyTicks=0;
}

void buildSearchLocked(const Vec3& player,float radius) {
    g_searchOffsets.clear();
    const int r=std::clamp(static_cast<int>(std::ceil(radius)),8,96);
    g_searchOffsets.reserve(static_cast<std::size_t>((r*2+1)*(r*2+1)));
    for(int dz=-r;dz<=r;++dz) for(int dx=-r;dx<=r;++dx) {
        const int d2=dx*dx+dz*dz;
        if(d2<=r*r) g_searchOffsets.push_back({dx,dz,d2});
    }
    std::sort(g_searchOffsets.begin(),g_searchOffsets.end(),[](const SearchOffset&a,const SearchOffset&b){return a.d2<b.d2;});
    g_searchCenterX=static_cast<int>(std::floor(player.x));
    g_searchCenterZ=static_cast<int>(std::floor(player.z));
    const int py=static_cast<int>(std::floor(player.y));
    g_searchTopY=std::min(g_maxY,py+r);
    g_searchBottomY=std::max(g_minY,py-r);
    g_searchIndex=0;
    g_searchY=g_searchTopY;
    g_searchColumnActive=false;
}

int normalizeSurfaceY(void* region,int x,int y,int z) {
    int top=std::clamp(y,g_minY,g_maxY);
    while(top<g_maxY && waterAt(region,x,top+1,z)) ++top;
    return top;
}

void startFloodLocked(void* region,int x,int y,int z) {
    g_frontier.clear();
    g_seenColumns.clear();
    g_surfaceColumns.clear();
    g_totalWaterDepth=0;
    g_depthColumns=0;
    const int top=normalizeSurfaceY(region,x,y,z);
    g_frontier.push_back({x,z,top});
    g_seenColumns.insert(columnKey(x,z));
    g_floodActive=true;
    g_floodComplete=false;
    g_panelVisible=false;
    g_averageDepth=0.f;
    g_knownColumns=0;
    g_panelScanning=true;
    g_noNearbyTicks=0;
    g_scanStarted=Clock::now();
    g_searchOffsets.clear();
    g_searchColumnActive=false;
}

bool findConnectedSurface(void* region,int x,int z,int bottomY,int topY,int& surfaceY) {
    // Test the full vertical span of the current water column. This keeps the
    // expansion faithful to actual 6-connected water: a neighboring column is
    // accepted if any water block touches this column at the same Y, even when
    // that connection is underwater rather than at the visible surface.
    for(int y=topY;y>=bottomY;--y) {
        if(!waterAt(region,x,y,z)) continue;
        surfaceY=normalizeSurfaceY(region,x,y,z);
        return true;
    }
    return false;
}

void processSearchLocked(void* region,const Vec3& player,float radius) {
    if(g_searchOffsets.empty()) buildSearchLocked(player,radius);
    std::size_t queries=0;
    while(queries<kSearchQueriesPerTick && g_searchIndex<g_searchOffsets.size()) {
        const auto& off=g_searchOffsets[g_searchIndex];
        const int x=g_searchCenterX+off.dx,z=g_searchCenterZ+off.dz;
        if(!g_searchColumnActive) { g_searchY=g_searchTopY; g_searchColumnActive=true; }
        while(queries<kSearchQueriesPerTick && g_searchY>=g_searchBottomY) {
            ++queries;
            if(waterAt(region,x,g_searchY,z)) {
                startFloodLocked(region,x,g_searchY,z);
                return;
            }
            --g_searchY;
        }
        if(g_searchY<g_searchBottomY) { ++g_searchIndex; g_searchColumnActive=false; }
    }
    // If the player moved materially while a long dry search was in progress,
    // restart around the new location so the "closest" source remains useful.
    const float dx=player.x-(g_searchCenterX+.5f),dz=player.z-(g_searchCenterZ+.5f);
    if(dx*dx+dz*dz>16.f) buildSearchLocked(player,radius);
}

void buildProbeLocked(const Vec3& player,float radius) {
    g_probeOffsets.clear();
    const int r=std::clamp(static_cast<int>(std::ceil(radius)),8,96);
    g_probeOffsets.reserve(static_cast<std::size_t>((r*2+1)*(r*2+1)));
    for(int dz=-r;dz<=r;++dz) for(int dx=-r;dx<=r;++dx) {
        const int d2=dx*dx+dz*dz;
        if(d2<=r*r) g_probeOffsets.push_back({dx,dz,d2});
    }
    std::sort(g_probeOffsets.begin(),g_probeOffsets.end(),[](const SearchOffset&a,const SearchOffset&b){return a.d2<b.d2;});
    g_probeCenterX=static_cast<int>(std::floor(player.x));
    g_probeCenterZ=static_cast<int>(std::floor(player.z));
    const int py=static_cast<int>(std::floor(player.y));
    g_probeTopY=std::min(g_maxY,py+r);
    g_probeBottomY=std::max(g_minY,py-r);
    g_probeIndex=0;
    g_probeY=g_probeTopY;
    g_probeColumnActive=false;
}

void finishProbeLocked(int cooldownTicks=20) {
    g_probeOffsets.clear();
    g_probeIndex=0;
    g_probeColumnActive=false;
    g_probeCooldownTicks=cooldownTicks;
}

void processNearestProbeLocked(void* region,const Vec3& player,float radius) {
    if(g_depthColumns==0)return;
    if(g_probeCooldownTicks>0){--g_probeCooldownTicks;return;}
    if(g_probeOffsets.empty())buildProbeLocked(player,radius);

    // If the player moves while the probe is still walking a dry area, restart
    // from the new position so the first water hit remains the actual nearest.
    const float movedX=player.x-(g_probeCenterX+.5f),movedZ=player.z-(g_probeCenterZ+.5f);
    if(movedX*movedX+movedZ*movedZ>9.f)buildProbeLocked(player,radius);

    std::size_t queries=0;
    while(queries<kProbeQueriesPerTick&&g_probeIndex<g_probeOffsets.size()) {
        const auto& off=g_probeOffsets[g_probeIndex];
        const int x=g_probeCenterX+off.dx,z=g_probeCenterZ+off.dz;
        if(!g_probeColumnActive){g_probeY=g_probeTopY;g_probeColumnActive=true;}
        while(queries<kProbeQueriesPerTick&&g_probeY>=g_probeBottomY) {
            ++queries;
            if(waterAt(region,x,g_probeY,z)) {
                const auto key=columnKey(x,z);
                const bool belongsToCurrent=g_seenColumns.contains(key)||g_surfaceColumns.contains(key);
                if(!belongsToCurrent&&g_floodComplete) {
                    // The closest water in the current radius belongs to another
                    // body, so immediately replace the stale completed result.
                    startFloodLocked(region,x,g_probeY,z);
                    finishProbeLocked(30);
                    return;
                }
                if(belongsToCurrent) {
                    const int top=normalizeSurfaceY(region,x,g_probeY,z);
                    g_panelAnchor={x+.5f,static_cast<float>(top)+2.35f,z+.5f};
                    g_panelVisible=g_depthColumns>0;
                    g_noNearbyTicks=0;
                }
                finishProbeLocked(20);
                return;
            }
            --g_probeY;
        }
        if(g_probeY<g_probeBottomY){++g_probeIndex;g_probeColumnActive=false;}
    }
    if(g_probeIndex>=g_probeOffsets.size())finishProbeLocked(10);
}

void processFloodLocked(void* region) {
    std::size_t processed=0;
    while(processed<kFloodColumnsPerTick && !g_frontier.empty()
        && g_surfaceColumns.size()<kMaxSurfaceColumns) {
        const SurfaceColumn c=g_frontier.front();
        g_frontier.pop_front();
        ++processed;

        int depth=0;
        for(int y=c.surfaceY;y>=g_minY&&waterAt(region,c.x,y,c.z);--y) ++depth;
        if(depth<=0) continue;
        const int bottomY=c.surfaceY-depth+1;
        g_surfaceColumns[columnKey(c.x,c.z)]=c.surfaceY;
        g_totalWaterDepth+=static_cast<std::uint64_t>(depth);
        ++g_depthColumns;

        static constexpr int dx[4]={1,-1,0,0};
        static constexpr int dz[4]={0,0,1,-1};
        for(int i=0;i<4;++i) {
            const int nx=c.x+dx[i],nz=c.z+dz[i];
            const auto key=columnKey(nx,nz);
            if(g_seenColumns.contains(key)) continue;
            int sy=0;
            if(findConnectedSurface(region,nx,nz,bottomY,c.surfaceY,sy)) {
                g_seenColumns.insert(key);
                g_frontier.push_back({nx,nz,sy});
            }
        }
    }
    if(g_frontier.empty() || g_surfaceColumns.size()>=kMaxSurfaceColumns) {
        g_floodActive=false;
        g_floodComplete=true;
    }
    if(g_depthColumns)
        g_averageDepth=static_cast<float>(g_totalWaterDepth)/static_cast<float>(g_depthColumns);
    g_knownColumns=g_depthColumns;
    g_panelScanning=!g_floodComplete;
}

bool findNearestKnownSurfaceLocked(const Vec3& player,float radius,Vec3& anchor) {
    if(g_surfaceColumns.empty()) return false;
    const int cx=static_cast<int>(std::floor(player.x));
    const int cz=static_cast<int>(std::floor(player.z));
    const int r=std::clamp(static_cast<int>(std::ceil(radius)),8,96);
    float best=radius*radius+1.f;
    bool found=false;
    for(int ring=0;ring<=r;++ring) {
        const int minX=cx-ring,maxX=cx+ring,minZ=cz-ring,maxZ=cz+ring;
        auto test=[&](int x,int z){
            const auto it=g_surfaceColumns.find(columnKey(x,z));
            if(it==g_surfaceColumns.end()) return;
            const float dx=(x+.5f)-player.x,dz=(z+.5f)-player.z;
            const float d2=dx*dx+dz*dz;
            if(d2<=radius*radius && d2<best) {
                best=d2;found=true;anchor={x+.5f,static_cast<float>(it->second)+2.35f,z+.5f};
            }
        };
        if(ring==0) test(cx,cz);
        else {
            for(int x=minX;x<=maxX;++x){test(x,minZ);test(x,maxZ);}
            for(int z=minZ+1;z<maxZ;++z){test(minX,z);test(maxX,z);}
        }
        if(found) return true; // first occupied Chebyshev ring is the closest useful source
    }
    return false;
}

void refreshPanelAnchorLocked(const Vec3& player,float radius) {
    Vec3 anchor{};
    if(findNearestKnownSurfaceLocked(player,radius,anchor)) {
        g_panelAnchor=anchor;
        g_panelVisible=g_depthColumns>0;
        g_noNearbyTicks=0;
    } else {
        g_panelVisible=false;
        ++g_noNearbyTicks;
    }
}

void renderOceanDepth(void* self,void* screen,void*) {
    if(!g_mod||!g_mod->enabled) return;
    Vec3 anchor{};float avg=0.f;std::size_t columns=0;bool visible=false,scanning=false;
    {
        std::lock_guard lock(g_mutex);
        anchor=g_panelAnchor;avg=g_averageDepth;columns=g_knownColumns;
        visible=g_panelVisible;scanning=g_panelScanning;
    }
    if(!visible||columns==0||!finite(anchor)) return;

    worldanalysis::worldoverlay::ColorScope scope(g_mod->color,g_mod->backgroundColor,g_mod->backgroundOpacity);
    worldanalysis::worldoverlay::RenderContext ctx{};
    if(!worldanalysis::worldoverlay::makeContext(self,screen,ctx)) return;
    void* mat=worldanalysis::worldoverlay::throughWallMaterial();
    if(!mat) return;

    Vec3 right{},up{};
    worldanalysis::worldoverlay::billboardBasis(anchor,ctx.camera,right,up);
    const float pulse=.96f+.04f*std::sin(std::chrono::duration<float>(Clock::now().time_since_epoch()).count()*2.2f);
    const float hw=.86f*pulse,hh=.34f*pulse;
    auto p=[&](float x,float y){return add(anchor,add(mul(right,x),mul(up,y)));};
    const Vec3 bl=p(-hw,-hh),br=p(hw,-hh),tr=p(hw,hh),tl=p(-hw,hh);
    worldanalysis::worldoverlay::drawQuads(ctx,mat,{{bl,br,tr,tl}},0x26000000u);
    worldanalysis::worldoverlay::drawLines(ctx,mat,{{bl,br},{br,tr},{tr,tl},{tl,bl}},white(.98f),2.0f);

    char body[96]{};
    std::snprintf(body,sizeof(body),"AVERAGE DEPTH %.1f BLOCKS",static_cast<double>(avg));
    const float titlePx=worldanalysis::worldoverlay::fitBillboardTextPixelSize("OCEAN DEPTH",1.42f,.020f,.009f);
    const float bodyPx=worldanalysis::worldoverlay::fitBillboardTextPixelSize(body,1.45f,.0145f,.0075f);
    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,"OCEAN DEPTH",p(0,.17f),right,up,titlePx,white(1.f),false);
    worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,body,p(0,.015f),right,up,bodyPx,white(.96f),false);
    if(scanning) {
        char status[80]{};
        std::snprintf(status,sizeof(status),"SCANNING CONNECTED WATER  %zu COLUMNS",columns);
        const float px=worldanalysis::worldoverlay::fitBillboardTextPixelSize(status,1.42f,.0092f,.0058f);
        worldanalysis::worldoverlay::drawBillboardTextOriented(ctx,mat,status,p(0,-.17f),right,up,px,white(.62f),false);
    }
}

void resetAll() {
    std::lock_guard lock(g_mutex);
    clearScanLocked();
    g_region=nullptr;g_dimension=nullptr;g_kind=Kind::Unknown;
}
} // namespace

OceanDepthModule::OceanDepthModule()
    : Module("Ocean Depth",
        "Continuously prioritizes the closest connected water body within the notification radius, follows that body beyond the radius through already-loaded client blocks, and reports its average water-column depth from a world-space panel. A launcher-native DEPTH button can pause/resume scanning without disabling the module.") {
    showInMenu=true;
    exposeBackgroundStyle=true;
    g_mod=this;
}

OceanDepthModule::~OceanDepthModule(){pl::modmenu::unregisterButton("worldanalysis.Ocean Depth.button");if(g_mod==this)g_mod=nullptr;}

void OceanDepthModule::onInit() {
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))
        g_getBlock=reinterpret_cast<GetBlockFn>(a);
    worldanalysis::worldoverlay::initialize();
    worldanalysis::worldoverlay::registerRenderCallback(renderOceanDepth);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto& e){if(g_mod)g_mod->handleTick(e.player);});
}
void OceanDepthModule::onEnable(){mScanActive=true;g_toggleRequested.store(false);resetAll();registerLauncherButton();}
void OceanDepthModule::onDisable(){mScanActive=false;g_toggleRequested.store(false);resetAll();}

void OceanDepthModule::registerLauncherButton(){
    constexpr std::string_view buttonId="worldanalysis.Ocean Depth.button";
    pl::modmenu::unregisterButton(buttonId);
    const std::uint32_t fg=worldanalysis::ui::parseColorOr(color,0xFFFFFFFFu);
    const std::uint32_t bg=worldanalysis::ui::parseColorOr(backgroundColor,0xFF000000u);
    const float opacity=std::clamp(backgroundOpacity,0.08f,1.0f);
    const std::uint32_t normal=worldanalysis::ui::withAlpha(bg,opacity*.84f);
    const std::uint32_t active=(static_cast<std::uint32_t>(std::clamp(opacity*.94f,0.f,1.f)*255.f+.5f)<<24)|(fg&0x00FFFFFFu);
    mButtonRegistered=pl::modmenu::ButtonBuilder(std::string(buttonId),"Ocean Depth")
        .moduleId(moduleId).label("DEPTH").behavior(pl::modmenu::ButtonBehavior::Click).defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent).styleColors(normal,active,worldanalysis::ui::withAlpha(fg,.92f))
        .textColor(worldanalysis::ui::withAlpha(fg,1.0f)).activeTextColor(0xFF000000u|(bg&0x00FFFFFFu))
        .onEvent([](std::string_view,pl::modmenu::ButtonEvent event,float){
            if(event==pl::modmenu::ButtonEvent::Click&&g_mod&&g_mod->enabled)g_toggleRequested.store(true);
        }).registerButton();
}

void OceanDepthModule::handleTick(worldanalysis::sdk::Player* player) {
    if(!enabled||!player||!g_getBlock) return;
    if(g_toggleRequested.exchange(false)) {
        mScanActive=!mScanActive;
        resetAll();
        if(!mScanActive)return;
    }
    if(!mScanActive)return;
    auto* dim=player->dimension();
    void* region=dim?dim->blockSource():nullptr;
    const Vec3 pos=player->position();
    if(!dim||!region||!finite(pos)) { resetAll(); return; }
    const float radius=std::clamp(notificationRadius,8.f,96.f);

    std::lock_guard lock(g_mutex);
    const Kind kind=worldanalysis::sdk::dimension_identity::kind(dim);
    if(region!=g_region||dim!=g_dimension||kind!=g_kind) {
        clearScanLocked();
        g_region=region;g_dimension=dim;g_kind=kind;
        yLimits(kind,g_minY,g_maxY);
        buildSearchLocked(pos,radius);
    }

    if(!g_floodActive&&!g_floodComplete) processSearchLocked(region,pos,radius);
    if(g_floodActive) processFloodLocked(region);
    if(g_depthColumns>0) processNearestProbeLocked(region,pos,radius);

    if(++g_anchorRefreshTicks>=5) {
        g_anchorRefreshTicks=0;
        refreshPanelAnchorLocked(pos,radius);
    }

    // If the completed body's known surface has entirely left the radius and
    // the nearest-water probe has not found a replacement yet, restart the
    // normal closest-water search around the player's current position.
    if(g_floodComplete&&g_noNearbyTicks>20&&g_probeOffsets.empty()&&g_probeCooldownTicks==0) {
        clearScanLocked();
        buildSearchLocked(pos,radius);
    }
}

void OceanDepthModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    notificationRadius=std::clamp(j.value("notificationRadius",notificationRadius),8.f,96.f);
}
void OceanDepthModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["notificationRadius"]=std::clamp(notificationRadius,8.f,96.f);
}
