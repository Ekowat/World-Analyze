#include "instoreviewer.hpp"

#include "config/ConfigManager.hpp"
#include "determine.hpp"
#include "itemutils.hpp"
#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/events/ScreenStateEvent.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/DimensionIdentity.hpp>
#include <worldanalysis/sdk/world/HitResult.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Vec3=worldanalysis::sdk::Vec3;
using Clock=std::chrono::steady_clock;
using Segment=worldanalysis::worldoverlay::Segment;

struct BlockPosRaw { int x=0,y=0,z=0; };
struct StorageItem {
    // This is a live ItemStack owned by the target BlockActor's Container.  It
    // is used only while that block remains the active target.  Cached entries
    // deliberately clear it instead of manufacturing an ABI-fragile stack.
    void* stack=nullptr;
    std::uintptr_t itemKey=0;
    std::int32_t id=0;
    std::int32_t damage=0;
    int count=0;
    std::uint32_t color=0xFFFFFFFFu;
    std::string name;
};
struct ContainerRead {
    bool readable=false;
    int slots=0;
    std::vector<StorageItem> items;
};
struct StorageTarget {
    bool valid=false;
    bool avoidDetermine=false;
    bool synchronized=false;
    BlockPosRaw pos{};
    void* dimension=nullptr;
    int dimensionId=0;
    int slots=0;
    Vec3 hit{};
    int side=1;
    std::vector<StorageItem> items;
    Clock::time_point acquiredAt{};
};
struct StorageCache {
    BlockPosRaw pos{};
    int dimensionId=0;
    int slots=0;
    std::vector<StorageItem> items;
    std::uint64_t updatedAt=0;
};
struct OpenSync {
    bool screenOpen=false;
    bool pending=false;
    bool closeRequested=false;
    void* controller=nullptr;
    BlockPosRaw pos{};
    void* dimension=nullptr;
    int dimensionId=0;
    Clock::time_point openedAt{};
    ContainerRead stable{};
    int emptyReadStreak=0;
};
struct DisplayRow { std::string text;std::uint32_t color=0xFFFFFFFFu; };

using LevelGetHitResultFn=worldanalysis::sdk::HitResult*(*)(void*);
using GetBlockFn=void*(*)(void*,const BlockPosRaw&);
using ContainerScreenGetItemStackFn=void*(*)(void*,const std::string&,int);

// Exact Minecraft 26.45 Android virtual layout recovered from dot45values.so:
// ChestBlockActor slots 45/46 return its +0xF0 Container subobject; Container
// slot 8 is getItem(int) and slot 21 is getContainerSize().
constexpr std::size_t kBlockSourceGetBlockActorSlot=4;
constexpr std::size_t kBlockActorGetContainerSlot=45;
constexpr std::size_t kBlockActorGetContainerConstSlot=46;
constexpr std::size_t kContainerGetItemSlot=8;
constexpr std::size_t kContainerGetSizeSlot=21;
constexpr int kMaxContainerSlots=128;
constexpr int kMaxTextRows=9;
constexpr float kBranchLength=1.20f;
constexpr float kLineAnimationSeconds=.50f;
constexpr float kTextFadeSeconds=.10f;
constexpr float kPanelAnimationSeconds=.24f;
constexpr float kHalfThickness=.0075f;
constexpr float kEndpointRadius=.075f;
constexpr float kPi=3.14159265358979323846f;
constexpr std::size_t kMaxCachedStorages=256;
constexpr int kFallbackScreenSlots=54;

InstoreViewerModule* g_mod=nullptr;
LevelGetHitResultFn g_getHitResult=nullptr;
GetBlockFn g_getBlock=nullptr;
ContainerScreenGetItemStackFn g_containerScreenGetItemStack=nullptr;
StorageTarget g_target{};
std::vector<StorageCache> g_cache;
OpenSync g_sync{};
std::mutex g_mutex;
bool g_cacheDirty=false;
Clock::time_point g_lastCacheSave{};


bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024.f&&p.y<4096.f;}
Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(const Vec3&a,const Vec3&b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float length(const Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
Vec3 normalize(const Vec3&v,const Vec3&fallback={1,0,0}){const float l=length(v);return l>.0001f?mul(v,1.f/l):fallback;}
Vec3 cross(const Vec3&a,const Vec3&b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float smoothStep(float v){v=std::clamp(v,0.f,1.f);return v*v*(3.f-2.f*v);}
bool samePos(const BlockPosRaw&a,const BlockPosRaw&b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
bool plausibleFunction(void*fn);
std::uint64_t epochMillis(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}

int dimensionCacheKey(void*dimension){
    // Never invoke BlockSource's manually indexed dimension-ID virtual from a
    // hot tick. The exact 26.45 binary exposes concrete Dimension RTTI, which
    // gives us stable 0/1/2 cache keys without an ABI-sensitive call.
    return worldanalysis::sdk::dimension_identity::cacheKey(
        worldanalysis::sdk::dimension_identity::kind(dimension));
}

bool plausibleFunction(void*fn){return worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(fn),4);}

std::string_view blockIdentifier(const void*block){
    if(!block)return{};
    const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!worldanalysis::worldoverlay::plausible(type))return{};
    const auto address=type+worldanalysis::sdk::offsets::BlockType::mNameInfo
        +worldanalysis::sdk::offsets::NameInfo::mFullName
        +worldanalysis::sdk::offsets::HashedString::mString;
    const auto*value=reinterpret_cast<const std::string*>(address);
    if(value->size()>128||(!value->empty()&&!value->data()))return{};
    return{value->data(),value->size()};
}

bool supportedStorage(std::string_view id){
    if(id.starts_with("minecraft:"))id.remove_prefix(10);
    if(id=="ender_chest")return false; // player-owned inventory, not this BlockActor
    return id.find("chest")!=std::string_view::npos
        ||id=="barrel"||id.find("shulker_box")!=std::string_view::npos
        ||id=="hopper"||id=="dispenser"||id=="dropper"||id=="crafter"
        ||id=="furnace"||id=="lit_furnace"||id=="blast_furnace"||id=="smoker"
        ||id=="brewing_stand"||id=="decorated_pot"||id=="chiseled_bookshelf";
}

void* blockActorAt(void*region,const BlockPosRaw&pos){
    if(!region||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(region)))return nullptr;
    auto**table=*reinterpret_cast<void***>(region);
    if(!table||!plausibleFunction(table[kBlockSourceGetBlockActorSlot]))return nullptr;
    using Fn=void*(*)(void*,const BlockPosRaw&);
    return reinterpret_cast<Fn>(table[kBlockSourceGetBlockActorSlot])(region,pos);
}

std::vector<void*> blockContainers(void*blockActor){
    std::vector<void*>result;
    if(!blockActor||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(blockActor)))return result;
    auto**table=*reinterpret_cast<void***>(blockActor);if(!table)return result;
    using Fn=void*(*)(void*);
    for(const auto slot:{kBlockActorGetContainerSlot,kBlockActorGetContainerConstSlot}){
        if(!plausibleFunction(table[slot]))continue;
        void*container=reinterpret_cast<Fn>(table[slot])(blockActor);
        if(!container||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(container)))continue;
        auto**containerTable=*reinterpret_cast<void***>(container);
        if(!containerTable||!plausibleFunction(containerTable[kContainerGetItemSlot])
            ||!plausibleFunction(containerTable[kContainerGetSizeSlot]))continue;
        if(std::find(result.begin(),result.end(),container)==result.end())result.push_back(container);
    }
    return result;
}

StorageItem inspectStack(void*stack){
    const auto item=worldanalysis::items::inspect(stack);
    if(!item.valid())return{};
    return{stack,item.itemKey,item.id,item.damage,item.count,item.color,item.name};
}

void mergeItem(std::vector<StorageItem>&items,const StorageItem&incoming){
    if(incoming.count<=0)return;
    auto found=std::find_if(items.begin(),items.end(),[&](const StorageItem&item){
        return item.id==incoming.id&&item.damage==incoming.damage&&item.name==incoming.name;
    });
    if(found==items.end())items.push_back(incoming);
    else found->count+=incoming.count;
}

ContainerRead readContainer(void*container,bool includeItems){
    ContainerRead result{};
    if(!container||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(container)))return result;
    auto**table=*reinterpret_cast<void***>(container);if(!table)return result;
    if(!plausibleFunction(table[kContainerGetItemSlot])||!plausibleFunction(table[kContainerGetSizeSlot]))return result;
    using SizeFn=int(*)(void*);using ItemFn=void*(*)(void*,int);
    const int slots=reinterpret_cast<SizeFn>(table[kContainerGetSizeSlot])(container);
    if(slots<=0||slots>kMaxContainerSlots)return result;
    result.readable=true;result.slots=slots;
    if(includeItems){
        auto getItem=reinterpret_cast<ItemFn>(table[kContainerGetItemSlot]);
        for(int slot=0;slot<slots;++slot)mergeItem(result.items,inspectStack(getItem(container,slot)));
    }
    return result;
}

