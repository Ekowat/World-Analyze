#include "worldoverlay.hpp"
#include "core/ui/ColorUtil.hpp"

#include "core/memory/Hooks.hpp"
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace worldanalysis::worldoverlay {
namespace {

using TessBegin = void (*)(void*, void*, int, int, int);
using TessColor = void (*)(void*, float, float, float, float);
using TessVertex = void (*)(void*, float, float, float);
using RenderMesh = void (*)(void*, void*, void*, char*);

struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;

    explicit HashedString(std::string_view text) : value(text) {
        if (text.empty()) return;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t h = kOffset;
        for (unsigned char c : text) h = static_cast<std::uint64_t>(c) ^ (kPrime * h);
        hash = h;
    }
};

using MaterialPtr = std::shared_ptr<void>;

TessBegin g_begin = nullptr;
TessColor g_color = nullptr;
TessVertex g_vertex = nullptr;
RenderMesh g_render = nullptr;
std::uintptr_t g_group = 0;
MaterialPtr* g_nameTag = nullptr;
MaterialPtr* g_nameTagDepth = nullptr;
MaterialPtr* g_selection = nullptr;
MaterialPtr* g_selectionOverlay = nullptr;
bool g_initialized = false;

using RenderLevelFn = void (*)(void*, void*, void*);
RenderLevelFn g_renderLevelOriginal = nullptr;
bool g_renderDispatcherInstalled = false;
std::vector<RenderCallback> g_renderCallbacks;
std::mutex g_renderCallbacksMutex;
thread_local std::uint32_t g_themeForeground = 0x00FFFFFFu;
thread_local std::uint32_t g_themeBackground = 0x00000000u;
thread_local float g_themeBackgroundOpacity = 1.0f;
thread_local bool g_themeForegroundEnabled = false;
thread_local bool g_themeBackgroundEnabled = false;

std::vector<RenderCallback> callbackSnapshot(
        const std::vector<RenderCallback>& source) {
    std::lock_guard lock(g_renderCallbacksMutex);
    return source;
}

void renderLevelDispatcher(void* self, void* screen, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screen, a3);
    for (auto callback : callbackSnapshot(g_renderCallbacks))
        if (callback) callback(self, screen, a3);
}

std::uintptr_t adrp(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i=0;i<count;++i) {
        const std::uint32_t insn=insns[i];
        if ((insn&0x1F)!=targetReg) continue;
        if ((insn&0x9F000000)==0x90000000) {
            std::uintptr_t page=(reinterpret_cast<std::uintptr_t>(&insns[i])&~0xFFFULL)+
                ((static_cast<std::int64_t>(static_cast<std::uint64_t>(((insn>>3)&0x1FFFFC)|((insn>>29)&3))<<43))>>31);
            for(std::size_t j=i+1;j<count;++j) {
                const std::uint32_t add=insns[j];
                if((add&0xFF000000)==0x91000000&&((add>>5)&0x1F)==targetReg&&(add&0x1F)==targetReg) {
                    std::uint32_t imm12=(add>>10)&0xFFF; if(add&0x400000)imm12<<=12; return page+imm12;
                }
                if((add&0x1F)==targetReg)break;
            }
        }
        if ((insn&0x9F000000)==0x10000000) {
            const std::int64_t imm=(static_cast<std::int64_t>(static_cast<std::uint64_t>(((insn>>3)&0x1FFFFC)|(insn>>29))<<43))>>43;
            return reinterpret_cast<std::uintptr_t>(&insns[i])+imm;
        }
    }
    return 0;
}

MaterialPtr* getMaterial(std::string_view name) {
    if (!g_group) return nullptr;
    HashedString hashed(name);
    auto** vtable = *reinterpret_cast<void***>(g_group);
    if (!vtable || !vtable[2]) return nullptr;
    using Fn = MaterialPtr (*)(void*, const HashedString*);
    MaterialPtr material = reinterpret_cast<Fn>(vtable[2])(reinterpret_cast<void*>(g_group), &hashed);
    return material ? new MaterialPtr(material) : nullptr;
}

