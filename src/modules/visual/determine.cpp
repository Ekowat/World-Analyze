#include "determine.hpp"

#include "core/memory/Hooks.hpp"
#include "worldoverlay.hpp"
#include <worldanalysis/events/EventBus.hpp>
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <worldanalysis/sdk/Types.hpp>
#include <worldanalysis/sdk/world/Actor.hpp>
#include <worldanalysis/sdk/world/Dimension.hpp>
#include <worldanalysis/sdk/world/HitResult.hpp>
#include <worldanalysis/sdk/world/Level.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using Vec3=worldanalysis::sdk::Vec3;
using AABB=worldanalysis::sdk::AABB;
using Clock=std::chrono::steady_clock;

enum class TargetKind : std::uint8_t { None, Block, Entity };
struct BlockPosRaw { int x=0,y=0,z=0; };
struct Segment { Vec3 a{},b{}; };
struct Quad { Vec3 a{},b{},c{},d{}; };
struct DropSlice { std::string item; float percent=0.f; };
struct Target {
    bool valid=false;
    TargetKind kind=TargetKind::None;
    BlockPosRaw pos{};
    AABB bounds{};
    Vec3 hit{};
    std::uintptr_t identity=0;
    std::int32_t id=0;
    int healthPercent=0;
    bool hasHealth=false;
    float chartPercent=0.f;
    std::string chartMaximum;
    std::string chartCurrent;
    float blastPercent=0.f;
    std::string blastMaximum;
    std::string blastCurrent;
    std::string bestTool;
    std::string speciesKey;
    bool dropProfileKnown=false;
    bool isBaby=false;
    bool isPlayer=false;
    std::vector<DropSlice> drops;
    int side=1;
    std::string name;
    std::string identifier;
    Clock::time_point acquiredAt{};
};
struct BoneCapture {
    std::uintptr_t identity=0;
    std::vector<Segment> segments;
    Clock::time_point capturedAt{};
};
struct HashedString {
    std::uint64_t hash=0;
    std::string value;
    mutable const HashedString* last=nullptr;
    explicit HashedString(const char* text):value(text?text:"") {
        if(value.empty())return;
        constexpr std::uint64_t off=0xCBF29CE484222325ULL,prime=0x100000001B3ULL;
        std::uint64_t h=off;
        for(unsigned char c:value)h=static_cast<std::uint64_t>(c)^(prime*h);
        hash=h;
    }
};
using MaterialPtr = std::shared_ptr<void>;
struct EmptySharedPtr { void* object=nullptr; void* control=nullptr; };

using GetBlockFn=void*(*)(void*,const BlockPosRaw&);
using SolidFn=bool(*)(void*,const BlockPosRaw*);
using LevelGetHitResultFn=worldanalysis::sdk::HitResult*(*)(void*);
using HitResultGetEntityFn=void*(*)(void*);
using ActorIsPlayerFn=bool(*)(void*);
using ActorHealthFn=int(*)(void*);
using ActorGetNameTagFn=std::string(*)(void*);
using I18nGetInstanceFn=void*(*)();
using I18nLookupFn=std::string(*)(void*,const std::string&,const EmptySharedPtr&);
using TessBeginFn=void(*)(void*,void*,int,int,int);
using TessColorFn=void(*)(void*,float,float,float,float);
using TessVertexFn=void(*)(void*,float,float,float);
using RenderMeshFn=void(*)(void*,void*,void*,char*);
using DataDrivenModelRenderFn=void(*)(void*,void*,void*,const Vec3*,const Vec3*);

constexpr float kPi=3.14159265358979323846f;
constexpr float kBranchLength=1.20f;
constexpr float kEndpointRadius=.075f;
constexpr float kHalfThickness=.0075f;
constexpr float kTextGlowThickness=.0060f;
constexpr float kLineAnimationSeconds=.50f;
constexpr float kTextFadeSeconds=.10f;
constexpr float kChartDelaySeconds=.30f;
constexpr float kChartCircleSeconds=.45f;
constexpr float kChartValueSeconds=.40f;
constexpr float kChartRadius=.13f;
constexpr float kMaximumFiniteToughness=55.f; // reinforced deepslate; bedrock is unbreakable
constexpr float kBlastReference=1200.f; // obsidian/reinforced-deepslate class; log-scaled for useful contrast
constexpr std::size_t kI18nLookupVtableIndex=0x90/sizeof(void*);
constexpr std::size_t kActorRenderDataPosition=0x10;
constexpr std::size_t kActorRenderDataRotation=0x1C;
constexpr std::size_t kActorRenderDataAnimation=0x38;
constexpr std::size_t kAnimationBoneSpan=0x328;
constexpr std::size_t kBoneStride=0xE0;
constexpr std::size_t kBoneMatrix=0x30;
constexpr std::size_t kMaxBones=512;

DetermineModule* g_mod=nullptr;
GetBlockFn g_getBlock=nullptr;
SolidFn g_isSolid=nullptr;
LevelGetHitResultFn g_getHitResult=nullptr;
HitResultGetEntityFn g_getHitEntity=nullptr;
ActorIsPlayerFn g_actorIsPlayer=nullptr;
ActorHealthFn g_getHealth=nullptr;
ActorHealthFn g_getMaxHealth=nullptr;
ActorGetNameTagFn g_getNameTag=nullptr;
I18nGetInstanceFn g_getI18n=nullptr;
TessBeginFn g_begin=nullptr;
TessColorFn g_color=nullptr;
TessVertexFn g_vertex=nullptr;
RenderMeshFn g_render=nullptr;
void(*g_renderOriginal)(void*,void*,void*)=nullptr;
DataDrivenModelRenderFn g_modelRenderOriginal=nullptr;
std::ptrdiff_t g_destroySpeedOffset=-1;
std::uintptr_t g_materialGroup=0;
MaterialPtr* g_throughMaterial=nullptr;
MaterialPtr* g_depthMaterial=nullptr;
Target g_target{};
BoneCapture g_bones{};
std::mutex g_mutex;