ContainerRead readStorage(void*region,const BlockPosRaw&pos,bool includeItems=true){
    ContainerRead best{};
    if(!region||!g_getBlock)return best;
    void*block=g_getBlock(region,pos);
    if(!block||!supportedStorage(blockIdentifier(block)))return best;
    const auto containers=blockContainers(blockActorAt(region,pos));
    for(void*container:containers){
        auto current=readContainer(container,includeItems);
        if(!current.readable)continue;
        if(!best.readable||current.items.size()>best.items.size())best=std::move(current);
        if(!best.items.empty())break;
    }
    return best;
}

void readScreenCollection(void* controller,const char* collection,int slots,ContainerRead& out){
    if(!g_containerScreenGetItemStack||!controller||!collection||slots<=0)return;
    const std::string key{collection};
    for(int slot=0;slot<slots;++slot)
        mergeItem(out.items,inspectStack(g_containerScreenGetItemStack(controller,key,slot)));
}

ContainerRead readOpenScreen(void*controller,int knownSlots,std::string_view storageId){
    ContainerRead result{};
    if(!controller||!worldanalysis::worldoverlay::plausible(reinterpret_cast<std::uintptr_t>(controller)))return result;
    if(storageId.starts_with("minecraft:"))storageId.remove_prefix(10);

    // Furnace-family and brewing screens do not expose their mutable slots
    // through the chest-style "container_items" collection.  The exact 26.45
    // binary contains these collection keys and the screen controller maps
    // them independently.  Reading the generic collection made furnace state
    // look correct on open, then snap empty after the first slot update.
    if(g_containerScreenGetItemStack){
        result.readable=true;
        if(storageId=="furnace"||storageId=="lit_furnace"||storageId=="blast_furnace"||storageId=="smoker"){
            result.slots=3;
            readScreenCollection(controller,"furnace_ingredient_items",1,result);
            readScreenCollection(controller,"furnace_fuel_items",1,result);
            readScreenCollection(controller,"furnace_output_items",1,result);
            return result;
        }
        if(storageId=="brewing_stand"){
            result.slots=5;
            readScreenCollection(controller,"brewing_input_item",1,result);
            readScreenCollection(controller,"brewing_fuel_item",1,result);
            readScreenCollection(controller,"brewing_result_items",3,result);
            return result;
        }

        const int slots=std::clamp(knownSlots>0?knownSlots:kFallbackScreenSlots,1,kMaxContainerSlots);
        result.slots=slots;
        readScreenCollection(controller,"container_items",slots,result);
        return result;
    }
    return result;
}

void bindLiveStacks(std::vector<StorageItem>&cached,const std::vector<StorageItem>&live){
    for(auto&item:cached){
        const auto found=std::find_if(live.begin(),live.end(),[&](const StorageItem&candidate){
            return candidate.id==item.id&&candidate.damage==item.damage&&candidate.name==item.name;
        });
        if(found!=live.end())item.stack=found->stack;
    }
}

StorageCache* findCacheLocked(const BlockPosRaw&pos,int dimensionIdValue){
    auto found=std::find_if(g_cache.begin(),g_cache.end(),[&](const StorageCache&entry){
        return entry.dimensionId==dimensionIdValue&&samePos(entry.pos,pos);
    });
    return found==g_cache.end()?nullptr:&*found;
}

bool sameItems(const std::vector<StorageItem>&a,const std::vector<StorageItem>&b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i){
        if(a[i].id!=b[i].id||a[i].damage!=b[i].damage||a[i].count!=b[i].count
            ||a[i].color!=b[i].color||a[i].name!=b[i].name)return false;
    }
    return true;
}

nlohmann::json persistentCacheJson(){
    nlohmann::json root;root["version"]=1;root["storages"]=nlohmann::json::array();
    std::lock_guard lock(g_mutex);
    for(const auto&entry:g_cache){
        nlohmann::json storage{{"dimensionId",entry.dimensionId},{"x",entry.pos.x},{"y",entry.pos.y},{"z",entry.pos.z},{"slots",entry.slots},{"updatedAt",entry.updatedAt}};
        storage["items"]=nlohmann::json::array();
        for(const auto&item:entry.items){
            storage["items"].push_back({{"id",item.id},{"damage",item.damage},{"count",item.count},{"color",item.color},{"name",item.name}});
        }
        root["storages"].push_back(std::move(storage));
    }
    return root;
}

void savePersistentCache(){
    const auto path=std::filesystem::path(worldanalysis::config::ConfigManager::get().getDataPath("instore_cache.json"));
    const auto temp=std::filesystem::path(path.string()+".tmp");
    try{
        const auto payload=persistentCacheJson().dump();
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(payload.data(),static_cast<std::streamsize>(payload.size()));out.flush();if(!out)return;}
        std::error_code ec;std::filesystem::rename(temp,path,ec);if(ec){std::filesystem::remove(path,ec);ec.clear();std::filesystem::rename(temp,path,ec);if(ec)std::filesystem::remove(temp,ec);}
    }catch(...){std::error_code ec;std::filesystem::remove(temp,ec);}
}

