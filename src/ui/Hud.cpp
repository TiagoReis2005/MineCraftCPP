#include "ui/Hud.h"

#include "gfx/UIRenderer.h"
#include "ui/Font.h"

#include <string>

namespace mc {

void Hud::build(UIRenderer& ui, uint32_t screenW, uint32_t screenH, int selectedSlot,
                const HudTextures& tex, const int* slotIcons, int slotCount,
                bool showHotbar, bool showCrosshair, const Font* font, const int* slotCounts) {
    float W = static_cast<float>(screenW);
    float H = static_cast<float>(screenH);
    float cx = W * 0.5f;
    float cy = H * 0.5f;

    if (showHotbar) {
        // Hotbar: the 182x22 sprite has 9 slots (20px each, first at local x=1), scaled up.
        const float scale = 3.0f;
        float barW = 182.0f * scale;
        float barH = 22.0f * scale;
        float barX = (W - barW) * 0.5f;
        float barY = H - barH - 8.0f;
        ui.sprite(tex.hotbar, barX, barY, barW, barH);

        // Block icons inside each slot (16px content, 2px inset within the 20px slot).
        for (int i = 0; i < slotCount && i < 9; ++i) {
            if (slotIcons[i] < 0) continue;
            float ix = barX + (3.0f + 20.0f * i) * scale;
            float iy = barY + 3.0f * scale;
            ui.sprite(slotIcons[i], ix, iy, 16.0f * scale, 16.0f * scale);
        }

        // Stack counts, bottom-right of each icon like Minecraft.
        if (font && slotCounts) {
            for (int i = 0; i < slotCount && i < 9; ++i) {
                if (slotCounts[i] <= 1 || slotIcons[i] < 0) continue;
                std::string txt = std::to_string(slotCounts[i]);
                float ix = barX + (3.0f + 20.0f * i) * scale;
                float iy = barY + 3.0f * scale;
                float tx = ix + 17.0f * scale - font->textWidth(txt, scale);
                float ty = iy + 9.0f * scale;
                font->drawText(ui, txt, tx, ty, scale, glm::vec4(1.0f), /*shadow=*/true);
            }
        }

        // Selection, centered over the selected slot; top-left = (-1 + 20*i, -1). The
        // 24x23 sprite is drawn one px wider/taller (25x24) so it wraps the slot fully
        // on the right and bottom edges, like vanilla.
        if (selectedSlot >= 0 && selectedSlot < 9) {
            float selX = barX + (-1.0f + 20.0f * selectedSlot) * scale;
            float selY = barY + (-1.0f) * scale;
            ui.sprite(tex.selection, selX, selY, 25.0f * scale, 24.0f * scale);
        }
    }

    if (!showCrosshair) return;

    // Inverting crosshair (+): a horizontal bar plus two vertical bars that stop at the
    // horizontal one, so the center isn't inverted twice.
    float arm = 9.0f, thick = 2.0f;
    ui.invertQuad(cx - arm, cy - thick * 0.5f, arm * 2.0f, thick);               // horizontal
    ui.invertQuad(cx - thick * 0.5f, cy - arm, thick, arm - thick * 0.5f);        // top
    ui.invertQuad(cx - thick * 0.5f, cy + thick * 0.5f, thick, arm - thick * 0.5f); // bottom
}

} // namespace mc