void ensureMaterials() {
    if (!g_nameTag) g_nameTag = getMaterial("name_tag");
    if (!g_nameTag) g_nameTag = getMaterial("name_tag_with_backface");
    if (!g_nameTagDepth) g_nameTagDepth = getMaterial("name_tag_depth_tested");
    if (!g_nameTagDepth) g_nameTagDepth = getMaterial("name_tag_depth_tested_with_backface");
    if (!g_selection) g_selection = getMaterial("selection_box");
    if (!g_selectionOverlay) g_selectionOverlay = getMaterial("selection_overlay");
}

struct ColorGuard {
    float* holder = nullptr;
    float saved[4]{1,1,1,1};
    explicit ColorGuard(void* screen) {
        if (!screen) return;
        const auto p = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(screen)+worldanalysis::sdk::offsets::ScreenContext::mColorHolder);
        if (!plausible(p, alignof(float))) return;
        holder = reinterpret_cast<float*>(p);
        for (int i=0;i<4;++i) saved[i]=holder[i];
        holder[0]=holder[1]=holder[2]=holder[3]=1.0f;
    }
    ~ColorGuard() {
        if (!holder) return;
        for (int i=0;i<4;++i) holder[i]=saved[i];
    }
};

void setColor(void* tess, std::uint32_t c) {
    c = applyThemeColor(c);
    const float a=((c>>24)&0xFF)/255.0f;
    const float r=((c>>16)&0xFF)/255.0f;
    const float g=((c>>8)&0xFF)/255.0f;
    const float b=(c&0xFF)/255.0f;
    g_color(tess,r,g,b,a);
}