void loadPersistentCache(){
    const auto path=std::filesystem::path(worldanalysis::config::ConfigManager::get().getDataPath("instore_cache.json"));
    try{
        if(!std::filesystem::exists(path))return;std::ifstream in(path,std::ios::binary);nlohmann::json root;in>>root;
        if(!root.is_object()||root.value("version",0)!=1||!root.contains("storages")||!root["storages"].is_array())return;
        std::vector<StorageCache> loaded;loaded.reserve(std::min<std::size_t>(root["storages"].size(),kMaxCachedStorages));
        for(const auto&value:root["storages"]){
            if(loaded.size()>=kMaxCachedStorages||!value.is_object())break;StorageCache entry{};
            entry.dimensionId=std::clamp(value.value("dimensionId",0),-16,16);entry.pos={value.value("x",0),value.value("y",0),value.value("z",0)};entry.slots=std::clamp(value.value("slots",0),0,kMaxContainerSlots);entry.updatedAt=value.value("updatedAt",std::uint64_t{0});
            if(std::abs(entry.pos.x)>30000000||std::abs(entry.pos.z)>30000000||entry.pos.y<-2048||entry.pos.y>8192)continue;
            if(value.contains("items")&&value["items"].is_array())for(const auto&raw:value["items"]){
                if(entry.items.size()>=kMaxContainerSlots||!raw.is_object())break;StorageItem item{};item.id=raw.value("id",0);item.damage=raw.value("damage",0);item.count=std::clamp(raw.value("count",0),0,1000000);item.color=raw.value("color",0xFFFFFFFFu);item.name=worldanalysis::worldoverlay::sanitizeText(raw.value("name",std::string{}),64);if(item.count>0&&!item.name.empty())entry.items.push_back(std::move(item));
            }
            loaded.push_back(std::move(entry));
        }
        std::lock_guard lock(g_mutex);g_cache=std::move(loaded);
    }catch(...){}
}

void updateCache(const BlockPosRaw&pos,int dimensionIdValue,const ContainerRead&read){
    if(!read.readable)return;std::vector<StorageItem>owned=read.items;
    for(auto&item:owned){item.stack=nullptr;item.itemKey=0;}
    std::lock_guard lock(g_mutex);auto*entry=findCacheLocked(pos,dimensionIdValue);bool changed=false;
    if(!entry){
        if(g_cache.size()>=kMaxCachedStorages){auto oldest=std::min_element(g_cache.begin(),g_cache.end(),[](const auto&a,const auto&b){return a.updatedAt<b.updatedAt;});if(oldest!=g_cache.end())g_cache.erase(oldest);}
        g_cache.push_back({});entry=&g_cache.back();entry->pos=pos;entry->dimensionId=dimensionIdValue;changed=true;
    }
    if(entry->slots!=read.slots||!sameItems(entry->items,owned)){entry->slots=read.slots;entry->items=std::move(owned);changed=true;}
    if(changed){entry->updatedAt=epochMillis();g_cacheDirty=true;}
}

void flushPersistentCache(bool force=false){
    bool shouldSave=false;const auto now=Clock::now();
    {
        std::lock_guard lock(g_mutex);
        const bool due=g_lastCacheSave.time_since_epoch().count()==0||std::chrono::duration<float>(now-g_lastCacheSave).count()>=.75f;
        if(g_cacheDirty&&(force||due)){g_cacheDirty=false;g_lastCacheSave=now;shouldSave=true;}
    }
    if(shouldSave)savePersistentCache();
}

bool cachedContents(const BlockPosRaw&pos,int dimensionIdValue,ContainerRead&out){
    std::lock_guard lock(g_mutex);auto*entry=findCacheLocked(pos,dimensionIdValue);if(!entry)return false;
    out.readable=true;out.slots=entry->slots;out.items=entry->items;return true;
}

void publish(StorageTarget&&next){
    const auto now=Clock::now();std::lock_guard lock(g_mutex);
    if(next.valid&&g_target.valid&&samePos(next.pos,g_target.pos)&&next.dimension==g_target.dimension){
        next.acquiredAt=g_target.acquiredAt;next.side=g_target.side;
    }else if(next.valid)next.acquiredAt=now;
    g_target=std::move(next);
}

