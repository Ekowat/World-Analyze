#pragma once

#include <cstdint>
#include <string>

namespace worldanalysis::ui {

using OverlayRenderer = void (*)(void* context);

bool ensureInstalled();
void addRenderer(OverlayRenderer renderer);

void fillRect(void* context, float x0, float y0, float x1, float y1, std::uint32_t argb, float alphaScale = 1.0f);
bool drawText(void* context, const std::string& text, float x0, float y0, float x1, float y1,
              std::uint32_t argb, float fontSize = 1.0f, bool centered = false);
void flushImages(void* context);
void* clientInstance(void* context);
void* screenContext(void* context);
// Converts physical GL/window pixels to Minecraft's GUI-scaled coordinate
// space.  The native inventory/creative item renderer requires this space.
bool viewportSize(void* context, float& physicalWidth, float& physicalHeight, float& uiWidth, float& uiHeight);
bool physicalToUi(void* context, float& x, float& y);

} // namespace worldanalysis::ui