bool finite(const Vec3&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<3e7f&&std::abs(p.z)<3e7f&&p.y>-1024.f&&p.y<4096.f;}
Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(const Vec3&a,const Vec3&b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}
float len(const Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
Vec3 norm(const Vec3&v,const Vec3&fallback={1,0,0}){float l=len(v);return l>.0001f?mul(v,1.f/l):fallback;}
Vec3 cross(const Vec3&a,const Vec3&b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool samePos(const BlockPosRaw&a,const BlockPosRaw&b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
BlockPosRaw floorPos(const Vec3&p){return{static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.y)),static_cast<int>(std::floor(p.z))};}
float smoothStep(float x){x=std::clamp(x,0.f,1.f);return x*x*(3.f-2.f*x);}
std::uint32_t whiteWithAlpha(float alpha){return(static_cast<std::uint32_t>(std::clamp(alpha,0.f,1.f)*255.f+.5f)<<24)|0x00FFFFFFu;}
std::uint32_t colorWithAlpha(std::uint32_t rgb,float alpha){return(static_cast<std::uint32_t>(std::clamp(alpha,0.f,1.f)*255.f+.5f)<<24)|(rgb&0x00FFFFFFu);}
bool validBounds(const AABB&b){return finite(b.min)&&finite(b.max)&&b.max.x>b.min.x&&b.max.y>b.min.y&&b.max.z>b.min.z&&b.max.x-b.min.x<64.f&&b.max.y-b.min.y<64.f&&b.max.z-b.min.z<64.f;}
Vec3 boundsCenter(const AABB&b){return mul(add(b.min,b.max),.5f);}

bool sameTarget(const Target&a,const Target&b){
    if(!a.valid||!b.valid||a.kind!=b.kind)return false;
    if(a.kind==TargetKind::Block)return samePos(a.pos,b.pos);
    if(a.kind==TargetKind::Entity)return a.identity!=0&&a.identity==b.identity;
    return false;
}

std::uintptr_t resolveADRP(std::uint32_t*insns,std::size_t count,std::uint32_t reg){
    for(std::size_t i=0;i<count;++i){
        const auto v=insns[i];if((v&0x1F)!=reg)continue;
        if((v&0x9F000000)==0x90000000){
            std::uintptr_t page=(reinterpret_cast<std::uintptr_t>(&insns[i])&~0xFFFULL)+((static_cast<std::int64_t>(static_cast<std::uint64_t>(((v>>3)&0x1FFFFC)|((v>>29)&3))<<43))>>31);
            for(std::size_t j=i+1;j<count;++j){
                const auto a=insns[j];
                if((a&0xFF000000)==0x91000000&&((a>>5)&0x1F)==reg&&(a&0x1F)==reg){std::uint32_t imm=(a>>10)&0xFFF;if(a&0x400000)imm<<=12;return page+imm;}
                if((a&0x1F)==reg)break;
            }
        }
    }
    return 0;
}
MaterialPtr* getMaterial(const char*name){
    if(!g_materialGroup)return nullptr;
    HashedString h(name);auto**vt=*reinterpret_cast<void***>(g_materialGroup);
    const auto slot=worldanalysis::sdk::offsets::VTable::RenderMaterialGroup_getMaterial;
    if(!vt||!vt[slot])return nullptr;
    using Fn=MaterialPtr(*)(void*,const HashedString*);
    MaterialPtr material=reinterpret_cast<Fn>(vt[slot])(reinterpret_cast<void*>(g_materialGroup),&h);return material?new MaterialPtr(material):nullptr;
}
void ensureMaterial(){
    if(!g_throughMaterial)g_throughMaterial=getMaterial("name_tag");
    if(!g_throughMaterial)g_throughMaterial=getMaterial("name_tag_with_backface");
    if(!g_depthMaterial)g_depthMaterial=getMaterial("name_tag_depth_tested");
    if(!g_depthMaterial)g_depthMaterial=getMaterial("name_tag_depth_tested_with_backface");
}

std::string_view blockIdentifier(const void*block){
    if(!block)return{};
    const auto bt=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!bt)return{};
    const auto addr=bt+worldanalysis::sdk::offsets::BlockType::mNameInfo+worldanalysis::sdk::offsets::NameInfo::mFullName+worldanalysis::sdk::offsets::HashedString::mString;
    const auto*s=reinterpret_cast<const std::string*>(addr);
    if(s->size()>256||(!s->empty()&&!s->data()))return{};
    return{s->data(),s->size()};
}
std::int32_t blockId(const void*block,int mode){
    if(!block)return 0;
    mode=std::clamp(mode,0,2);
    if(mode==0)return 0;
    if(mode==2)return static_cast<std::int32_t>(*reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mNetworkId));
    const auto bt=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
    if(!bt)return 0;
    return static_cast<std::int32_t>(*reinterpret_cast<const std::int16_t*>(bt+worldanalysis::sdk::offsets::BlockType::mId));
}
std::string i18nLookup(const std::string&key){
    if(!g_getI18n||key.empty())return{};
    void*instance=g_getI18n();if(!instance)return{};
    auto**vt=*reinterpret_cast<void***>(instance);if(!vt||!vt[kI18nLookupVtableIndex])return{};
    EmptySharedPtr noLocalization{};
    auto value=reinterpret_cast<I18nLookupFn>(vt[kI18nLookupVtableIndex])(instance,key,noLocalization);
    if(value.empty()||value==key)return{};
    return value;
}
std::string localizedBlockName(std::string_view identifier){
    if(identifier.empty())return{};
    std::string id(identifier);
    std::string shortId=id;
    if(shortId.starts_with("minecraft:"))shortId.erase(0,10);
    std::string localized=i18nLookup("tile."+shortId+".name");
    if(localized.empty()&&shortId!=id)localized=i18nLookup("tile."+id+".name");
    return localized.empty()?id:localized;
}
std::string displayName(std::string_view name,std::string_view fallback="BLOCK"){
    if(name.starts_with("minecraft:"))name.remove_prefix(10);
    std::string out;out.reserve(std::min<std::size_t>(name.size(),30));
    for(char c:name){
        if(out.size()>=30)break;
        if(c=='_'||c=='-'){if(!out.empty()&&out.back()!=' ')out.push_back(' ');continue;}
        if(c>='a'&&c<='z')out.push_back(static_cast<char>(c-'a'+'A'));
        else if((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==' ')out.push_back(c);
    }
    while(!out.empty()&&out.back()==' ')out.pop_back();
    return out.empty()?std::string(fallback):out;
}
std::string displayIdentifier(std::string_view id){
    std::string out;out.reserve(std::min<std::size_t>(id.size(),30));
    for(char c:id){
        if(out.size()>=30)break;
        if(c>='a'&&c<='z')out.push_back(static_cast<char>(c-'a'+'A'));
        else if((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==':'||c=='_')out.push_back(c);
    }
    return out.empty()?std::string("BLOCK"):out;
}
bool plausiblePtr(std::uintptr_t p,std::size_t alignment=alignof(void*)){
    if(p<0x10000)return false;
    if(alignment>1&&(p&(alignment-1)))return false;
#if UINTPTR_MAX > 0xFFFFFFFFu
    if(p>=0x0100000000000000ULL)return false;
#endif
    return true;
}

std::uintptr_t branchTarget(const std::uint32_t*instruction){
    const std::uint32_t value=*instruction;
    std::int64_t immediate=static_cast<std::int64_t>(value&0x03FFFFFFu);
    if(immediate&0x02000000LL)immediate|=~0x03FFFFFFLL;
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(
        reinterpret_cast<std::uintptr_t>(instruction))+immediate*4);
}
bool sameImage(std::uintptr_t a,std::uintptr_t b){
    Dl_info left{},right{};return dladdr(reinterpret_cast<void*>(a),&left)!=0
        &&dladdr(reinterpret_cast<void*>(b),&right)!=0&&left.dli_fbase==right.dli_fbase;
}
std::ptrdiff_t directFloatField(const std::uint32_t*code,unsigned baseRegister,std::size_t limit=12){
    for(std::size_t i=0;i<limit;++i){
        const std::uint32_t insn=code[i];
        if((insn&0xFFC00000u)==0xBD400000u&&((insn>>5)&31u)==baseRegister&&(insn&31u)==0u)
            return static_cast<std::ptrdiff_t>(((insn>>10)&0xFFFu)*4u);
        if(insn==0xD65F03C0u)break;
    }
    return -1;
}
std::ptrdiff_t blockDestroyFieldInFunction(std::uintptr_t address){
    if(!plausiblePtr(address,4))return -1;
    const auto*code=reinterpret_cast<const std::uint32_t*>(address);
    for(std::size_t i=0;i<40;++i){
        const std::uint32_t load=code[i];
        // ldr Xd,[X0,#0x68] -- Block::mBlockType in the supplied 26.45 image.
        if((load&0xFFC003E0u)!=0xF9400000u||(((load>>10)&0xFFFu)*8u)!=0x68u)continue;
        const unsigned typeRegister=load&31u;
        if(const auto direct=directFloatField(code+i+1,typeRegister,10);direct>=0)return direct;
        // Tiny wrappers commonly tail-call BlockLegacy/BlockType's raw float
        // accessor after loading mBlockType. Decode that accessor too.
        for(std::size_t j=i+1;j<std::min<std::size_t>(i+10,40);++j){
            if((code[j]&0xFC000000u)!=0x14000000u)continue;
            const auto target=branchTarget(code+j);if(!sameImage(address,target))continue;
            if(const auto field=directFloatField(reinterpret_cast<const std::uint32_t*>(target),0,12);field>=0)return field;
        }
    }
    return -1;
}
std::ptrdiff_t findDestroySpeedOffset(std::uintptr_t root){
    if(!root)return -1;
    std::vector<std::pair<std::uintptr_t,int>>pending{{root,0}};
    std::unordered_set<std::uintptr_t>seen;
    while(!pending.empty()&&seen.size()<384){
        const auto[address,depth]=pending.back();pending.pop_back();
        if(!seen.insert(address).second||!sameImage(root,address))continue;
        if(const auto field=blockDestroyFieldInFunction(address);field>=0&&field<0x300)return field;
        if(depth>=2)continue;
        const auto*code=reinterpret_cast<const std::uint32_t*>(address);
        const std::size_t limit=depth==0?512:128;
        for(std::size_t i=0;i<limit;++i){
            const auto insn=code[i];
            if((insn&0xFC000000u)==0x94000000u){
                const auto target=branchTarget(code+i);if(sameImage(root,target))pending.emplace_back(target,depth+1);
            }
            if(i>8&&insn==0xD65F03C0u)break;
        }
    }
    return -1;
}

float knownToughness(std::string_view identifier){
    if(identifier.starts_with("minecraft:"))identifier.remove_prefix(10);
    if(identifier=="bedrock"||identifier=="barrier"||identifier=="end_portal_frame"||identifier=="command_block"||identifier=="repeating_command_block"||identifier=="chain_command_block"||identifier=="structure_block"||identifier=="jigsaw")return -1.f;
    if(identifier=="reinforced_deepslate")return 55.f;
    if(identifier=="obsidian"||identifier=="crying_obsidian"||identifier=="respawn_anchor")return 50.f;
    if(identifier=="ancient_debris"||identifier=="netherite_block")return 30.f;
    if(identifier=="ender_chest")return 22.5f;
    if(identifier=="enchanting_table")return 5.f;
    if(identifier.find("deepslate")!=std::string_view::npos)return 3.f;
    if(identifier.find("ore")!=std::string_view::npos||identifier.find("stone")!=std::string_view::npos||identifier.find("brick")!=std::string_view::npos)return 1.5f;
    if(identifier.find("log")!=std::string_view::npos||identifier.find("wood")!=std::string_view::npos||identifier.find("planks")!=std::string_view::npos)return 2.f;
    if(identifier.find("dirt")!=std::string_view::npos||identifier.find("sand")!=std::string_view::npos||identifier.find("gravel")!=std::string_view::npos)return .5f;
    if(identifier.find("glass")!=std::string_view::npos)return .3f;
    if(identifier.find("leaves")!=std::string_view::npos)return .2f;
    return 1.f;
}
float calibrationToughness(std::string_view identifier){
    if(identifier.starts_with("minecraft:"))identifier.remove_prefix(10);
    if(identifier=="bedrock"||identifier=="barrier"||identifier=="end_portal_frame"||identifier=="command_block"||identifier=="repeating_command_block"||identifier=="chain_command_block"||identifier=="structure_block"||identifier=="jigsaw")return -1.f;
    if(identifier=="reinforced_deepslate")return 55.f;
    if(identifier=="obsidian"||identifier=="crying_obsidian"||identifier=="respawn_anchor")return 50.f;
    if(identifier=="ancient_debris"||identifier=="netherite_block")return 30.f;
    if(identifier=="ender_chest")return 22.5f;
    if(identifier=="enchanting_table")return 5.f;
    if(identifier=="stone")return 1.5f;
    if(identifier=="dirt")return .5f;
    return -2.f;
}
float blockToughness(const void*block,std::string_view identifier){
    if(block){
        const auto type=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(block)+worldanalysis::sdk::offsets::Block::mBlockType);
        if(plausiblePtr(type)){
            const float calibration=calibrationToughness(identifier);
            if(calibration>-1.5f){
                bool currentMatches=false;
                if(g_destroySpeedOffset>=0){
                    const float current=*reinterpret_cast<const float*>(type+static_cast<std::uintptr_t>(g_destroySpeedOffset));
                    currentMatches=std::isfinite(current)&&std::abs(current-calibration)<.001f;
                }
                if(!currentMatches){
                    std::ptrdiff_t match=-1;
                    for(std::ptrdiff_t offset=0xF0;offset<=0x14C;offset+=4){
                        const float value=*reinterpret_cast<const float*>(type+static_cast<std::uintptr_t>(offset));
                        if(!std::isfinite(value)||std::abs(value-calibration)>=.001f)continue;
                        if(match>=0){match=-1;break;}match=offset;
                    }
                    if(match>=0)g_destroySpeedOffset=match;
                }
                return calibration;
            }
            if(g_destroySpeedOffset>=0){
            const float value=*reinterpret_cast<const float*>(type+static_cast<std::uintptr_t>(g_destroySpeedOffset));
            if(std::isfinite(value)&&value>=-1.f&&value<1000000.f)return value;
            }
        }
    }
    return knownToughness(identifier);
}
std::string oneDecimal(float value){char text[32]{};std::snprintf(text,sizeof(text),"%.1f",static_cast<double>(value));return text;}
float blastResistanceProfile(std::string_view identifier,float toughness){
    std::string_view id=identifier;if(id.starts_with("minecraft:"))id.remove_prefix(10);
    auto has=[&](std::string_view v){return id.find(v)!=std::string_view::npos;};
    if(has("bedrock")||has("barrier")||has("end_gateway")||has("end_portal")||has("command_block")||has("structure_block")||has("jigsaw"))return 3600000.f;
    if(has("obsidian")||has("reinforced_deepslate")||has("ancient_debris")||has("respawn_anchor"))return 1200.f;
    if(has("ender_chest"))return 600.f;
    if(has("anvil"))return 120.f;
    if(has("water")||has("lava"))return 100.f;
    if(has("deepslate")||has("stone")||has("brick")||has("ore")||has("concrete")||has("terracotta")||has("copper")||has("iron")||has("gold")||has("diamond")||has("emerald")||has("netherite"))return std::max(6.f,std::max(0.f,toughness)*2.f);
    if(has("log")||has("wood")||has("planks")||has("chest")||has("barrel")||has("bookshelf")||has("crafting_table"))return 3.f;
    if(has("glass")||has("pane"))return .3f;
    if(has("wool")||has("carpet"))return .8f;
    if(has("sand")||has("gravel")||has("dirt")||has("clay")||has("snow")||has("mud"))return .5f;
    if(toughness<0.f)return 3600000.f;
    return std::clamp(std::max(.1f,toughness)*1.7f,.1f,80.f);
}
std::string bestToolForBlock(std::string_view identifier){
    std::string_view id=identifier;if(id.starts_with("minecraft:"))id.remove_prefix(10);
    auto has=[&](std::string_view v){return id.find(v)!=std::string_view::npos;};
    if(has("cobweb"))return "SWORD";
    if(has("wool")||has("carpet")||has("leaves")||has("vine")||has("moss")||has("tripwire"))return "SHEARS";
    if(has("hay")||has("wart_block")||has("sponge")||has("sculk")||has("leaves"))return "HOE";
    if(has("dirt")||has("grass_block")||has("sand")||has("gravel")||has("clay")||has("mud")||has("snow")||has("soul_sand")||has("soul_soil")||has("concrete_powder")||has("path"))return "SHOVEL";
    if(has("log")||has("wood")||has("plank")||has("stem")||has("hyphae")||has("chest")||has("barrel")||has("bookshelf")||has("crafting_table")||has("pumpkin")||has("melon")||has("fence")||has("door")||has("trapdoor"))return "AXE";
    if(has("stone")||has("deepslate")||has("ore")||has("brick")||has("cobblestone")||has("obsidian")||has("netherrack")||has("basalt")||has("blackstone")||has("terracotta")||has("concrete")||has("anvil")||has("furnace")||has("hopper")||has("rail")||has("copper")||has("iron")||has("gold")||has("diamond")||has("emerald")||has("netherite")||has("quartz")||has("prismarine")||has("end_stone"))return "PICKAXE";
    return "HAND";
}
std::string actorRttiName(void*actor){
    if(!actor)return{};
    const auto object=reinterpret_cast<std::uintptr_t>(actor);
    const auto vtable=*reinterpret_cast<const std::uintptr_t*>(object);if(!plausiblePtr(vtable))return{};
    const auto typeInfo=*reinterpret_cast<const std::uintptr_t*>(vtable-sizeof(void*));if(!plausiblePtr(typeInfo))return{};
    const auto namePtr=*reinterpret_cast<const std::uintptr_t*>(typeInfo+sizeof(void*));if(!plausiblePtr(namePtr,1))return{};
    const char*text=reinterpret_cast<const char*>(namePtr);std::string raw;
    for(std::size_t i=0;i<96&&text[i];++i){const unsigned char c=static_cast<unsigned char>(text[i]);if(c<0x20||c>0x7E)return{};raw.push_back(static_cast<char>(c));}
    if(raw.empty()||raw.size()>=96)return{};
    std::size_t begin=0;while(begin<raw.size()&&raw[begin]>='0'&&raw[begin]<='9')++begin;
    if(begin>0&&begin<raw.size())raw.erase(0,begin);
    const auto ns=raw.rfind("::");if(ns!=std::string::npos)raw.erase(0,ns+2);
    return raw;
}
std::string snakeCase(std::string_view name){
    std::string out;out.reserve(name.size()+4);
    for(std::size_t i=0;i<name.size();++i){const char c=name[i];if(c>='A'&&c<='Z'){if(!out.empty()&&out.back()!='_'&&(i+1<name.size()&&name[i+1]>='a'&&name[i+1]<='z'))out.push_back('_');out.push_back(static_cast<char>(c-'A'+'a'));}else if((c>='a'&&c<='z')||(c>='0'&&c<='9'))out.push_back(c);else if(!out.empty()&&out.back()!='_')out.push_back('_');}
    while(!out.empty()&&out.back()=='_')out.pop_back();return out;
}
bool genericEntityLabel(std::string_view value){
    std::string lower(value);std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return lower.empty()||lower=="actor"||lower=="mob"||lower=="animal"||lower=="entity"||lower=="monster";
}
std::string genericAnimalSpecies(const AABB&bounds,int maxHealth){
    if(!validBounds(bounds))return{};const float w=std::max(bounds.max.x-bounds.min.x,bounds.max.z-bounds.min.z),h=bounds.max.y-bounds.min.y;
    // Data-driven animals can share the C++ Animal RTTI/name tag. Use dimensions
    // plus verified health maximum only as a fallback when Bedrock exposes no
    // species label. Thresholds deliberately leave gaps rather than mislabel.
    if(w>=.82f&&w<=1.02f&&h>=1.12f&&h<=1.45f&&maxHealth>=9&&maxHealth<=12)return"Cow";
    if(w>=.82f&&w<=1.02f&&h>=1.12f&&h<=1.45f&&maxHealth>=7&&maxHealth<=8)return"Sheep";
    if(w>=.38f&&w<=.62f&&h>=.48f&&h<=.86f&&maxHealth>=9&&maxHealth<=12)return"Cow";
    if(w>=.38f&&w<=.62f&&h>=.48f&&h<=.86f&&maxHealth>=7&&maxHealth<=8)return"Sheep";
    if(w>=.80f&&w<=1.02f&&h>=.78f&&h<=1.02f&&maxHealth>=9&&maxHealth<=12)return"Pig";
    if(w>=.38f&&w<=.58f&&h>=.34f&&h<.58f&&maxHealth>=9&&maxHealth<=12)return"Pig";
    if(w>=.52f&&w<=.72f&&h>=.58f&&h<=.82f&&maxHealth>=8&&maxHealth<=12)return"Fox";
    if(w>=.24f&&w<.46f&&h>=.26f&&h<.52f&&maxHealth>=8&&maxHealth<=12)return"Fox";
    if(w>=.30f&&w<=.52f&&h>=.58f&&h<=.82f&&maxHealth>=3&&maxHealth<=6)return"Chicken";
    if(w>=.28f&&w<=.52f&&h>=.38f&&h<=.62f&&maxHealth>=2&&maxHealth<=5)return"Rabbit";
    return{};
}
bool likelyBabyMob(std::string_view species,const AABB&bounds){
    if(!validBounds(bounds))return false;const std::string key=snakeCase(species);const float h=bounds.max.y-bounds.min.y;
    auto has=[&](std::string_view v){return key.find(v)!=std::string::npos;};
    if(has("cow")||has("mooshroom"))return h<1.02f;
    if(has("sheep"))return h<.92f;
    if(has("pig")&&!has("piglin"))return h<.68f;
    if(has("fox")||has("cat")||has("wolf"))return h<.52f;
    if(has("chicken"))return h<.50f;
    if(has("rabbit"))return h<.36f;
    if(has("horse")||has("donkey")||has("mule"))return h<1.18f;
    if(has("llama"))return h<1.35f;
    if(has("hoglin"))return h<1.05f;
    if(has("zombie")&&!has("zombie_villager"))return h<1.45f;
    return false;
}
std::string entityDisplayName(void*actor,const AABB&bounds,int maxHealth){
    if(!actor)return"ENTITY";
    if(g_getNameTag){auto tag=g_getNameTag(actor);auto display=displayName(tag,"");if(!display.empty()&&!genericEntityLabel(display))return display;}
    if(g_actorIsPlayer&&g_actorIsPlayer(actor)){const auto&name=reinterpret_cast<worldanalysis::sdk::Player*>(actor)->name();auto display=displayName(name,"");if(!display.empty())return display;return"PLAYER";}
    for(const std::string className:{worldanalysis::worldoverlay::actorTypeName(actor),actorRttiName(actor)}){
        if(className.empty()||genericEntityLabel(className))continue;const auto id=snakeCase(className);auto localized=i18nLookup("entity."+id+".name");if(localized.empty())localized=i18nLookup("item.spawn_egg.entity."+id+".name");auto display=displayName(localized.empty()?className:localized,"");if(!display.empty()&&!genericEntityLabel(display))return display;
    }
    if(auto species=genericAnimalSpecies(bounds,maxHealth);!species.empty())return displayName(species,"");
    return"ENTITY";
}