Vec3 add(const Vec3&a,const Vec3&b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(const Vec3&a,float s){return{a.x*s,a.y*s,a.z*s};}

void billboardBasisInternal(const Vec3& center,const Vec3& camera,Vec3& right,Vec3& up) {
    Vec3 toCam{camera.x-center.x,0.0f,camera.z-center.z};
    const float len=std::sqrt(toCam.x*toCam.x+toCam.z*toCam.z);
    right=len>0.0001f?Vec3{toCam.z/len,0.0f,-toCam.x/len}:Vec3{1,0,0};
    up={0,1,0};
}

using Glyph = std::array<std::uint8_t,7>;
const Glyph& glyph(char c) {
    static const Glyph blank{0,0,0,0,0,0,0};
    static const std::unordered_map<char,Glyph> map{
        {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
        {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
        {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
        {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
        {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
        {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
        {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
        {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
        {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
        {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
        {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
        {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
        {'-',{0,0,0,0x1F,0,0,0}}, {'.',{0,0,0,0,0,0x0C,0x0C}}, {':',{0,0x0C,0x0C,0,0x0C,0x0C,0}},
        {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}}, {'#',{0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0}},
        {'!',{0x04,0x04,0x04,0x04,0x04,0,0x04}}, {'?',{0x0E,0x11,0x01,0x02,0x04,0,0x04}},
        {'+',{0,0x04,0x04,0x1F,0x04,0x04,0}}, {'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
        {']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}}, {'_',{0,0,0,0,0,0,0x1F}},
        {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}}, {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
        {'=',{0,0x1F,0,0x1F,0,0,0}}, {'<',{0x01,0x02,0x04,0x08,0x04,0x02,0x01}},
        {'>',{0x10,0x08,0x04,0x02,0x04,0x08,0x10}}, {' ',{0,0,0,0,0,0,0}}
    };
    if (c>='a'&&c<='z') c=static_cast<char>(c-'a'+'A');
    auto it=map.find(c); return it==map.end()?blank:it->second;
}

} // namespace

bool plausible(std::uintptr_t p,std::size_t alignment) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    // Keep Android's TBI/MTE top-byte tag intact for dereferences, but ignore
    // it while checking the user-space address and alignment.
    constexpr std::uintptr_t kAddressMask=0x00FFFFFFFFFFFFFFULL;
    constexpr std::uintptr_t kMaxUserAddress=0x0010000000000000ULL;
    const std::uintptr_t address=p&kAddressMask;
    if(address<0x10000||address>=kMaxUserAddress
        ||(alignment>1&&(address&(alignment-1))))return false;
#else
    if (p<0x10000 || (alignment>1 && (p&(alignment-1)))) return false;
#endif
    return true;
}

bool registerRenderCallback(RenderCallback callback) {
    if (!callback) return false;
    {
        std::lock_guard lock(g_renderCallbacksMutex);
        if (std::find(g_renderCallbacks.begin(), g_renderCallbacks.end(), callback) == g_renderCallbacks.end())
            g_renderCallbacks.push_back(callback);
    }
    if (g_renderDispatcherInstalled) return true;
    const auto address = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::RenderLevel);
    if (!address) return false;
    if (!worldanalysis::hooks::install(reinterpret_cast<void*>(address),
            reinterpret_cast<void*>(renderLevelDispatcher),
            reinterpret_cast<void**>(&g_renderLevelOriginal))) return false;
    g_renderDispatcherInstalled = true;
    return true;
}

bool initialize() {
    if (g_initialized) return g_begin&&g_color&&g_vertex&&g_render;
    g_initialized=true;
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorBegin))g_begin=reinterpret_cast<TessBegin>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorColor))g_color=reinterpret_cast<TessColor>(a);
    if(auto a=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::TessellatorVertex))g_vertex=reinterpret_cast<TessVertex>(a);
    auto r=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if(!r)r=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    if(r)g_render=reinterpret_cast<RenderMesh>(r);
    if(auto rmg=worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::RenderMaterialGroupCommon)) {
        if(auto base=adrp(reinterpret_cast<std::uint32_t*>(rmg),2,0)) g_group=base+worldanalysis::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
    }
    ensureMaterials();
    return g_begin&&g_color&&g_vertex&&g_render;
}

bool makeContext(void* levelRenderer,void* screenContext,RenderContext& out) {
    if(!initialize()||!levelRenderer||!screenContext)return false;
    const auto t=*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(screenContext)+worldanalysis::sdk::offsets::ScreenContext::mTessellator);
    const auto rp=*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(levelRenderer)+worldanalysis::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if(!plausible(t)||!plausible(rp))return false;
    const auto off=worldanalysis::sdk::offsets::LevelRendererPlayer::mCamPos;
    Vec3 cam{*reinterpret_cast<float*>(rp+off),*reinterpret_cast<float*>(rp+off+4),*reinterpret_cast<float*>(rp+off+8)};
    if(!std::isfinite(cam.x)||!std::isfinite(cam.y)||!std::isfinite(cam.z))return false;
    out={screenContext,reinterpret_cast<void*>(t),reinterpret_cast<void*>(rp),cam};return true;
}

void* material(std::string_view name){initialize();return getMaterial(name);}
void* throughWallMaterial(){initialize();ensureMaterials();return g_nameTag?static_cast<void*>(g_nameTag):(g_selectionOverlay?static_cast<void*>(g_selectionOverlay):static_cast<void*>(g_selection));}
void* depthMaterial(){initialize();ensureMaterials();return g_nameTagDepth?static_cast<void*>(g_nameTagDepth):(g_selection?static_cast<void*>(g_selection):(g_selectionOverlay?static_cast<void*>(g_selectionOverlay):static_cast<void*>(g_nameTag)));}

void drawLines(const RenderContext&ctx,void*mat,const std::vector<Segment>&lines,std::uint32_t argb,float thickness) {
    if(!ctx.screen||!ctx.tessellator||!mat||lines.empty()||!g_begin||!g_color||!g_vertex||!g_render)return;
    ColorGuard guard(ctx.screen);
    const int strands=std::clamp(static_cast<int>(std::lround(thickness)),1,4);
    // The detailed World Scan can submit tens of thousands of one-pixel line
    // segments.  Avoid allocating/copying a second vector for the overwhelmingly
    // common one-strand case; this keeps the native tessellator path cheap while
    // preserving the legacy faux-thickness expansion for thicker outlines.
    if(strands==1){
        g_begin(ctx.tessellator,nullptr,4,static_cast<int>(lines.size()*2),0);setColor(ctx.tessellator,argb);
        for(const auto&l:lines){g_vertex(ctx.tessellator,l.a.x-ctx.camera.x,l.a.y-ctx.camera.y,l.a.z-ctx.camera.z);g_vertex(ctx.tessellator,l.b.x-ctx.camera.x,l.b.y-ctx.camera.y,l.b.z-ctx.camera.z);}
        char pad[0x58]{};g_render(ctx.screen,ctx.tessellator,mat,pad);return;
    }
    std::vector<Segment> expanded; expanded.reserve(lines.size()*static_cast<std::size_t>(strands));
    for(const auto&l:lines){for(int i=0;i<strands;++i){const float dy=(i-(strands-1)*.5f)*.0035f;expanded.push_back({{l.a.x,l.a.y+dy,l.a.z},{l.b.x,l.b.y+dy,l.b.z}});}}
    g_begin(ctx.tessellator,nullptr,4,static_cast<int>(expanded.size()*2),0);setColor(ctx.tessellator,argb);
    for(const auto&l:expanded){g_vertex(ctx.tessellator,l.a.x-ctx.camera.x,l.a.y-ctx.camera.y,l.a.z-ctx.camera.z);g_vertex(ctx.tessellator,l.b.x-ctx.camera.x,l.b.y-ctx.camera.y,l.b.z-ctx.camera.z);}
    char pad[0x58]{};g_render(ctx.screen,ctx.tessellator,mat,pad);
}

void drawQuads(const RenderContext&ctx,void*mat,const std::vector<Quad>&quads,std::uint32_t argb) {
    if(!ctx.screen||!ctx.tessellator||!mat||quads.empty()||!g_begin||!g_color||!g_vertex||!g_render)return;
    ColorGuard guard(ctx.screen);g_begin(ctx.tessellator,nullptr,1,static_cast<int>(quads.size()*4),0);setColor(ctx.tessellator,argb);
    for(const auto&q:quads){const std::array<const Vec3*,4> points{&q.a,&q.b,&q.c,&q.d};for(const Vec3* p:points)g_vertex(ctx.tessellator,p->x-ctx.camera.x,p->y-ctx.camera.y,p->z-ctx.camera.z);}
    char pad[0x58]{};g_render(ctx.screen,ctx.tessellator,mat,pad);
}

float billboardTextWidth(std::string_view text,float pixelSize){return text.empty()?0.0f:((static_cast<float>(text.size())*6.0f)-1.0f)*pixelSize;}
float billboardTextHeight(float pixelSize){return 7.0f*pixelSize;}
float fitBillboardTextPixelSize(std::string_view raw,float maxWidth,float preferredPixelSize,float minimumPixelSize){
    const std::string text=sanitizeText(raw,32);
    if(text.empty()||maxWidth<=0.f||preferredPixelSize<=0.f)return std::max(0.f,minimumPixelSize);
    const float units=std::max(1.f,static_cast<float>(text.size())*6.f-1.f);
    const float fitted=maxWidth/units;
    return std::clamp(std::min(preferredPixelSize,fitted),std::max(0.001f,minimumPixelSize),preferredPixelSize);
}
void billboardBasis(const Vec3&center,const Vec3&camera,Vec3&right,Vec3&up){billboardBasisInternal(center,camera,right,up);}

void drawBillboardTextOriented(const RenderContext&ctx,void*mat,std::string_view raw,const Vec3&center,const Vec3&right,const Vec3&up,float pixelSize,std::uint32_t argb,bool background,std::uint32_t backgroundArgb) {
    if(raw.empty()||pixelSize<=0||!mat)return;const std::string text=sanitizeText(raw,32);if(text.empty())return;
    const float width=billboardTextWidth(text,pixelSize),height=billboardTextHeight(pixelSize);
    if(background){
        const float px=pixelSize*.85f;const Vec3 bl=add(add(center,mul(right,-width*.5f-px)),mul(up,-height*.5f-px));const Vec3 br=add(add(center,mul(right,width*.5f+px)),mul(up,-height*.5f-px));const Vec3 tr=add(add(center,mul(right,width*.5f+px)),mul(up,height*.5f+px));const Vec3 tl=add(add(center,mul(right,-width*.5f-px)),mul(up,height*.5f+px));
        drawQuads(ctx,mat,{{bl,br,tr,tl}},backgroundArgb);
    }
    std::vector<Quad> pixels;pixels.reserve(text.size()*24);const Vec3 origin=add(add(center,mul(right,-width*.5f)),mul(up,height*.5f-pixelSize));
    for(std::size_t ci=0;ci<text.size();++ci){const auto&g=glyph(text[ci]);const float x0=static_cast<float>(ci)*6.0f*pixelSize;for(int row=0;row<7;++row){for(int col=0;col<5;++col){if((g[static_cast<std::size_t>(row)]&(1u<<(4-col)))==0)continue;const float x=x0+col*pixelSize,y=-row*pixelSize;const Vec3 tl=add(add(origin,mul(right,x)),mul(up,y));const Vec3 tr=add(tl,mul(right,pixelSize*.88f));const Vec3 br=add(tr,mul(up,-pixelSize*.88f));const Vec3 bl=add(tl,mul(up,-pixelSize*.88f));pixels.push_back({bl,br,tr,tl});}}}
    drawQuads(ctx,mat,pixels,argb);
}

void drawBillboardText(const RenderContext&ctx,void*mat,std::string_view raw,const Vec3&center,float pixelSize,std::uint32_t argb,bool background,std::uint32_t backgroundArgb) {
    Vec3 right{},up{};billboardBasisInternal(center,ctx.camera,right,up);
    drawBillboardTextOriented(ctx,mat,raw,center,right,up,pixelSize,argb,background,backgroundArgb);
}

void drawBillboardSquare(const RenderContext&ctx,void*mat,const Vec3&center,float size,std::uint32_t argb,float thickness,bool fill,std::uint32_t fillArgb) {
    Vec3 right{},up{};billboardBasisInternal(center,ctx.camera,right,up);const float h=size*.5f;const Vec3 bl=add(add(center,mul(right,-h)),mul(up,-h));const Vec3 br=add(add(center,mul(right,h)),mul(up,-h));const Vec3 tr=add(add(center,mul(right,h)),mul(up,h));const Vec3 tl=add(add(center,mul(right,-h)),mul(up,h));if(fill)drawQuads(ctx,mat,{{bl,br,tr,tl}},fillArgb);drawLines(ctx,mat,{{bl,br},{br,tr},{tr,tl},{tl,bl}},argb,thickness);
}

std::uint32_t parseColor(std::string_view value,std::uint32_t fallback) {
    return worldanalysis::ui::parseColorOr(value, fallback);
}

std::uint32_t applyThemeColor(std::uint32_t argb) {
    const std::uint32_t rgb=argb&0x00FFFFFFu;
    std::uint32_t alpha=(argb>>24)&255u;
    if(rgb==0u && g_themeBackgroundEnabled){
        alpha=static_cast<std::uint32_t>(std::clamp(static_cast<float>(alpha)*g_themeBackgroundOpacity,0.f,255.f)+.5f);
        return (alpha<<24)|(g_themeBackground&0x00FFFFFFu);
    }
    if(rgb!=0u && g_themeForegroundEnabled) return (argb&0xFF000000u)|(g_themeForeground&0x00FFFFFFu);
    return argb;
}

ColorScope::ColorScope(std::string_view foreground):ColorScope(foreground,"#000000",1.0f) {}
ColorScope::ColorScope(std::string_view foreground,std::string_view background,float backgroundOpacity) {
    mPrevForeground=g_themeForeground;mPrevBackground=g_themeBackground;mPrevBackgroundOpacity=g_themeBackgroundOpacity;
    mPrevForegroundEnabled=g_themeForegroundEnabled;mPrevBackgroundEnabled=g_themeBackgroundEnabled;
    const auto fg=parseColor(foreground,0xFFFFFFFFu),bg=parseColor(background,0xFF000000u);
    g_themeForeground=fg&0x00FFFFFFu; g_themeBackground=bg&0x00FFFFFFu;
    g_themeForegroundEnabled=(g_themeForeground!=0x00FFFFFFu);
    g_themeBackgroundOpacity=std::clamp(backgroundOpacity,0.f,1.f);
    g_themeBackgroundEnabled=(g_themeBackground!=0u)||g_themeBackgroundOpacity<.999f;
}
ColorScope::~ColorScope(){g_themeForeground=mPrevForeground;g_themeBackground=mPrevBackground;g_themeBackgroundOpacity=mPrevBackgroundOpacity;g_themeForegroundEnabled=mPrevForegroundEnabled;g_themeBackgroundEnabled=mPrevBackgroundEnabled;}

std::string sanitizeText(std::string_view input,std::size_t maxChars) {
    std::string out;out.reserve(std::min(maxChars,input.size()));bool skipFormat=false;
    for(std::size_t i=0;i<input.size()&&out.size()<maxChars;++i){const unsigned char c=static_cast<unsigned char>(input[i]);if(c==0xC2&&i+1<input.size()&&static_cast<unsigned char>(input[i+1])==0xA7){++i;skipFormat=true;continue;}if(c==0xA7){skipFormat=true;continue;}if(skipFormat){skipFormat=false;continue;}if(c>=32&&c<127)out.push_back(static_cast<char>(c));}
    while(!out.empty()&&out.back()==' ')out.pop_back();return out;
}

std::string actorTypeName(void* actor) {
    if(!actor||!plausible(reinterpret_cast<std::uintptr_t>(actor)))return{};const auto vt=*reinterpret_cast<std::uintptr_t*>(actor);if(!plausible(vt))return{};
    const auto ti=*reinterpret_cast<std::uintptr_t*>(vt-sizeof(void*));if(!plausible(ti))return{};const auto namePtr=*reinterpret_cast<const char* const*>(ti+sizeof(void*));if(!plausible(reinterpret_cast<std::uintptr_t>(namePtr),1))return{};
    std::string raw;for(int i=0;i<96;++i){char c=namePtr[i];if(!c)break;if(static_cast<unsigned char>(c)<32||static_cast<unsigned char>(c)>=127)return{};raw.push_back(c);}if(raw.empty())return{};
    std::string best;for(std::size_t i=0;i<raw.size();){if(std::isdigit(static_cast<unsigned char>(raw[i]))){std::size_t n=0;while(i<raw.size()&&std::isdigit(static_cast<unsigned char>(raw[i]))){n=n*10+static_cast<std::size_t>(raw[i]-'0');++i;}if(n&&i+n<=raw.size()){best=raw.substr(i,n);i+=n;continue;}}++i;}
    if(best.empty())best=raw;const std::string suffix="Actor";if(best.size()>suffix.size()&&best.compare(best.size()-suffix.size(),suffix.size(),suffix)==0)best.resize(best.size()-suffix.size());return sanitizeText(best,24);
}

} // namespace worldanalysis::worldoverlay
