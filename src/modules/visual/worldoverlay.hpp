#pragma once

#include <worldanalysis/sdk/Types.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace worldanalysis::worldoverlay {

using Vec3 = worldanalysis::sdk::Vec3;

struct Segment {
    Vec3 a{};
    Vec3 b{};
};

struct Quad {
    Vec3 a{};
    Vec3 b{};
    Vec3 c{};
    Vec3 d{};
};

struct RenderContext {
    void* screen = nullptr;
    void* tessellator = nullptr;
    void* rendererPlayer = nullptr;
    Vec3 camera{};
};

using RenderCallback = void (*)(void* levelRenderer, void* screenContext, void* a3);

// Scoped per-module color theming. The default foreground (#FFFFFF) and
// background (#000000, opacity 1) preserve legacy colors. Any other foreground
// RGB tints non-black geometry/text while the background RGB/opacity applies to
// black translucent fills.
class ColorScope {
public:
    explicit ColorScope(std::string_view foreground);
    ColorScope(std::string_view foreground, std::string_view background, float backgroundOpacity = 1.0f);
    ~ColorScope();
    ColorScope(const ColorScope&) = delete;
    ColorScope& operator=(const ColorScope&) = delete;
private:
    std::uint32_t mPrevForeground = 0;
    std::uint32_t mPrevBackground = 0;
    float mPrevBackgroundOpacity = 1.0f;
    bool mPrevForegroundEnabled = false;
    bool mPrevBackgroundEnabled = false;
};

std::uint32_t parseColor(std::string_view value, std::uint32_t fallback = 0xFFFFFFFFu);
std::uint32_t applyThemeColor(std::uint32_t argb);

// One RenderLevel hook fans out to every World Analysis world overlay that
// registers here. This avoids each module competing for the same native entry.
bool registerRenderCallback(RenderCallback callback);

bool initialize();
bool makeContext(void* levelRenderer, void* screenContext, RenderContext& out);
void* material(std::string_view name);
void* throughWallMaterial();
void* depthMaterial();

void drawLines(const RenderContext& ctx, void* mat, const std::vector<Segment>& lines,
               std::uint32_t argb, float thickness = 1.0f);
void drawQuads(const RenderContext& ctx, void* mat, const std::vector<Quad>& quads,
               std::uint32_t argb);

float billboardTextWidth(std::string_view text, float pixelSize);
float billboardTextHeight(float pixelSize);
float fitBillboardTextPixelSize(std::string_view text, float maxWidth,
                                float preferredPixelSize, float minimumPixelSize = 0.006f);
void billboardBasis(const Vec3& center, const Vec3& camera, Vec3& right, Vec3& up);
void drawBillboardTextOriented(const RenderContext& ctx, void* mat, std::string_view text,
                               const Vec3& center, const Vec3& right, const Vec3& up,
                               float pixelSize, std::uint32_t argb, bool background = true,
                               std::uint32_t backgroundArgb = 0x8A000000u);
void drawBillboardText(const RenderContext& ctx, void* mat, std::string_view text,
                       const Vec3& center, float pixelSize, std::uint32_t argb,
                       bool background = true, std::uint32_t backgroundArgb = 0x8A000000u);
void drawBillboardSquare(const RenderContext& ctx, void* mat, const Vec3& center,
                         float size, std::uint32_t argb, float thickness = 1.0f,
                         bool fill = false, std::uint32_t fillArgb = 0x30000000u);

std::string sanitizeText(std::string_view input, std::size_t maxChars = 28);
std::string actorTypeName(void* actor);
bool plausible(std::uintptr_t ptr, std::size_t alignment = alignof(void*));

} // namespace worldanalysis::worldoverlay