std::vector<DropSlice> mobDropProfile(std::string_view species, bool& known) {
    known = true;
    std::string key = snakeCase(species);
    auto has=[&](std::string_view value){ return key.find(value)!=std::string::npos; };
    // Relative base-drop weights. These intentionally exclude conditional
    // player-kill/charged-creeper/equipment drops and normalize the normal
    // loot mix to a 100% pie, so the chart remains readable and comparable.
    std::vector<std::pair<std::string,float>> weights;
    if(has("zombie")&&!has("piglin")&&!has("drowned")) weights={{"ROTTEN FLESH",1.0f},{"IRON INGOT",.025f},{"CARROT",.025f},{"POTATO",.025f}};
    else if(has("drowned")) weights={{"ROTTEN FLESH",1.0f},{"COPPER INGOT",.11f}};
    else if(has("skeleton")&&!has("wither")) weights={{"BONE",1.5f},{"ARROW",1.5f},{"BOW",.085f}};
    else if(has("wither_skeleton")) weights={{"BONE",1.5f},{"COAL",.33f},{"STONE SWORD",.085f}};
    else if(has("creeper")) weights={{"GUNPOWDER",1.0f}};
    else if(has("cave_spider")||has("spider")) weights={{"STRING",1.0f},{"SPIDER EYE",.33f}};
    else if(has("enderman")) weights={{"ENDER PEARL",.5f}};
    else if(has("blaze")) weights={{"BLAZE ROD",.5f}};
    else if(has("ghast")) weights={{"GUNPOWDER",.5f},{"GHAST TEAR",.5f}};
    else if(has("slime")) weights={{"SLIMEBALL",1.0f}};
    else if(has("magma_cube")) weights={{"MAGMA CREAM",.5f}};
    else if(has("guardian")) weights={{"PRISMARINE SHARD",1.0f},{"RAW COD",.4f},{"PRISMARINE CRYSTALS",.33f}};
    else if(has("shulker")) weights={{"SHULKER SHELL",.5f}};
    else if(has("iron_golem")) weights={{"IRON INGOT",4.0f},{"POPPY",1.0f}};
    else if(has("witch")) weights={{"REDSTONE",1.0f},{"GLOWSTONE DUST",1.0f},{"SUGAR",1.0f},{"STICK",1.0f},{"GLASS BOTTLE",1.0f},{"SPIDER EYE",1.0f},{"GUNPOWDER",1.0f}};
    else if(has("cow")||has("mooshroom")) weights={{"RAW BEEF",2.0f},{"LEATHER",1.5f}};
    else if(has("pig")&&!has("piglin")) weights={{"RAW PORKCHOP",2.0f}};
    else if(has("sheep")) weights={{"RAW MUTTON",1.5f},{"WOOL",1.0f}};
    else if(has("chicken")) weights={{"RAW CHICKEN",1.0f},{"FEATHER",1.0f}};
    else if(has("rabbit")) weights={{"RAW RABBIT",1.5f},{"RABBIT HIDE",.75f},{"RABBIT FOOT",.10f}};
    else if(has("horse")||has("donkey")||has("mule")||has("llama")) weights={{"LEATHER",1.5f}};
    else if(has("hoglin")) weights={{"RAW PORKCHOP",3.0f},{"LEATHER",1.0f}};
    else if(has("fox")||has("cat")||has("wolf")) weights={};
    else if(has("phantom")) weights={{"PHANTOM MEMBRANE",1.0f}};
    else if(has("silverfish")) weights={};
    else if(has("endermite")) weights={};
    else if(has("bat")) weights={};
    else if(has("allay")) weights={};
    else if(has("villager")) weights={};
    else { known=false; return {}; }
    float total=0.f;for(const auto&entry:weights)total+=std::max(0.f,entry.second);
    std::vector<DropSlice> result;result.reserve(weights.size());
    if(total<=.0001f)return result;
    for(const auto&entry:weights)result.push_back({entry.first,entry.second*100.f/total});
    return result;
}

