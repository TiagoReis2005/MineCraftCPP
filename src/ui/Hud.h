#pragma once

#include <cstdint>

namespace mc {

class UIRenderer;
class Font;

struct HudTextures {
    int hotbar = 0;    // hotbar.png (182x22)
    int selection = 0; // hotbar_selection.png (24x23)
};

// Emits the in-game HUD (Minecraft hotbar sprite + selection + inverting crosshair).
class Hud {
public:
    // slotIcons[i] is the UI texture id of the block icon in slot i (or < 0 if empty).
    // showHotbar = false draws only the crosshair (spectator hides the bar until scrolled);
    // showCrosshair = false drops the crosshair too (a screen like the inventory is open).
    // font + slotCounts (optional) draw stack counts over the icons, vanilla style.
    void build(UIRenderer& ui, uint32_t screenW, uint32_t screenH, int selectedSlot,
               const HudTextures& tex, const int* slotIcons, int slotCount,
               bool showHotbar = true, bool showCrosshair = true,
               const Font* font = nullptr, const int* slotCounts = nullptr);
};

} // namespace mc
