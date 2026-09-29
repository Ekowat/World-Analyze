#include "UiOverlay.hpp"

#include "core/memory/Hooks.hpp"
#include <worldanalysis/memory/Signatures.hpp>
#include <worldanalysis/sdk/Offsets.hpp>

#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace worldanalysis::ui {
namespace {

struct Font {};
#pragma pack(push, 4)
struct RectangleArea { float x0, x1, y0, y1; };
struct TextMeasureData { float fontSize; float linePadding; bool renderShadow; bool showColorSymbol; bool hideHyphen; };
struct CaretMeasureData { int position; bool shouldRender; };
#pragma pack(pop)
namespace mce { struct Color { float r,g,b,a; }; }
enum class TextAlignment : std::uint8_t { Left, Right, Center };

struct HashedString {
    std::uint64_t hash = 0;
    std::string text;
    mutable const HashedString* last = nullptr;
    explicit HashedString(const char* value) : text(value ? value : "") {
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        std::uint64_t v = offset;
        for (char c : text) v = static_cast<std::uint64_t>(static_cast<unsigned char>(c)) ^ (prime * v);
        hash = text.empty() ? 0 : v;
    }
};

using DrawTextFn = void (*)(void*, Font&, const RectangleArea&, const std::string&, const mce::Color&, TextAlignment, float, const TextMeasureData&, const CaretMeasureData&);
// ScreenView::setUpAndRender(ScreenView*, MinecraftUIRenderContext*) uses only
// x0/x1 in the supplied 26.45 image.  Keeping the exact prototype avoids
// forwarding unrelated register contents into a large UI routine.
using ScreenViewRenderFn = void (*)(void*, void*);
using FlushImagesFn = void (*)(void*, const mce::Color&, float, const HashedString&);

DrawTextFn g_drawTextOriginal = nullptr;
ScreenViewRenderFn g_screenViewOriginal = nullptr;
FlushImagesFn g_flushImagesOriginal = nullptr;
thread_local Font* g_activeFont = nullptr;
thread_local void* g_activeContext = nullptr;
thread_local bool g_insideScreenView = false;
thread_local bool g_overlayDispatched = false;
thread_local bool g_dispatchingOverlay = false;
bool g_installed = false;
bool g_drawTextHooked = false;
bool g_screenViewHooked = false;
bool g_flushImagesHooked = false;
std::mutex g_mutex;
std::vector<OverlayRenderer> g_renderers;

void** vtable(void* obj) { return obj ? *reinterpret_cast<void***>(obj) : nullptr; }

mce::Color color(std::uint32_t argb, float scale = 1.0f) {
    return {
        ((argb >> 16) & 255) / 255.0f,
        ((argb >> 8) & 255) / 255.0f,
        (argb & 255) / 255.0f,
        std::clamp(((argb >> 24) & 255) / 255.0f * scale, 0.0f, 1.0f)
    };
}

void dispatchOverlay(void* context) {
    if (!context || g_dispatchingOverlay) return;
    g_dispatchingOverlay = true;
    std::vector<OverlayRenderer> renderers;
    {
        std::lock_guard lock(g_mutex);
        renderers = g_renderers;
    }
    for (auto renderer : renderers) if (renderer) renderer(context);
    g_dispatchingOverlay = false;
}

void flushImagesHook(void* self, const mce::Color& color, float alpha, const HashedString& material) {
    g_activeContext = self;
    // InventoryItemRenderer queues its atlas geometry into the active UI image
    // batch. Dispatch immediately before Bedrock submits that batch: this is
    // late enough for the ScreenContext/material state to exist, but early
    // enough for item geometry to be included in the native flush.
    if (g_insideScreenView && !g_overlayDispatched && !g_dispatchingOverlay) {
        g_overlayDispatched = true;
        dispatchOverlay(self);
    }
    if (g_flushImagesOriginal) g_flushImagesOriginal(self, color, alpha, material);
}

void ensureFlushHook(void* context) {
    if (!context || g_flushImagesHooked) return;
    auto** vt = vtable(context);
    const auto slot = worldanalysis::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages;
    void* target = vt ? vt[slot] : nullptr;
    if (!target) return;
    std::lock_guard lock(g_mutex);
    if (g_flushImagesHooked) return;
    g_flushImagesHooked = worldanalysis::hooks::install(
        target, reinterpret_cast<void*>(flushImagesHook),
        reinterpret_cast<void**>(&g_flushImagesOriginal)) != nullptr;
}

void drawTextHook(void* self, Font& font, const RectangleArea& rectangle, const std::string& text,
                  const mce::Color& c, TextAlignment alignment, float alpha,
                  const TextMeasureData& measure, const CaretMeasureData& caret) {
    g_activeContext = self;
    g_activeFont = &font;
    ensureFlushHook(self);
    if (g_drawTextOriginal) g_drawTextOriginal(self, font, rectangle, text, c, alignment, alpha, measure, caret);

    // A first-frame fallback is retained only if the live UI implementation
    // could not be hooked. Once flushImages is observed, it owns dispatch.
    if (!g_flushImagesHooked && g_insideScreenView && !g_overlayDispatched && !g_dispatchingOverlay) {
        g_overlayDispatched = true;
        dispatchOverlay(self);
    }
}

void screenViewHook(void* self, void* context) {
    g_activeContext = context;
    g_activeFont = nullptr;
    g_insideScreenView = true;
    g_overlayDispatched = false;
    ensureFlushHook(context);
    if (g_screenViewOriginal) g_screenViewOriginal(self, context);
    // Text-less screens still receive non-image overlays as a conservative
    // fallback. Normal HUD/container screens always dispatch above while the
    // native image batch is active.
    if (!g_overlayDispatched && g_activeContext) dispatchOverlay(g_activeContext);
    g_insideScreenView = false;
    g_activeContext = nullptr;
    g_activeFont = nullptr;
}

} // namespace