void appendThick(std::vector<Segment>&out,const Vec3&a,const Vec3&b,const Vec3&camera){
    const Vec3 dir=normalize(sub(b,a),{0,1,0}),mid=mul(add(a,b),.5f);
    const Vec3 perpendicular=normalize(cross(dir,normalize(sub(camera,mid),{0,0,1})),{1,0,0});
    for(float n:{-2.f,-1.f,0.f,1.f,2.f}){const Vec3 offset=mul(perpendicular,kHalfThickness*n*.5f);out.push_back({add(a,offset),add(b,offset)});}
}
void appendRing(std::vector<Segment>&out,const Vec3&center,const Vec3&camera,float radius=kEndpointRadius){
    constexpr int sides=20;const Vec3 toCamera=normalize({camera.x-center.x,0,camera.z-center.z},{0,0,1});const Vec3 right=normalize({toCamera.z,0,-toCamera.x},{1,0,0});
    for(int i=0;i<sides;++i){const float a=2.f*kPi*i/sides,b=2.f*kPi*(i+1)/sides;const Vec3 p0=add(center,add(mul(right,std::cos(a)*radius),Vec3{0,std::sin(a)*radius,0}));const Vec3 p1=add(center,add(mul(right,std::cos(b)*radius),Vec3{0,std::sin(b)*radius,0}));appendThick(out,p0,p1,camera);}
}
void appendCube(std::vector<Segment>&out,const BlockPosRaw&p){
    constexpr float e=.0025f;const float x0=p.x-e,y0=p.y-e,z0=p.z-e,x1=p.x+1+e,y1=p.y+1+e,z1=p.z+1+e;
    const Vec3 a{x0,y0,z0},b{x1,y0,z0},c{x1,y0,z1},d{x0,y0,z1},e0{x0,y1,z0},f{x1,y1,z0},g{x1,y1,z1},h{x0,y1,z1};
    out.insert(out.end(),{{a,b},{b,c},{c,d},{d,a},{e0,f},{f,g},{g,h},{h,e0},{a,e0},{b,f},{c,g},{d,h}});
}
void appendPanelOutline(std::vector<Segment>&out,const Vec3&center,const Vec3&right,const Vec3&up,const Vec3&camera,float width,float height,float progress){
    width*=std::clamp(progress,0.f,1.f);height*=std::clamp(progress,0.f,1.f);if(width<.01f||height<.01f)return;
    const Vec3 a=add(add(center,mul(right,-width*.5f)),mul(up,-height*.5f));
    const Vec3 b=add(add(center,mul(right, width*.5f)),mul(up,-height*.5f));
    const Vec3 c=add(add(center,mul(right, width*.5f)),mul(up, height*.5f));
    const Vec3 d=add(add(center,mul(right,-width*.5f)),mul(up, height*.5f));
    appendThick(out,a,b,camera);appendThick(out,b,c,camera);appendThick(out,c,d,camera);appendThick(out,d,a,camera);
}
std::uint32_t alphaColor(std::uint32_t color,float alpha){const auto a=static_cast<std::uint32_t>(std::clamp(alpha,0.f,1.f)*static_cast<float>((color>>24)&255)+.5f);return(a<<24)|(color&0x00FFFFFFu);}

std::vector<DisplayRow> displayRows(const StorageTarget&target){
    std::vector<DisplayRow>rows;
    if(target.items.empty()){rows.push_back({target.synchronized?"EMPTY":"OPEN TO SYNC",0xFFFFFFFFu});return rows;}
    const auto count=std::min<std::size_t>(target.items.size(),kMaxTextRows);rows.reserve(count+1);
    constexpr std::size_t maxRowChars=24;
    for(std::size_t i=0;i<count;++i){
        const std::string suffix=target.items[i].count>1?" X"+std::to_string(target.items[i].count):std::string{};
        const std::size_t room=maxRowChars>suffix.size()?maxRowChars-suffix.size():4;
        std::string clean=worldanalysis::worldoverlay::sanitizeText(target.items[i].name,64);
        if(clean.size()>room){const std::size_t keep=room>3?room-3:room;clean.resize(keep);if(room>3)clean+="...";}
        std::string label=clean+suffix;if(label.size()>maxRowChars)label.resize(maxRowChars);
        rows.push_back({std::move(label),target.items[i].color});
    }
    if(target.items.size()>count)rows.push_back({"+"+std::to_string(target.items.size()-count)+" MORE",0xFFFFFFFFu});
    return rows;
}