bool branchClear(void*region,const Vec3&start,const Vec3&right,int side,const BlockPosRaw&selected){
    if(!g_isSolid||!region)return true;
    for(float t:{.42f,.75f,1.05f}){
        const Vec3 p=add(start,add(mul(right,side*t),Vec3{0,t,0}));
        BlockPosRaw bp=floorPos(p);
        if(samePos(bp,selected))continue;
        if(g_isSolid(region,&bp))return false;
    }
    return true;
}

using Glyph=std::array<std::uint8_t,7>;
const Glyph&glyph(char c){
    static const Glyph blank{0,0,0,0,0,0,0};
    static const std::unordered_map<char,Glyph>m{
        {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},{'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
        {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},{'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},{'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},{'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},{'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},{'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},{'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},{'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},{'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
        {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},{'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
        {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},{'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},{' ',blank},{'#',{0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0}},{'%',{0x19,0x1A,0x04,0x04,0x08,0x0B,0x13}},{':',{0,0x04,0x04,0,0x04,0x04,0}},{'.',{0,0,0,0,0,0x04,0x04}},{'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},{'_',{0,0,0,0,0,0,0x1F}}
    };
    if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');auto it=m.find(c);return it==m.end()?blank:it->second;
}
Vec3 localPoint(const Vec3&o,const Vec3&r,float x,float y){return{o.x+r.x*x,o.y+y,o.z+r.z*x};}
float textWidth(std::string_view s,float px){return s.empty()?0.f:(static_cast<float>(s.size())*6.f-1.f)*px;}
void appendText(std::vector<Segment>&out,std::string_view text,const Vec3&center,const Vec3&right,float px){
    const float w=textWidth(text,px),h=7.f*px;const Vec3 origin=localPoint(center,right,-w*.5f,h*.5f-px);
    for(std::size_t ci=0;ci<text.size();++ci){const auto&g=glyph(text[ci]);for(int row=0;row<7;++row)for(int col=0;col<5;++col){if(!(g[static_cast<std::size_t>(row)]&(1u<<(4-col))))continue;float x=static_cast<float>(ci)*6.f*px+static_cast<float>(col)*px,y=-static_cast<float>(row)*px;out.push_back({localPoint(origin,right,x,y),localPoint(origin,right,x+px*.78f,y)});}}
}
void appendPanelOutline(std::vector<Segment>&out,const Vec3&center,const Vec3&right,float halfWidth,float halfHeight,float progress){
    progress=smoothStep(progress);const Vec3 r=mul(right,halfWidth*progress);const Vec3 u{0,halfHeight*progress,0};
    const Vec3 a=sub(sub(center,r),u),b=add(sub(center,u),r),c=add(add(center,r),u),d=add(sub(center,r),u);
    out.push_back({a,b});out.push_back({b,c});out.push_back({c,d});out.push_back({d,a});
}

void appendThick(std::vector<Segment>&out,const Vec3&a,const Vec3&b,const Vec3&cam,float width){
    Vec3 dir=norm(sub(b,a),{0,1,0}),mid=mul(add(a,b),.5f),perp=norm(cross(dir,norm(sub(cam,mid),{0,0,1})),{1,0,0});
    for(float n:{-2.f,-1.f,0.f,1.f,2.f}){Vec3 off=mul(perp,width*n*.5f);out.push_back({add(a,off),add(b,off)});}
}
void appendRing(std::vector<Segment>&out,const Vec3&center,const Vec3&cam,float radius){
    constexpr int n=20;Vec3 toCam=norm({cam.x-center.x,0,cam.z-center.z},{0,0,1}),right=norm({toCam.z,0,-toCam.x},{1,0,0});
    for(int i=0;i<n;++i){float a=2*kPi*i/n,b=2*kPi*(i+1)/n;Vec3 p1=add(center,add(mul(right,std::cos(a)*radius),Vec3{0,std::sin(a)*radius,0}));Vec3 p2=add(center,add(mul(right,std::cos(b)*radius),Vec3{0,std::sin(b)*radius,0}));appendThick(out,p1,p2,cam,kHalfThickness*.75f);}
}
Vec3 chartPoint(const Vec3&center,const Vec3&right,float angle,float radius){
    return add(center,add(mul(right,std::cos(angle)*radius),Vec3{0,std::sin(angle)*radius,0}));
}
void appendChartArc(std::vector<Segment>&out,const Vec3&center,const Vec3&right,const Vec3&cam,
                    float radius,float turns,bool spoke){
    turns=std::clamp(turns,0.f,1.f);if(turns<=0.f)return;
    constexpr float start=-kPi*.5f;const int segments=std::max(1,static_cast<int>(std::ceil(48.f*turns)));
    Vec3 previous=chartPoint(center,right,start,radius);
    for(int i=1;i<=segments;++i){
        const float phase=turns*static_cast<float>(i)/static_cast<float>(segments);
        const Vec3 point=chartPoint(center,right,start+2.f*kPi*phase,radius);
        appendThick(out,previous,point,cam,kHalfThickness*.80f);previous=point;
    }
    if(spoke)appendThick(out,center,previous,cam,kHalfThickness*.65f);
}
void appendChartFill(std::vector<Quad>&out,const Vec3&center,const Vec3&right,
                     float radius,float turns){
    turns=std::clamp(turns,0.f,1.f);if(turns<=0.f)return;
    // Keep the value disk strictly inside the grey circumference. Each quad is
    // a fan triangle plus one degenerate triangle, using the same primitive
    // mode as WorldOverlay's filled panels.
    constexpr float start=-kPi*.5f;
    const float innerRadius=std::max(.01f,radius-kHalfThickness*1.85f);
    const int wedges=std::max(1,static_cast<int>(std::ceil(64.f*turns)));
    out.reserve(out.size()+static_cast<std::size_t>(wedges));
    for(int i=0;i<wedges;++i){
        const float a=turns*static_cast<float>(i)/static_cast<float>(wedges);
        const float b=turns*static_cast<float>(i+1)/static_cast<float>(wedges);
        const Vec3 p0=chartPoint(center,right,start+2.f*kPi*a,innerRadius);
        const Vec3 p1=chartPoint(center,right,start+2.f*kPi*b,innerRadius);
        out.push_back({center,p0,p1,center});
    }
}
void appendChartSector(std::vector<Quad>&out,const Vec3&center,const Vec3&right,
                       float radius,float fromTurn,float toTurn){
    fromTurn=std::clamp(fromTurn,0.f,1.f);toTurn=std::clamp(toTurn,0.f,1.f);
    if(toTurn<=fromTurn)return;constexpr float start=-kPi*.5f;
    const float innerRadius=std::max(.01f,radius-kHalfThickness*1.85f);
    const int wedges=std::max(1,static_cast<int>(std::ceil(64.f*(toTurn-fromTurn))));
    for(int i=0;i<wedges;++i){
        const float a=fromTurn+(toTurn-fromTurn)*static_cast<float>(i)/wedges;
        const float b=fromTurn+(toTurn-fromTurn)*static_cast<float>(i+1)/wedges;
        const Vec3 p0=chartPoint(center,right,start+2.f*kPi*a,innerRadius);
        const Vec3 p1=chartPoint(center,right,start+2.f*kPi*b,innerRadius);
        out.push_back({center,p0,p1,center});
    }
}

void appendCube(std::vector<Segment>&out,const BlockPosRaw&p){
    constexpr float e=.0025f;float x0=p.x-e,y0=p.y-e,z0=p.z-e,x1=p.x+1+e,y1=p.y+1+e,z1=p.z+1+e;
    Vec3 a{x0,y0,z0},b{x1,y0,z0},c{x1,y0,z1},d{x0,y0,z1},e0{x0,y1,z0},f{x1,y1,z0},g{x1,y1,z1},h{x0,y1,z1};
    out.insert(out.end(),{{a,b},{b,c},{c,d},{d,a},{e0,f},{f,g},{g,h},{h,e0},{a,e0},{b,f},{c,g},{d,h}});
}
Vec3 rotateXZ(const Vec3&origin,float x,float y,float z,float yaw){
    const float c=std::cos(yaw),sn=std::sin(yaw);
    return{origin.x+x*c-z*sn,origin.y+y,origin.z+x*sn+z*c};
}

struct BonePose {
    bool valid=false;
    int parent=-1;
    Vec3 translation{};
    std::array<Vec3,3> axes{};
};
struct RigTransform {
    Vec3 base{};
    float scale=1.f;
    float signX=1.f,signY=1.f,signZ=1.f;
    float yaw=0.f;
};

Vec3 transformBonePoint(const RigTransform&t,const Vec3&p){
    return rotateXZ(t.base,p.x*t.scale*t.signX,p.y*t.scale*t.signY,p.z*t.scale*t.signZ,t.yaw);
}
Vec3 transformBoneDirection(const RigTransform&t,const Vec3&p){
    return sub(rotateXZ({},p.x*t.signX,p.y*t.signY,p.z*t.signZ,t.yaw),Vec3{});
}
float rigTransformScore(const RigTransform&t,const std::vector<BonePose>&poses,const AABB&bounds){
    Vec3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    Vec3 high{-low.x,-low.y,-low.z};
    float outside=0.f;std::size_t count=0;
    const Vec3 size{std::max(bounds.max.x-bounds.min.x,.1f),std::max(bounds.max.y-bounds.min.y,.1f),std::max(bounds.max.z-bounds.min.z,.1f)};
    const Vec3 pad{size.x*.35f+.10f,size.y*.25f+.10f,size.z*.35f+.10f};
    for(const auto&pose:poses){
        if(!pose.valid)continue;
        const Vec3 p=transformBonePoint(t,pose.translation);
        if(!finite(p))return std::numeric_limits<float>::max();
        low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);low.z=std::min(low.z,p.z);
        high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);high.z=std::max(high.z,p.z);
        auto axisOutside=[](float v,float minimum,float maximum){return v<minimum?minimum-v:(v>maximum?v-maximum:0.f);};
        outside+=axisOutside(p.x,bounds.min.x-pad.x,bounds.max.x+pad.x)/size.x;
        outside+=axisOutside(p.y,bounds.min.y-pad.y,bounds.max.y+pad.y)/size.y;
        outside+=axisOutside(p.z,bounds.min.z-pad.z,bounds.max.z+pad.z)/size.z;++count;
    }
    if(count<2)return std::numeric_limits<float>::max();
    const Vec3 actualCenter=mul(add(low,high),.5f),wantedCenter=boundsCenter(bounds);
    const Vec3 centerDelta{sub(actualCenter,wantedCenter)};
    float score=(centerDelta.x*centerDelta.x)/(size.x*size.x)+(centerDelta.y*centerDelta.y)/(size.y*size.y)+(centerDelta.z*centerDelta.z)/(size.z*size.z);
    const Vec3 extent{sub(high,low)};
    auto oversize=[](float actual,float expected){const float over=std::max(0.f,actual-expected*1.75f);return over*over/(expected*expected);};
    score+=oversize(extent.x,size.x)+oversize(extent.y,size.y)+oversize(extent.z,size.z);
    return score+outside/static_cast<float>(count);
}
void addCandidateBase(std::vector<Vec3>&bases,const Vec3&base,const Vec3&targetCenter){
    if(!finite(base)||len(sub(base,targetCenter))>128.f)return;
    for(const auto&existing:bases)if(len(sub(existing,base))<.001f)return;
    bases.push_back(base);
}
bool buildBoneRig(void*actorRenderData,const Vec3*modelTranslation,const Target&target,std::vector<Segment>&out){
    if(!actorRenderData||!validBounds(target.bounds))return false;
    const auto*data=static_cast<const std::byte*>(actorRenderData);
    void*holder=nullptr;std::memcpy(&holder,data+kActorRenderDataAnimation,sizeof(holder));
    if(!plausiblePtr(reinterpret_cast<std::uintptr_t>(holder)))return false;
    auto**vtable=*reinterpret_cast<void***>(holder);
    if(!plausiblePtr(reinterpret_cast<std::uintptr_t>(vtable))||!plausiblePtr(reinterpret_cast<std::uintptr_t>(vtable[2]),4))return false;
    using GetAnimationFn=void*(*)(void*);
    void*animation=reinterpret_cast<GetAnimationFn>(vtable[2])(holder);
    if(!plausiblePtr(reinterpret_cast<std::uintptr_t>(animation)))return false;

    std::uintptr_t begin=0,second=0;
    const auto*span=static_cast<const std::byte*>(animation)+kAnimationBoneSpan;
    std::memcpy(&begin,span,sizeof(begin));std::memcpy(&second,span+sizeof(second),sizeof(second));
    if(!plausiblePtr(begin))return false;
    std::size_t count=0;
    if(second<=kMaxBones)count=static_cast<std::size_t>(second);
    else if(second>=begin&&(second-begin)%kBoneStride==0)count=static_cast<std::size_t>((second-begin)/kBoneStride);
    if(count<2||count>kMaxBones)return false;

    std::vector<BonePose>poses(count);std::size_t validCount=0;
    for(std::size_t i=0;i<count;++i){
        const auto*bone=reinterpret_cast<const std::byte*>(begin+i*kBoneStride);BonePose pose{};
        std::memcpy(&pose.parent,bone,sizeof(pose.parent));
        std::array<float,16>matrix{};std::memcpy(matrix.data(),bone+kBoneMatrix,sizeof(matrix));
        pose.translation={matrix[12],matrix[13],matrix[14]};
        pose.axes={Vec3{matrix[0],matrix[1],matrix[2]},Vec3{matrix[4],matrix[5],matrix[6]},Vec3{matrix[8],matrix[9],matrix[10]}};
        pose.valid=finite(pose.translation)&&std::abs(pose.translation.x)<65536.f&&std::abs(pose.translation.y)<65536.f&&std::abs(pose.translation.z)<65536.f;
        if(pose.valid)++validCount;
        poses[i]=pose;
    }
    if(validCount<2)return false;

    Vec3 renderPosition{};std::memcpy(&renderPosition,data+kActorRenderDataPosition,sizeof(renderPosition));
    float yawDegrees=0.f;std::memcpy(&yawDegrees,data+kActorRenderDataRotation+sizeof(float),sizeof(yawDegrees));
    if(!std::isfinite(yawDegrees))yawDegrees=0.f;
    const float yaw=yawDegrees*(kPi/180.f);const Vec3 targetCenter=boundsCenter(target.bounds);
    std::vector<Vec3>bases;bases.reserve(3);addCandidateBase(bases,renderPosition,targetCenter);
    if(modelTranslation&&finite(*modelTranslation)){
        addCandidateBase(bases,*modelTranslation,targetCenter);
        if(finite(renderPosition))addCandidateBase(bases,add(renderPosition,*modelTranslation),targetCenter);
    }
    if(bases.empty())return false;

    RigTransform best{};float bestScore=std::numeric_limits<float>::max();
    for(const Vec3&base:bases)for(float scale:{1.f,1.f/16.f})for(float sx:{1.f,-1.f})for(float sy:{1.f,-1.f})for(float sz:{1.f,-1.f})for(float yr:{yaw,-yaw}){
        RigTransform candidate{base,scale,sx,sy,sz,yr};const float score=rigTransformScore(candidate,poses,target.bounds);
        if(score<bestScore){bestScore=score;best=candidate;}
    }
    if(!std::isfinite(bestScore)||bestScore>64.f)return false;

    std::vector<unsigned short>children(count,0);out.clear();out.reserve(count*2);
    const float maxDimension=std::max({target.bounds.max.x-target.bounds.min.x,target.bounds.max.y-target.bounds.min.y,target.bounds.max.z-target.bounds.min.z});
    const float maxSegment=std::max(2.f,maxDimension*3.f);
    for(std::size_t i=0;i<count;++i){
        const auto parent=poses[i].parent;if(!poses[i].valid||parent<0||static_cast<std::size_t>(parent)>=count||parent==static_cast<int>(i)||!poses[static_cast<std::size_t>(parent)].valid)continue;
        const Vec3 a=transformBonePoint(best,poses[static_cast<std::size_t>(parent)].translation),b=transformBonePoint(best,poses[i].translation);const float length=len(sub(b,a));
        if(length>.006f&&length<=maxSegment){out.push_back({a,b});++children[static_cast<std::size_t>(parent)];}
    }
    const float leafHalfLength=std::clamp(maxDimension*.035f,.025f,.10f);
    for(std::size_t i=0;i<count;++i){
        if(!poses[i].valid||children[i]!=0)continue;
        Vec3 axis=poses[i].axes[1];
        if(len(axis)<.001f)axis=poses[i].axes[2];
        axis=norm(transformBoneDirection(best,axis),{0,1,0});
        const Vec3 center=transformBonePoint(best,poses[i].translation),offset=mul(axis,leafHalfLength);out.push_back({sub(center,offset),add(center,offset)});
    }
    return !out.empty();
}

void dataDrivenModelRenderHook(void*model,void*renderer,void*actorRenderData,const Vec3*modelTranslation,const Vec3*cameraTarget){
    if(g_modelRenderOriginal)g_modelRenderOriginal(model,renderer,actorRenderData,modelTranslation,cameraTarget);
    if(!g_mod||!g_mod->enabled||!actorRenderData)return;
    void*actor=nullptr;std::memcpy(&actor,actorRenderData,sizeof(actor));if(!actor)return;
    Target target;{std::lock_guard lock(g_mutex);target=g_target;}
    if(!target.valid||target.kind!=TargetKind::Entity||target.isPlayer
        ||target.identity!=reinterpret_cast<std::uintptr_t>(actor))return;
    std::vector<Segment>segments;if(!buildBoneRig(actorRenderData,modelTranslation,target,segments))return;
    std::lock_guard lock(g_mutex);
    if(!g_target.valid||g_target.kind!=TargetKind::Entity||g_target.identity!=target.identity)return;
    g_bones.identity=target.identity;g_bones.segments=std::move(segments);g_bones.capturedAt=Clock::now();
}
void appendGlowSegments(std::vector<Segment>&out,const std::vector<Segment>&core,const Vec3&cam,float width){for(const auto&s:core)appendThick(out,s.a,s.b,cam,width);}
void draw(void*screen,void*tess,void*mat,const std::vector<Segment>&segments,std::uint32_t color,const Vec3&cam){
    color=worldanalysis::worldoverlay::applyThemeColor(color);
    if(segments.empty()||!screen||!tess||!mat||!g_begin||!g_color||!g_vertex||!g_render)return;
    g_begin(tess,nullptr,4,static_cast<int>(segments.size()*2),0);g_color(tess,((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,((color>>24)&255)/255.f);
    for(const auto&s:segments){g_vertex(tess,s.a.x-cam.x,s.a.y-cam.y,s.a.z-cam.z);g_vertex(tess,s.b.x-cam.x,s.b.y-cam.y,s.b.z-cam.z);}char pad[0x58]{};g_render(screen,tess,mat,pad);
}
void drawQuads(void*screen,void*tess,void*mat,const std::vector<Quad>&quads,std::uint32_t color,const Vec3&cam){
    color=worldanalysis::worldoverlay::applyThemeColor(color);
    if(quads.empty()||!screen||!tess||!mat||!g_begin||!g_color||!g_vertex||!g_render)return;
    g_begin(tess,nullptr,1,static_cast<int>(quads.size()*4),0);g_color(tess,((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,((color>>24)&255)/255.f);
    for(const auto&q:quads)for(const Vec3*p:{&q.a,&q.b,&q.c,&q.d})g_vertex(tess,p->x-cam.x,p->y-cam.y,p->z-cam.z);
    char pad[0x58]{};g_render(screen,tess,mat,pad);
}

void renderLevelHook(void*self,void*screen,void*a3){
    if(g_renderOriginal)g_renderOriginal(self,screen,a3);if(!g_mod||!g_mod->enabled||!self||!screen)return;
    worldanalysis::worldoverlay::ColorScope colorScope(g_mod->color);
    const auto ta=*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(screen)+worldanalysis::sdk::offsets::ScreenContext::mTessellator);
    const auto rp=*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(self)+worldanalysis::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if(ta<0x1000||rp<0x1000)return;
    const auto off=worldanalysis::sdk::offsets::LevelRendererPlayer::mCamPos;Vec3 cam{*reinterpret_cast<float*>(rp+off),*reinterpret_cast<float*>(rp+off+4),*reinterpret_cast<float*>(rp+off+8)};if(!finite(cam))return;
    const auto colorHolderAddress=*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(screen)+worldanalysis::sdk::offsets::ScreenContext::mColorHolder);
    if(colorHolderAddress<0x1000)return;
    auto*colorHolder=reinterpret_cast<float*>(colorHolderAddress);
    const float savedColor[4]{colorHolder[0],colorHolder[1],colorHolder[2],colorHolder[3]};
    colorHolder[0]=1.f;colorHolder[1]=1.f;colorHolder[2]=1.f;colorHolder[3]=1.f;
    void*selection=reinterpret_cast<void*>(rp+worldanalysis::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);ensureMaterial();
    Target t;BoneCapture bones;{std::lock_guard lock(g_mutex);t=g_target;bones=g_bones;}if(!t.valid){colorHolder[0]=savedColor[0];colorHolder[1]=savedColor[1];colorHolder[2]=savedColor[2];colorHolder[3]=savedColor[3];return;}

    const Vec3 center=t.kind==TargetKind::Block?Vec3{t.pos.x+.5f,t.pos.y+.5f,t.pos.z+.5f}:boundsCenter(t.bounds);
    Vec3 start=t.hit;const float maxStartDistance=t.kind==TargetKind::Block?2.f:std::max({t.bounds.max.x-t.bounds.min.x,t.bounds.max.y-t.bounds.min.y,t.bounds.max.z-t.bounds.min.z})+2.f;
    if(!finite(start)||len(sub(start,center))>maxStartDistance)start=center;
    Vec3 toCam{cam.x-center.x,0,cam.z-center.z};toCam=norm(toCam,{0,0,1});const Vec3 right=norm({toCam.z,0,-toCam.x},{1,0,0});
    const Vec3 fullEnd=add(start,add(mul(right,static_cast<float>(t.side)*kBranchLength),Vec3{0,kBranchLength,0}));

    const float elapsed=std::max(0.f,std::chrono::duration<float>(Clock::now()-t.acquiredAt).count());
    const float speed=std::clamp(g_mod->animationSpeed,.50f,1.50f);
    const bool animate=g_mod->animation;
    const float lineSeconds=kLineAnimationSeconds/speed;
    const float fadeSeconds=kTextFadeSeconds/speed;
    float lineProgress=1.f,textAlpha=1.f;
    if(animate){
        lineProgress=smoothStep(elapsed/lineSeconds);
        textAlpha=smoothStep((elapsed-lineSeconds)/fadeSeconds);
    }
    const Vec3 end=add(start,mul(sub(fullEnd,start),lineProgress));
    // Keep the name/ID clearly above the o-o branch and endpoint ring.
    const Vec3 labelCenter=add(fullEnd,Vec3{0,.24f,0});

    // Block analysis is always readable through terrain; entity analysis keeps
    // normal depth so only the requested block info and system alerts ignore walls.
    void*calloutMaterial=t.kind==TargetKind::Block
        ?static_cast<void*>(g_throughMaterial):static_cast<void*>(g_depthMaterial);
    if(!calloutMaterial)calloutMaterial=selection;

    if(t.kind==TargetKind::Block){
        std::vector<Segment>outline;outline.reserve(12);appendCube(outline,t.pos);
        draw(screen,reinterpret_cast<void*>(ta),selection,outline,0xFFFFFFFFu,cam);
    }else if(t.kind==TargetKind::Entity&&!t.isPlayer&&bones.identity==t.identity&&!bones.segments.empty()&&std::chrono::duration<float>(Clock::now()-bones.capturedAt).count()<.25f){
        std::vector<Segment>glow;glow.reserve(bones.segments.size()*5);appendGlowSegments(glow,bones.segments,cam,kHalfThickness*.90f);
        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,glow,0xFFFFFFFFu,cam);
        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,bones.segments,0xFFFFFFFFu,cam);
    }

    std::vector<Segment>callout;callout.reserve(230);appendThick(callout,start,end,cam,kHalfThickness);appendRing(callout,start,cam,kEndpointRadius);
    if(lineProgress>.01f)appendRing(callout,end,cam,kEndpointRadius*std::min(1.f,.30f+lineProgress));
    draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,callout,0xFFFFFFFFu,cam);

    if(textAlpha>0.f){
        std::string label;
        if(t.kind==TargetKind::Entity)label=t.isPlayer?t.name:t.name+" "+(t.hasHealth?std::to_string(t.healthPercent)+"%":"NO HP");
        else if(t.id!=0)label=t.name+" #"+std::to_string(t.id);
        else label=t.name+" "+displayIdentifier(t.identifier);
        if(label.size()>38)label.resize(38);
        std::vector<Segment>textCore,textGlow;textCore.reserve(360);textGlow.reserve(1800);appendText(textCore,label,labelCenter,right,.023f);appendGlowSegments(textGlow,textCore,cam,kTextGlowThickness);
        const auto textColor=whiteWithAlpha(textAlpha);
        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,textGlow,textColor,cam);
        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,textCore,textColor,cam);
    }
    if(g_mod->circleChart
            &&textAlpha>0.f&&((t.kind==TargetKind::Entity&&t.dropProfileKnown)
                ||(!t.chartMaximum.empty()&&!t.chartCurrent.empty()))){
        const Vec3 chartCenter=add(fullEnd,add(mul(right,static_cast<float>(t.side)*.58f),Vec3{0,-.16f,0}));
        const float chartStart=lineSeconds+kChartDelaySeconds;
        float greyProgress=1.f,valueProgress=1.f,infoAlpha=1.f;
        if(animate){
            greyProgress=smoothStep((elapsed-chartStart)/(kChartCircleSeconds/speed));
            valueProgress=smoothStep((elapsed-chartStart-kChartCircleSeconds/speed)/(kChartValueSeconds/speed));
            infoAlpha=smoothStep((elapsed-chartStart-kChartCircleSeconds/speed)/(.14f/speed));
        }
        if(!animate||elapsed>=chartStart){
            std::vector<Segment>grey;grey.reserve(300);
            appendChartArc(grey,chartCenter,right,cam,kChartRadius,std::max(greyProgress,.001f),greyProgress<.999f);
            draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,grey,colorWithAlpha(0x00888888u,textAlpha),cam);
            const Vec3 valueCenter=add(chartCenter,mul(norm(sub(cam,chartCenter),{0,0,1}),.006f));
            if(t.kind==TargetKind::Entity){
                static constexpr std::array<std::uint8_t,8> shades{{248,222,198,174,150,126,104,82}};
                float cumulative=0.f;
                for(std::size_t i=0;i<t.drops.size();++i){
                    const float begin=cumulative;
                    cumulative=std::min(1.f,cumulative+t.drops[i].percent/100.f);
                    const float visibleEnd=std::min(cumulative,valueProgress);
                    if(visibleEnd>begin){
                        std::vector<Quad>sector;sector.reserve(24);
                        appendChartSector(sector,valueCenter,right,kChartRadius,begin,visibleEnd);
                        const std::uint8_t shade=shades[i%shades.size()];
                        const std::uint32_t rgb=(static_cast<std::uint32_t>(shade)<<16)|(static_cast<std::uint32_t>(shade)<<8)|shade;
                        drawQuads(screen,reinterpret_cast<void*>(ta),calloutMaterial,sector,colorWithAlpha(rgb,textAlpha),cam);
                    }
                }
                if(infoAlpha>0.f){
                    const float alpha=textAlpha*infoAlpha;
                    if(t.drops.empty()){
                        std::vector<Segment>text,glow;appendText(text,"NO BASE DROPS",add(chartCenter,Vec3{0,-.24f,0}),right,.0115f);appendGlowSegments(glow,text,cam,kTextGlowThickness*.70f);
                        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,glow,colorWithAlpha(0x00B0B0B0u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,text,whiteWithAlpha(alpha),cam);
                    }else{
                        const Vec3 titleCenter=add(chartCenter,Vec3{0,-.22f,0});
                        std::vector<Segment>title,titleGlow;appendText(title,"BASE DROP MIX",titleCenter,right,.0108f);appendGlowSegments(titleGlow,title,cam,kTextGlowThickness*.65f);
                        draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,titleGlow,colorWithAlpha(0x00A8A8A8u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,title,whiteWithAlpha(alpha),cam);
                        for(std::size_t i=0;i<t.drops.size();++i){
                            const float y=-.32f-static_cast<float>(i)*.090f;const Vec3 rowCenter=add(chartCenter,Vec3{0,y,0});
                            const std::uint8_t shade=shades[i%shades.size()];const std::uint32_t rgb=(static_cast<std::uint32_t>(shade)<<16)|(static_cast<std::uint32_t>(shade)<<8)|shade;
                            std::vector<Segment>swatch{{add(rowCenter,mul(right,-.39f)),add(rowCenter,mul(right,-.28f))}};
                            draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,swatch,colorWithAlpha(rgb,alpha),cam);
                            std::string label=t.drops[i].item+" "+std::to_string(static_cast<int>(std::lround(t.drops[i].percent)))+"%";
                            if(label.size()>28){label.resize(25);label+="...";}
                            std::vector<Segment>text,glow;appendText(text,label,add(rowCenter,mul(right,.08f)),right,.0102f);appendGlowSegments(glow,text,cam,kTextGlowThickness*.62f);
                            draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,glow,colorWithAlpha(rgb,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,text,colorWithAlpha(rgb,alpha),cam);
                        }
                    }
                }
            }else{
                std::vector<Quad>value;value.reserve(72);
                if(valueProgress>0.f)appendChartFill(value,valueCenter,right,kChartRadius,std::clamp(t.chartPercent/100.f,0.f,1.f)*valueProgress);
                drawQuads(screen,reinterpret_cast<void*>(ta),calloutMaterial,value,whiteWithAlpha(textAlpha),cam);
                if(infoAlpha>0.f){
                    const Vec3 maximumCenter=add(chartCenter,Vec3{0,-.21f,0});const Vec3 currentCenter=add(chartCenter,Vec3{0,-.31f,0});
                    std::vector<Segment>maximumText,currentText,maximumGlow,currentGlow;appendText(maximumText,t.chartMaximum,maximumCenter,right,.0115f);appendText(currentText,t.chartCurrent,currentCenter,right,.0115f);appendGlowSegments(maximumGlow,maximumText,cam,kTextGlowThickness*.72f);appendGlowSegments(currentGlow,currentText,cam,kTextGlowThickness*.72f);const float alpha=textAlpha*infoAlpha;
                    draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,maximumGlow,colorWithAlpha(0x00888888u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,maximumText,colorWithAlpha(0x00888888u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,currentGlow,whiteWithAlpha(alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,currentText,whiteWithAlpha(alpha),cam);
                }
            }
        }
    }
    if(g_mod->toolCallout&&t.kind==TargetKind::Block&&!t.bestTool.empty()){
        const float toolDelay=.08f/speed;
        float toolProgress=1.f,toolAlpha=1.f;
        if(animate){toolProgress=smoothStep((elapsed-toolDelay)/lineSeconds);toolAlpha=smoothStep((elapsed-toolDelay-lineSeconds)/fadeSeconds);}
        if(!animate||elapsed>=toolDelay){
            const Vec3 toolFullEnd=add(start,add(mul(right,static_cast<float>(t.side)*.92f),Vec3{0,-.82f,0}));
            const Vec3 toolEnd=add(start,mul(sub(toolFullEnd,start),toolProgress));
            std::vector<Segment>toolLine;toolLine.reserve(180);appendThick(toolLine,start,toolEnd,cam,kHalfThickness);appendRing(toolLine,start,cam,kEndpointRadius*.86f);if(toolProgress>.02f)appendRing(toolLine,toolEnd,cam,kEndpointRadius*.86f*std::min(1.f,.3f+toolProgress));
            draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,toolLine,whiteWithAlpha(std::max(.18f,toolAlpha)),cam);
            if(toolAlpha>0.f){
                const std::string toolLabel=std::string("Tool: ")+t.bestTool;
                constexpr float preferredPx=.0155f;
                const float requiredHalf=textWidth(toolLabel,preferredPx)*.5f+.10f;
                const float panelHalfWidth=std::clamp(requiredHalf,.43f,.92f);
                const float innerWidth=panelHalfWidth*2.f-.16f;
                const float units=std::max(1.f,static_cast<float>(toolLabel.size())*6.f-1.f);
                const float fittedPx=std::clamp(std::min(preferredPx,innerWidth/units),.0090f,preferredPx);
                // Keep a visible gap between the endpoint o-ring and the box.
                // The previous -0.20 offset let the animated ring overlap the
                // panel's top edge at close range.
                const Vec3 panelCenter=add(toolFullEnd,Vec3{0,-.31f,0});
                std::vector<Segment>panel,panelText,panelGlow;appendPanelOutline(panel,panelCenter,right,panelHalfWidth,.17f,toolProgress);
                appendText(panelText,toolLabel,panelCenter,right,fittedPx);appendGlowSegments(panelGlow,panelText,cam,kTextGlowThickness*.75f);
                draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,panel,whiteWithAlpha(toolAlpha),cam);
                draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,panelGlow,whiteWithAlpha(toolAlpha),cam);
                draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,panelText,whiteWithAlpha(toolAlpha),cam);
            }
        }
    }
    if(g_mod->blastChart&&t.kind==TargetKind::Block&&textAlpha>0.f&&!t.blastMaximum.empty()&&!t.blastCurrent.empty()){
        const Vec3 baseChart=add(fullEnd,add(mul(right,static_cast<float>(t.side)*.58f),Vec3{0,-.16f,0}));
        const Vec3 chartCenter2=add(baseChart,Vec3{0,-.66f,0});
        const float chartStart=lineSeconds+kChartDelaySeconds+.18f/speed;
        float greyProgress=1.f,valueProgress=1.f,infoAlpha=1.f;
        if(animate){greyProgress=smoothStep((elapsed-chartStart)/(kChartCircleSeconds/speed));valueProgress=smoothStep((elapsed-chartStart-kChartCircleSeconds/speed)/(kChartValueSeconds/speed));infoAlpha=smoothStep((elapsed-chartStart-kChartCircleSeconds/speed)/(.14f/speed));}
        if(!animate||elapsed>=chartStart){
            std::vector<Segment>grey;std::vector<Quad>value;grey.reserve(300);value.reserve(72);
            appendChartArc(grey,chartCenter2,right,cam,kChartRadius,std::max(greyProgress,.001f),greyProgress<.999f);
            const Vec3 valueCenter=add(chartCenter2,mul(norm(sub(cam,chartCenter2),{0,0,1}),.006f));
            if(valueProgress>0.f)appendChartFill(value,valueCenter,right,kChartRadius,std::clamp(t.blastPercent/100.f,0.f,1.f)*valueProgress);
            draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,grey,colorWithAlpha(0x00888888u,textAlpha),cam);drawQuads(screen,reinterpret_cast<void*>(ta),calloutMaterial,value,whiteWithAlpha(textAlpha),cam);
            if(infoAlpha>0.f){
                const Vec3 maximumCenter=add(chartCenter2,Vec3{0,-.21f,0}),currentCenter=add(chartCenter2,Vec3{0,-.31f,0});
                std::vector<Segment>maximumText,currentText,maximumGlow,currentGlow;appendText(maximumText,t.blastMaximum,maximumCenter,right,.0115f);appendText(currentText,t.blastCurrent,currentCenter,right,.0115f);appendGlowSegments(maximumGlow,maximumText,cam,kTextGlowThickness*.72f);appendGlowSegments(currentGlow,currentText,cam,kTextGlowThickness*.72f);const float alpha=textAlpha*infoAlpha;
                draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,maximumGlow,colorWithAlpha(0x00888888u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,maximumText,colorWithAlpha(0x00888888u,alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,currentGlow,whiteWithAlpha(alpha),cam);draw(screen,reinterpret_cast<void*>(ta),calloutMaterial,currentText,whiteWithAlpha(alpha),cam);
            }
        }
    }
    colorHolder[0]=savedColor[0];colorHolder[1]=savedColor[1];colorHolder[2]=savedColor[2];colorHolder[3]=savedColor[3];
}
} // namespace