bool ensureInstalled() {
    if (g_installed) return true;
    const auto draw = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::MinecraftUIRenderContextDrawText);
    const auto screen = worldanalysis::memory::resolve(worldanalysis::memory::SignatureId::ScreenViewRender);
    if (!draw || !screen) return false;
    if (!g_drawTextHooked) {
        g_drawTextHooked = worldanalysis::hooks::install(
            reinterpret_cast<void*>(draw), reinterpret_cast<void*>(drawTextHook),
            reinterpret_cast<void**>(&g_drawTextOriginal)) != nullptr;
    }
    if (!g_screenViewHooked) {
        g_screenViewHooked = worldanalysis::hooks::install(
            reinterpret_cast<void*>(screen), reinterpret_cast<void*>(screenViewHook),
            reinterpret_cast<void**>(&g_screenViewOriginal)) != nullptr;
    }
    g_installed = g_drawTextHooked && g_screenViewHooked;
    return g_installed;
}

void addRenderer(OverlayRenderer renderer) {
    if (!renderer) return;
    ensureInstalled();
    std::lock_guard lock(g_mutex);
    if (std::find(g_renderers.begin(), g_renderers.end(), renderer) == g_renderers.end()) g_renderers.push_back(renderer);
}

void fillRect(void* context, float x0, float y0, float x1, float y1, std::uint32_t argb, float alphaScale) {
    auto** vt = vtable(context);
    if (!vt || !vt[worldanalysis::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle]) return;
    using Fn = void (*)(void*, const RectangleArea&, const mce::Color&, float);
    const auto c = color(argb, alphaScale);
    const RectangleArea area{x0,x1,y0,y1};
    reinterpret_cast<Fn>(vt[worldanalysis::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle])(context, area, c, 1.0f);
}

bool drawText(void* context, const std::string& text, float x0, float y0, float x1, float y1,
              std::uint32_t argb, float fontSize, bool centered) {
    if (!context || !g_activeFont || !g_drawTextOriginal || text.empty()) return false;
    const RectangleArea area{x0,x1,y0,y1};
    const TextMeasureData measure{fontSize, 0.0f, true, false, false};
    const CaretMeasureData caret{-1,false};
    g_drawTextOriginal(context, *g_activeFont, area, text, color(argb), centered ? TextAlignment::Center : TextAlignment::Left, 1.0f, measure, caret);
    return true;
}