void renderLevelHook(void*self,void*screen,void*a3){
    (void)a3;if(!g_mod||!g_mod->enabled)return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color);
    worldanalysis::worldoverlay::RenderContext context;if(!worldanalysis::worldoverlay::makeContext(self,screen,context))return;
    StorageTarget target;{std::lock_guard lock(g_mutex);target=g_target;}if(!target.valid)return;
    void*material=worldanalysis::worldoverlay::throughWallMaterial();if(!material)return;
    const Vec3 center{target.pos.x+.5f,target.pos.y+.5f,target.pos.z+.5f};Vec3 start=target.hit;if(!finite(start)||length(sub(start,center))>2.f)start=center;
    const Vec3 toCamera=normalize({context.camera.x-center.x,0,context.camera.z-center.z},{0,0,1});const Vec3 right=normalize({toCamera.z,0,-toCamera.x},{1,0,0});
    // Determine and Instore intentionally share the exact same o-o origin and
    // branch geometry. Collision avoidance is lateral only: when Determine owns
    // one side (including its chart), Instore takes the opposite side.
    const float horizontal=kBranchLength,vertical=kBranchLength;
    const Vec3 fullEnd=add(start,add(mul(right,static_cast<float>(target.side)*horizontal),Vec3{0,vertical,0}));
    const float elapsed=std::max(0.f,std::chrono::duration<float>(Clock::now()-target.acquiredAt).count()),speed=std::clamp(g_mod->animationSpeed,.5f,1.5f);
    const bool animate=g_mod->animation;
    const float lineSeconds=kLineAnimationSeconds/speed,fadeSeconds=kTextFadeSeconds/speed;
    const float progress=animate?smoothStep(elapsed/lineSeconds):1.f,textAlpha=animate?smoothStep((elapsed-lineSeconds)/fadeSeconds):1.f;
    const Vec3 end=add(start,mul(sub(fullEnd,start),progress));
    std::vector<Segment>outline;appendCube(outline,target.pos);worldanalysis::worldoverlay::drawLines(context,worldanalysis::worldoverlay::material("selection_box"),outline,0xFFFFFFFFu);
    std::vector<Segment>callout;appendThick(callout,start,end,context.camera);appendRing(callout,start,context.camera);if(progress>.01f)appendRing(callout,end,context.camera,kEndpointRadius*std::min(1.f,.3f+progress));worldanalysis::worldoverlay::drawLines(context,material,callout,0xFFFFFFFFu);

    const auto rows=displayRows(target);
    std::size_t longest=1;for(const auto&row:rows)longest=std::max(longest,row.text.size());
    const float textPixel=std::clamp((2.50f-.28f)/(std::max(1.f,static_cast<float>(longest)*6.f-1.f)),.0095f,.0185f);
    const float textHeight=worldanalysis::worldoverlay::billboardTextHeight(textPixel),rowStep=textHeight+.065f;
    float panelWidth=.86f,panelHeight=std::max(.32f,static_cast<float>(rows.size())*rowStep+.16f);
    for(const auto&row:rows)panelWidth=std::max(panelWidth,worldanalysis::worldoverlay::billboardTextWidth(row.text,textPixel)+.28f);
    panelWidth=std::clamp(panelWidth,.86f,2.50f);panelHeight=std::clamp(panelHeight,.32f,2.25f);
    const float panelGap=target.avoidDetermine?.36f:.26f;const Vec3 panelCenter=add(fullEnd,add(mul(right,static_cast<float>(target.side)*(panelWidth*.5f+panelGap)),Vec3{0,.10f,0}));
    Vec3 panelRight{},panelUp{};worldanalysis::worldoverlay::billboardBasis(panelCenter,context.camera,panelRight,panelUp);
    const float panelProgress=animate?smoothStep((elapsed-lineSeconds)/(kPanelAnimationSeconds/speed)):1.f;
    const float contentAlpha=animate?smoothStep((elapsed-lineSeconds-kPanelAnimationSeconds*.35f/speed)/(fadeSeconds+.08f/speed)):1.f;
    if(g_mod->instoreBoxOutline&&panelProgress>0.f){std::vector<Segment>panel;appendPanelOutline(panel,panelCenter,panelRight,panelUp,context.camera,panelWidth,panelHeight,panelProgress);worldanalysis::worldoverlay::drawLines(context,material,panel,alphaColor(0xFFFFFFFFu,textAlpha));}
    if(contentAlpha>0.f){
        const float firstY=panelHeight*.5f-.08f-textHeight*.5f;
        const float innerWidth=std::max(.10f,panelWidth-.18f);
        for(std::size_t i=0;i<rows.size();++i){
            const Vec3 position=add(panelCenter,mul(panelUp,firstY-static_cast<float>(i)*rowStep));
            const float fitted=worldanalysis::worldoverlay::fitBillboardTextPixelSize(rows[i].text,innerWidth,textPixel,.0075f);
            worldanalysis::worldoverlay::drawBillboardTextOriented(context,material,rows[i].text,position,panelRight,panelUp,fitted,alphaColor(rows[i].color,textAlpha*contentAlpha),false);
        }
    }
}