bool activeDetermineBlockCallout(int x,int y,int z,int&side){
    std::lock_guard lock(g_mutex);
    if(!g_mod||!g_mod->enabled||!g_target.valid||g_target.kind!=TargetKind::Block
        ||g_target.pos.x!=x||g_target.pos.y!=y||g_target.pos.z!=z)return false;
    side=g_target.side<0?-1:1;
    return true;
}

bool activeDetermineCalloutSide(int&side){
    std::lock_guard lock(g_mutex);
    if(!g_mod||!g_mod->enabled||!g_target.valid)return false;
    side=g_target.side<0?-1:1;
    return true;
}

bool activeDetermineEntityCallout(std::uintptr_t identity,int&side){
    std::lock_guard lock(g_mutex);
    if(!identity||!g_mod||!g_mod->enabled||!g_target.valid
        ||g_target.kind!=TargetKind::Entity||g_target.identity!=identity)return false;
    side=g_target.side<0?-1:1;
    return true;
}

DetermineModule::DetermineModule():Module("Determine","Analyzes the targeted block or entity with smooth o-line-o callouts. Block charts cover toughness/blast data, the lowered tool panel shows the preferred tool family, and supported mobs can show a grayscale base-drop mix with item percentages. Block information stays visible through terrain."){g_mod=this;showInMenu=true;}
DetermineModule::~DetermineModule(){if(g_mod==this)g_mod=nullptr;}
void DetermineModule::onInit(){
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::RenderLevel))m_patchTarget=reinterpret_cast<void*>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::DataDrivenModelRender))m_modelPatchTarget=reinterpret_cast<void*>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::LevelGetHitResult))g_getHitResult=reinterpret_cast<LevelGetHitResultFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::I18nGetInstance))g_getI18n=reinterpret_cast<I18nGetInstanceFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::HitResultGetEntity))g_getHitEntity=reinterpret_cast<HitResultGetEntityFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorIsPlayer))g_actorIsPlayer=reinterpret_cast<ActorIsPlayerFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetHealth))g_getHealth=reinterpret_cast<ActorHealthFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetMaxHealth))g_getMaxHealth=reinterpret_cast<ActorHealthFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ActorGetNameTag))g_getNameTag=reinterpret_cast<ActorGetNameTagFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::GetDestroyProgress)){
        // Recover the raw BlockType destroy-speed field through the exact
        // Player::getDestroyProgress call graph instead of hard-coding an
        // offset from a different point release.
        g_destroySpeedOffset=findDestroySpeedOffset(a);
    }
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceGetBlock))g_getBlock=reinterpret_cast<GetBlockFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::BlockSourceIsSolidBlockingBlock))g_isSolid=reinterpret_cast<SolidFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorBegin))g_begin=reinterpret_cast<TessBeginFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorColor))g_color=reinterpret_cast<TessColorFn>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorVertex))g_vertex=reinterpret_cast<TessVertexFn>(a);
    auto rm=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::MeshHelpersRenderMeshImmediately2);if(!rm)rm=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::MeshHelpersRenderMeshImmediately);if(rm)g_render=reinterpret_cast<RenderMeshFn>(rm);
    if(auto rmg=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::RenderMaterialGroupCommon)){if(auto base=resolveADRP(reinterpret_cast<std::uint32_t*>(rmg),2,0))g_materialGroup=base+worldanalysis::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;}
    worldanalysis::events::bus().subscribe<worldanalysis::events::LocalPlayerTickEvent>([](auto&e){if(g_mod)g_mod->handleTick(e.player);});
}
void DetermineModule::applyPatch(){
    if(!m_patched&&m_patchTarget&&worldanalysis::hooks::install(m_patchTarget,reinterpret_cast<void*>(renderLevelHook),reinterpret_cast<void**>(&g_renderOriginal)))m_patched=true;
    if(!m_modelPatched&&m_modelPatchTarget&&worldanalysis::hooks::install(m_modelPatchTarget,reinterpret_cast<void*>(dataDrivenModelRenderHook),reinterpret_cast<void**>(&g_modelRenderOriginal)))m_modelPatched=true;
}
void DetermineModule::onEnable(){applyPatch();std::lock_guard lock(g_mutex);g_target={};g_bones={};}
void DetermineModule::onDisable(){std::lock_guard lock(g_mutex);g_target={};g_bones={};}
void DetermineModule::handleTick(worldanalysis::sdk::Player*player){
    Target next{};
    auto publish=[&](Target&&value){
        const auto now=Clock::now();
        std::lock_guard lock(g_mutex);
        const bool keepsBones=value.valid&&value.kind==TargetKind::Entity&&!value.isPlayer
            &&g_target.valid&&g_target.kind==TargetKind::Entity&&!g_target.isPlayer
            &&value.identity==g_target.identity;
        if(value.valid&&sameTarget(g_target,value))value.acquiredAt=g_target.acquiredAt;
        else if(value.valid)value.acquiredAt=now;
        if(!keepsBones)g_bones={};
        g_target=std::move(value);
    };
    if(!enabled||!player||!g_getHitResult){publish(std::move(next));return;}
    auto*level=player->level();auto*dimension=player->dimension();void*region=dimension?dimension->blockSource():nullptr;auto*hit=level?g_getHitResult(level):nullptr;
    if(!hit){publish(std::move(next));return;}

    if(hit->type()==0){
        if(!region||!g_getBlock){publish(std::move(next));return;}
        const auto*rawPos=reinterpret_cast<const BlockPosRaw*>(reinterpret_cast<const std::byte*>(hit)+worldanalysis::sdk::offsets::HitResult::mBlock);const BlockPosRaw pos=*rawPos;
        void*block=g_getBlock(region,pos);if(!block){publish(std::move(next));return;}
        const auto identifier=blockIdentifier(block);if(identifier.empty()){publish(std::move(next));return;}
        const auto localized=localizedBlockName(identifier);
        next.valid=true;next.kind=TargetKind::Block;next.pos=pos;next.hit=hit->position();next.id=blockId(block,blockIdMode);next.name=displayName(localized);next.identifier=std::string(identifier);
        const float toughness=blockToughness(block,identifier);
        next.chartMaximum="MAX TOUGHNESS "+oneDecimal(kMaximumFiniteToughness);
        if(toughness<0.f){
            next.chartPercent=100.f;next.chartCurrent="UNBREAKABLE 100%";
        }else{
            next.chartPercent=std::clamp(toughness/kMaximumFiniteToughness*100.f,0.f,100.f);
            next.chartCurrent="TOUGHNESS "+oneDecimal(toughness)+" "+std::to_string(static_cast<int>(std::lround(next.chartPercent)))+"%";
        }
        const float blast=blastResistanceProfile(identifier,toughness);
        const float shownBlast=std::min(blast,kBlastReference);
        next.blastPercent=std::clamp(std::log1p(shownBlast)/std::log1p(kBlastReference)*100.f,0.f,100.f);
        next.blastMaximum="BLAST SCALE "+oneDecimal(kBlastReference);
        next.blastCurrent=(blast>kBlastReference?std::string("BLAST RES >")+oneDecimal(kBlastReference):std::string("BLAST RES ")+oneDecimal(blast))
            +" "+std::to_string(static_cast<int>(std::lround(next.blastPercent)))+"%";
        next.bestTool=bestToolForBlock(identifier);

        int preferred=((static_cast<std::uint32_t>(pos.x)*73856093u)^(static_cast<std::uint32_t>(pos.y)*19349663u)^(static_cast<std::uint32_t>(pos.z)*83492791u))&1u?1:-1;
        {std::lock_guard lock(g_mutex);if(g_target.valid&&g_target.kind==TargetKind::Block&&samePos(g_target.pos,pos))preferred=g_target.side;}
        const Vec3 center{pos.x+.5f,pos.y+.5f,pos.z+.5f};Vec3 start=next.hit;if(!finite(start)||len(sub(start,center))>2.f)start=center;const Vec3 p=player->position();const Vec3 view=norm({p.x-center.x,0,p.z-center.z},{0,0,1});const Vec3 right=norm({view.z,0,-view.x},{1,0,0});
        if(!branchClear(region,start,right,preferred,pos)&&branchClear(region,start,right,-preferred,pos))preferred=-preferred;next.side=preferred;
        publish(std::move(next));return;
    }

    if(hit->type()==1&&entities&&g_getHitEntity){
        void*actor=g_getHitEntity(hit);if(!actor||actor==player){publish(std::move(next));return;}
        const AABB bounds=reinterpret_cast<worldanalysis::sdk::Actor*>(actor)->bounds();if(!validBounds(bounds)){publish(std::move(next));return;}
        next.valid=true;next.kind=TargetKind::Entity;next.bounds=bounds;next.hit=hit->position();next.identity=reinterpret_cast<std::uintptr_t>(actor);
        // Player targets are intentionally name-only. Do not read combat health,
        // derive drop data, or render skeleton analysis for other players. Use
        // both the verified isPlayer helper and RTTI as a conservative fallback.
        const std::string preliminaryRtti=actorRttiName(actor);
        next.isPlayer=(g_actorIsPlayer&&g_actorIsPlayer(actor))
            ||preliminaryRtti.find("Player")!=std::string::npos;
        if(next.isPlayer){
            next.name=entityDisplayName(actor,bounds,0);
            next.hasHealth=false;next.dropProfileKnown=false;next.drops.clear();
        }else{
            int maximumHealth=0;
            if(g_getHealth&&g_getMaxHealth){const int maximum=g_getMaxHealth(actor);const int health=g_getHealth(actor);if(maximum>0&&maximum<1000000&&health>-1000000&&health<1000000){maximumHealth=maximum;next.hasHealth=true;next.healthPercent=std::clamp(static_cast<int>(std::lround(static_cast<double>(health)*100.0/static_cast<double>(maximum))),0,100);}}
            next.name=entityDisplayName(actor,bounds,maximumHealth);
            next.speciesKey=preliminaryRtti;
            if(genericEntityLabel(next.speciesKey)){const auto overlayType=worldanalysis::worldoverlay::actorTypeName(actor);if(!genericEntityLabel(overlayType))next.speciesKey=overlayType;}
            if(genericEntityLabel(next.speciesKey)||next.speciesKey.empty())next.speciesKey=next.name;
            next.isBaby=likelyBabyMob(next.speciesKey,bounds);
            if(next.isBaby&&!next.name.starts_with("BABY "))next.name="BABY "+next.name;
            if(next.isBaby){next.dropProfileKnown=true;next.drops.clear();}
            else{next.drops=mobDropProfile(next.speciesKey,next.dropProfileKnown);if(!next.dropProfileKnown)next.drops=mobDropProfile(next.name,next.dropProfileKnown);}
        }
        int preferred=((next.identity>>4)^next.identity)&1u?1:-1;
        {std::lock_guard lock(g_mutex);if(g_target.valid&&g_target.kind==TargetKind::Entity&&g_target.identity==next.identity)preferred=g_target.side;}
        const Vec3 center=boundsCenter(bounds);Vec3 start=next.hit;if(!finite(start)||len(sub(start,center))>std::max({bounds.max.x-bounds.min.x,bounds.max.y-bounds.min.y,bounds.max.z-bounds.min.z})+2.f)start=center;
        const Vec3 p=player->position();const Vec3 view=norm({p.x-center.x,0,p.z-center.z},{0,0,1});const Vec3 right=norm({view.z,0,-view.x},{1,0,0});const BlockPosRaw selected=floorPos(center);
        if(region&&!branchClear(region,start,right,preferred,selected)&&branchClear(region,start,right,-preferred,selected))preferred=-preferred;next.side=preferred;
        publish(std::move(next));return;
    }
    publish(std::move(next));
}
void DetermineModule::loadConfig(const nlohmann::json&j){Module::loadConfig(j);animation=j.value("animation",animation);entities=j.value("entities",entities);circleChart=j.value("circleChart",circleChart);blastChart=j.value("blastChart",blastChart);toolCallout=j.value("toolCallout",toolCallout);animationSpeed=std::clamp(j.value("animationSpeed",animationSpeed),.50f,1.50f);blockIdMode=std::clamp(j.value("blockIdMode",blockIdMode),0,2);}
void DetermineModule::saveConfig(nlohmann::json&j){Module::saveConfig(j);j["animation"]=animation;j["entities"]=entities;j["circleChart"]=circleChart;j["blastChart"]=blastChart;j["toolCallout"]=toolCallout;j["animationSpeed"]=std::clamp(animationSpeed,.50f,1.50f);j["blockIdMode"]=std::clamp(blockIdMode,0,2);}
