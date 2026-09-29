#pragma once

#include <cstddef>

namespace worldanalysis::sdk::offsets {

namespace VTable {
inline constexpr std::size_t ClientInstance_getRegion = 31;
// Current Bedrock ClientInstance layout: getRegion remains slot 31 and
// getCamera is 174 virtual entries later, yielding slot 205.
inline constexpr std::size_t ClientInstance_getCamera = 205;
// dot52: primary vtable slot 236, accessor 0x9818A8C loads this + 0x5C0.
inline constexpr std::size_t ClientInstance_getGuiData = 236;
// dot52: slot 19 forwards to Dimension slot 3; slot 18 is a region query.
inline constexpr std::size_t BlockSource_getDimensionId = 19;
// Minecraft 26.45 Level::getTime() is virtual slot 133 (byte offset 0x428).
// The exact target contains multiple call sites that invoke this slot and
// immediately normalize the returned integer against the 24,000-tick day.
inline constexpr std::size_t Level_getTime = 133;
inline constexpr std::size_t RenderMaterialGroup_getMaterial = 2;
inline constexpr std::size_t HoverTextRendererRenderHoverBox = 17;
inline constexpr std::size_t MinecraftUIRenderContextGetLineLength = 2;
inline constexpr std::size_t MinecraftUIRenderContextDrawText = 6;
inline constexpr std::size_t MinecraftUIRenderContextFlushText = 7;
inline constexpr std::size_t MinecraftUIRenderContextDrawImage = 8;
inline constexpr std::size_t MinecraftUIRenderContextFlushImages = 10;
inline constexpr std::size_t MinecraftUIRenderContextFillRectangle = 16;
inline constexpr std::size_t MinecraftUIRenderContextGetTexture = 32;
inline constexpr std::size_t ClientInstanceGetMinecraftGame = 83;
inline constexpr std::size_t ItemGetMaxDamage = 37;
}

namespace ClientInstance {
inline constexpr std::size_t mLevelRenderer = 0x1A0;
}

namespace GuiData {
// Minecraft 26.45 keeps the physical and GUI-scaled viewport sizes in the
// modern GuiData layout.  UiOverlay also validates the older adjacent layout
// at runtime before using either pair, so a bad/stale pointer is never trusted.
inline constexpr std::size_t mScreenSize = 0x40;
inline constexpr std::size_t mScreenSizeScaled = 0x50;
inline constexpr std::size_t mLegacyScreenSize = 0x30;
inline constexpr std::size_t mLegacyScreenSizeScaled = 0x40;
}

}