void screenStateChanged(worldanalysis::events::ScreenStateEvent&event){
    using namespace worldanalysis::events;if(event.screen!=ScreenKind::Container)return;
    std::lock_guard lock(g_mutex);
    if(event.phase==ScreenPhase::Opened){
        if(!g_mod||!g_mod->enabled||!g_target.valid||!event.controller)return;
        g_sync={};
        g_sync.screenOpen=true;g_sync.pending=true;g_sync.controller=event.controller;
        g_sync.pos=g_target.pos;g_sync.dimension=g_target.dimension;g_sync.dimensionId=g_target.dimensionId;g_sync.openedAt=Clock::now();
        if(g_target.synchronized){
            g_sync.stable.readable=true;g_sync.stable.slots=g_target.slots;g_sync.stable.items=g_target.items;
        }
    }else{
        // ContainerScreenController destructors are global. Ignore stale/other
        // controller destruction so an unrelated controller cannot terminate
        // the active chest session and roll the viewer back to an old snapshot.
        if(!g_sync.pending||!g_sync.controller||event.controller!=g_sync.controller)return;
        g_sync.screenOpen=false;g_sync.closeRequested=true;
    }
}

} // namespace

InstoreViewerModule::InstoreViewerModule():Module("Instore Viewer","Targets real storage BlockActors. While a container screen is open it snapshots the synchronized ContainerScreenController model, preserves the newest contents after close, and displays fitted rarity-colored item names. Synced snapshots are persisted on disk across launcher restarts."){showInMenu=true;g_mod=this;}
InstoreViewerModule::~InstoreViewerModule(){if(g_mod==this)g_mod=nullptr;}
void InstoreViewerModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::LevelGetHitResult))g_getHitResult=reinterpret_cast<LevelGetHitResultFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ContainerScreenControllerGetItemStack))g_containerScreenGetItemStack=reinterpret_cast<ContainerScreenGetItemStackFn>(a);
    worldanalysis::worldoverlay::initialize();worldanalysis::worldoverlay::registerRenderCallback(renderLevelHook);worldanalysis::items::initialize();loadPersistentCache();
    worldanalysis::events::bus().subscribe<worldanalysis::events::ScreenStateEvent>(screenStateChanged);
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});
}
void InstoreViewerModule::onEnable(){std::lock_guard lock(g_mutex);g_target={};g_sync={};}
void InstoreViewerModule::onDisable(){flushPersistentCache(true);std::lock_guard lock(g_mutex);g_target={};g_sync={};}