void flushImages(void* context) {
    auto** vt = vtable(context);
    if (!vt || !vt[worldanalysis::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    static const HashedString material("ui_flush");
    const mce::Color white{1,1,1,1};
    reinterpret_cast<FlushImagesFn>(vt[worldanalysis::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, white, 1.0f, material);
}

void* clientInstance(void* context) {
    return context ? *reinterpret_cast<void**>(static_cast<std::byte*>(context) + worldanalysis::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextClient) : nullptr;
}

void* screenContext(void* context) {
    return context ? *reinterpret_cast<void**>(static_cast<std::byte*>(context) + worldanalysis::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext) : nullptr;
}

bool viewportSize(void* context, float& physicalWidth, float& physicalHeight, float& uiWidth, float& uiHeight) {
    if (!context) return false;
    void* client = clientInstance(context);
    if (!client) return false;
    auto** table = *reinterpret_cast<void***>(client);
    const auto slot = worldanalysis::sdk::offsets::VTable::ClientInstance_getGuiData;
    if (!table || !table[slot]) return false;
    using GetGuiDataFn = void* (*)(void*);
    void* gui = reinterpret_cast<GetGuiDataFn>(table[slot])(client);
    if (!gui) return false;

    struct Size { float x, y; };
    auto readPair = [gui](std::size_t physicalOffset, std::size_t scaledOffset,
                          Size& physical, Size& scaled) {
        const auto* bytes = static_cast<const std::byte*>(gui);
        physical = *reinterpret_cast<const Size*>(bytes + physicalOffset);
        scaled = *reinterpret_cast<const Size*>(bytes + scaledOffset);
        if (!std::isfinite(physical.x) || !std::isfinite(physical.y)
            || !std::isfinite(scaled.x) || !std::isfinite(scaled.y)) return false;
        if (physical.x < 64.0f || physical.y < 64.0f
            || physical.x > 20000.0f || physical.y > 20000.0f
            || scaled.x < 32.0f || scaled.y < 32.0f
            || scaled.x > physical.x * 1.05f || scaled.y > physical.y * 1.05f) return false;
        const float sx = scaled.x / physical.x;
        const float sy = scaled.y / physical.y;
        return sx >= 0.05f && sx <= 1.05f && sy >= 0.05f && sy <= 1.05f
            && std::abs(sx - sy) <= 0.08f;
    };

    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    auto score = [&viewport](const Size& physical) {
        if (viewport[2] <= 0 || viewport[3] <= 0) return 0.0f;
        const float width = static_cast<float>(viewport[2]);
        const float height = static_cast<float>(viewport[3]);
        const float aspectError = std::abs(physical.x / physical.y - width / height);
        return std::abs(physical.x - width) / width
            + std::abs(physical.y - height) / height + aspectError;
    };

    Size modernPhysical{}, modernScaled{}, legacyPhysical{}, legacyScaled{};
    const bool modernValid = readPair(worldanalysis::sdk::offsets::GuiData::mScreenSize,
                                      worldanalysis::sdk::offsets::GuiData::mScreenSizeScaled,
                                      modernPhysical, modernScaled);
    const bool legacyValid = readPair(worldanalysis::sdk::offsets::GuiData::mLegacyScreenSize,
                                      worldanalysis::sdk::offsets::GuiData::mLegacyScreenSizeScaled,
                                      legacyPhysical, legacyScaled);
    if (!modernValid && !legacyValid) return false;
    Size physical{}, scaled{};
    if (modernValid && (!legacyValid || score(modernPhysical) <= score(legacyPhysical))) {
        physical = modernPhysical; scaled = modernScaled;
    } else {
        physical = legacyPhysical; scaled = legacyScaled;
    }
    physicalWidth = physical.x; physicalHeight = physical.y;
    uiWidth = scaled.x; uiHeight = scaled.y;
    return true;
}

bool physicalToUi(void* context, float& x, float& y) {
    if (!context || !std::isfinite(x) || !std::isfinite(y)) return false;
    float physicalWidth=0.0f, physicalHeight=0.0f, uiWidth=0.0f, uiHeight=0.0f;
    if (!viewportSize(context, physicalWidth, physicalHeight, uiWidth, uiHeight)) return false;
    x *= uiWidth / physicalWidth;
    y *= uiHeight / physicalHeight;
    return std::isfinite(x) && std::isfinite(y);
}

} // namespace worldanalysis::ui