void InstoreViewerModule::handleTick(worldanalysis::sdk::Player*player){
    StorageTarget next{};if(!enabled||!player||!g_getHitResult||!g_getBlock){publish(std::move(next));return;}
    auto*dimension=player->dimension();void*region=dimension?dimension->blockSource():nullptr;if(!region){publish(std::move(next));return;}const int dimId=dimensionCacheKey(dimension);

    OpenSync sync;StorageTarget held;{std::lock_guard lock(g_mutex);sync=g_sync;held=g_target;}
    if(sync.pending&&sync.dimension==dimension){
        // While the actual container screen is alive, its controller model is
        // authoritative. Polling the BlockActor in parallel used to let a
        // slower world-side snapshot delay removals and occasionally roll a
        // fresh GUI state backward. Only fall back to world data when the GUI
        // controller cannot be read or after the screen starts closing.
        ContainerRead guiRead{};
        // Never dereference the screen controller after its close event. The
        // controller owns GUI ItemStacks and can already be in destruction by
        // the time the next LocalPlayerTick arrives.
        if(sync.screenOpen&&!sync.closeRequested&&sync.controller)
            { void* currentBlock=g_getBlock(region,sync.pos); guiRead=readOpenScreen(sync.controller,held.slots,blockIdentifier(currentBlock)); }

        ContainerRead worldRead{};
        if(!sync.closeRequested&&!guiRead.readable)
            worldRead=readStorage(region,sync.pos,true);

        ContainerRead stable=sync.stable;
        int emptyStreak=sync.emptyReadStreak;
        const float openAge=std::chrono::duration<float>(Clock::now()-sync.openedAt).count();

        if(sync.closeRequested){
            // Closing is a commit boundary, not another synchronization source.
            // Keep the last authoritative GUI snapshot exactly as-is. A
            // teardown-frame BlockActor read can legitimately be empty/stale
            // and must never erase chest contents that were visible one tick
            // earlier. Live ItemStack pointers are removed from the presented
            // post-close copy below.
            emptyStreak=0;
        }else if(guiRead.readable){
            if(!guiRead.items.empty()){
                // Adds, count changes and partial removals appear immediately.
                stable=guiRead;emptyStreak=0;
            }else if(stable.items.empty()){
                stable=guiRead;emptyStreak=0;
            }else{
                // Only a confirmed empty GUI while the controller is still
                // alive may erase a populated snapshot. This preserves a real
                // final-item removal while rejecting transient opening/closing
                // empty frames.
                if(openAge>=.18f)++emptyStreak;
                constexpr int kConfirmedEmptyTicks=2;
                if(emptyStreak>=kConfirmedEmptyTicks){stable=guiRead;emptyStreak=0;}
            }
        }else if(worldRead.readable&&!worldRead.items.empty()){
            // World-side data may fill an unreadable initial GUI, but an empty
            // fallback never overwrites a populated session.
            stable=worldRead;emptyStreak=0;
        }

        if(stable.readable)updateCache(sync.pos,sync.dimensionId,stable);
        flushPersistentCache(sync.closeRequested);

        {
            std::lock_guard lock(g_mutex);
            if(g_sync.pending&&samePos(g_sync.pos,sync.pos)&&g_sync.dimension==sync.dimension
                &&g_sync.controller==sync.controller){
                g_sync.stable=stable;g_sync.emptyReadStreak=emptyStreak;
                if(sync.closeRequested){
                    g_sync.pending=false;g_sync.closeRequested=false;g_sync.screenOpen=false;g_sync.controller=nullptr;
                }
            }
        }

        if(held.valid&&samePos(held.pos,sync.pos)&&held.dimension==sync.dimension){
            next=held;ContainerRead display=stable;if(!display.readable)cachedContents(sync.pos,sync.dimensionId,display);
            if(display.readable){
                next.items=display.items;next.slots=display.slots;next.synchronized=true;
                if(sync.closeRequested)for(auto&item:next.items){item.stack=nullptr;item.itemKey=0;}
            }
            publish(std::move(next));return;
        }
    }
    // Keep the selected storage and its callout stable while its screen owns
    // input.  No controller pointer or UI ItemStack is retained.
    if(sync.screenOpen&&held.valid){publish(std::move(held));return;}

    auto*level=player->level();auto*hit=level?g_getHitResult(level):nullptr;if(!hit||hit->type()!=0){publish(std::move(next));return;}
    const auto pos=*reinterpret_cast<const BlockPosRaw*>(reinterpret_cast<const std::byte*>(hit)+worldanalysis::sdk::offsets::HitResult::mBlock);
    // Once a storage has been opened, use that one captured snapshot until the
    // next open. Normal targeting probes only type/size, so another player's
    // edits do not trigger continuous controller or network synchronization.
    ContainerRead probe=readStorage(region,pos,false);if(!probe.readable){publish(std::move(next));return;}
    next.valid=true;next.pos=pos;next.dimension=dimension;next.dimensionId=dimId;next.slots=probe.slots;next.hit=hit->position();
    next.side=(((static_cast<std::uint32_t>(pos.x)*73856093u)^(static_cast<std::uint32_t>(pos.y)*19349663u)^(static_cast<std::uint32_t>(pos.z)*83492791u))&1u)?1:-1;
    ContainerRead cached{};
    if(cachedContents(pos,dimId,cached)){
        next.items=std::move(cached.items);next.slots=cached.slots;next.synchronized=true;

    }else{
        // Before the first open, a server-prepopulated BlockActor may already be
        // synchronized. Show that live state, but do not make it the persistent
        // snapshot until the real container-open event arrives.
        ContainerRead live=readStorage(region,pos,true);if(live.readable){next.items=std::move(live.items);next.slots=live.slots;next.synchronized=!next.items.empty();}
    }
    int determineSide=1;if(activeDetermineCalloutSide(determineSide)){next.avoidDetermine=true;next.side=-determineSide;}
    publish(std::move(next));
}

void InstoreViewerModule::loadConfig(const nlohmann::json&j){Module::loadConfig(j);animation=j.value("animation",animation);animationSpeed=std::clamp(j.value("animationSpeed",animationSpeed),.5f,1.5f);instoreBoxOutline=j.value("instoreBoxOutline",instoreBoxOutline);}
void InstoreViewerModule::saveConfig(nlohmann::json&j){Module::saveConfig(j);j["animation"]=animation;j["animationSpeed"]=std::clamp(animationSpeed,.5f,1.5f);j["instoreBoxOutline"]=instoreBoxOutline;}
