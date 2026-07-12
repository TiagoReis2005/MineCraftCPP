#pragma once

#include "game/Inventory.h"
#include "world/Block.h"

#include <cstdint>
#include <vector>

namespace mc {

class UIRenderer;
class Font;
class BlockRegistry;
class ItemIconRenderer;
class RecipeBook;
class Lang;

// The E inventory screen.
// Creative: the vanilla tab_items.png panel with category tabs (Building/Nature/
//   Functional); clicking a catalog block picks a full stack, hotbar slots place/lift,
//   hover + 1-9 assigns, right-click clears a slot.
// Survival: the vanilla inventory.png panel; 27 storage slots + the hotbar, with real
//   stack moving (click = pick/place/merge/swap, right-click = place one).
class InventoryScreen {
public:
    // Which screen the E key / a container block opened.
    enum class Mode {
        Creative, // tabbed catalog + inventory panel
        Survival, // inventory.png: player box, 2x2 crafting, storage + hotbar
        Crafting, // crafting_table.png: 3x3 crafting, storage + hotbar
    };

    struct Input {
        float mouseX = 0.0f, mouseY = 0.0f; // framebuffer pixels
        bool leftPressed = false;           // edge: went down this frame
        bool leftDown = false;              // level: split-drag / scrollbar
        bool rightPressed = false;          // edge
        bool rightDown = false;             // level: one-per-slot right drag
        bool middlePressed = false;         // edge: creative catalog stack pick
        bool dropPressed = false;           // edge: Q drops from the hovered slot
        bool shiftDown = false;             // level: quick-move modifier
        double scroll = 0.0;                // wheel notches (positive = up)
        double time = 0.0;                  // seconds (double-click detection)
    };

    // UI texture ids (0 = missing -> flat-quad fallback for that element).
    struct Textures {
        int creativePanel = 0;                // creative_inventory/tab_items.png (catalog)
        int creativeInventoryPanel = 0;       // creative_inventory/tab_inventory.png
        int survivalPanel = 0;                // container/inventory.png
        int craftingPanel = 0;                // container/crafting_table.png
        int tabSelected[kTabCount] = {};      // sprites/.../tab_top_selected_N.png
        int tabUnselected[kTabCount] = {};    // sprites/.../tab_top_unselected_N.png
        int scroller = 0, scrollerDisabled = 0;
        int playerPreview = 0;          // offscreen player render for the panel's player box
        int tooltipBg = 0, tooltipFrame = 0; // sprites/tooltip nine-slice (0 = flat fallback)
    };

    void init(const Textures& tex, const RecipeBook* recipes, const Lang* lang) {
        tex_ = tex;
        recipes_ = recipes;
        lang_ = lang;
    }

    bool open() const { return open_; }
    // Closing hands the carried stack AND the crafting grid back to the inventory so
    // items don't vanish.
    void setOpen(bool open, Inventory* inv = nullptr);

    // Header shown on the survival panel (the user's edited top band): the player name,
    // or "Inventory" when empty.
    void setHeaderTitle(std::string title) { headerTitle_ = std::move(title); }

    void build(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
               const ItemIconRenderer& icons, Inventory& inv,
               uint32_t screenW, uint32_t screenH, const Input& in, Mode mode);

    // Catalog block under the cursor after the last build (creative only; AIR if none).
    BlockId hoveredBlock() const { return hovered_; }

    // Cursor-look for the player preview (valid when a panel drew the preview box this
    // build): angles the model uses to watch the mouse, vanilla style.
    bool previewLookValid() const { return previewLookValid_; }
    float previewLookYawDeg() const { return previewLookYaw_; }
    float previewLookPitchDeg() const { return previewLookPitch_; }

    // Stacks Q-dropped from slots this frame; the caller throws them into the world.
    std::vector<ItemStack> takeDrops() {
        std::vector<ItemStack> out;
        out.swap(pendingDrops_);
        return out;
    }

private:
    Textures tex_{};
    bool open_ = false;
    ItemStack held_{};   // stack carried on the cursor
    int scrollRow_ = 0;
    int tab_ = 0;        // active creative tab (kTab*)
    bool draggingBar_ = false;
    BlockId hovered_ = BLOCK_AIR;
    double lastShiftClick_ = -1.0;  // shift+LMB double-click = mass quick-move
    double lastDestroyClick_ = -1.0; // shift+double-click the X = wipe the inventory
    double lastLeftTime_ = -1.0;    // plain LMB double-click on the SAME slot while
    int lastLeftCell_ = -1;         // carrying = collect same-id stacks onto the cursor
    bool dragLeft_ = false;         // LMB drag while carrying: split on release
    std::vector<int> dragCells_;    // slots visited this left drag (in order)
    int dragPreviewRemaining_ = -1; // cursor count shown while the split preview is live
    bool dragRight_ = false;        // RMB drag while carrying: one item per slot
    std::vector<int> rightVisited_;
    std::vector<ItemStack> pendingDrops_; // Q drops queued for the world
    bool previewLookValid_ = false;       // preview drawn this build (cursor-look active)
    float previewLookYaw_ = 0.0f;
    float previewLookPitch_ = 0.0f;

    // Localized block name for tooltips: "block.<name>" key, pretty name fallback.
    const Lang* lang_ = nullptr;
    std::string blockLabel(const BlockRegistry& reg, BlockId id) const;
    std::string headerTitle_; // survival panel header (player name / "Inventory")

    // Crafting: grid slots (2x2 uses [0..3], 3x3 all nine) + the matched result.
    const RecipeBook* recipes_ = nullptr;
    ItemStack craft_[9]{};
    ItemStack craftResult_{};
    double lastResultClick_ = -1.0; // double-click the result = craft-all onto cursor

    // Slot interaction shared by every panel (cell: 0..8 hotbar, 9+ storage, 100+
    // crafting grid, -1 = not over a slot; drags commit here too). `outside` = the click
    // landed off the panel entirely (drops the carried stack into the world).
    void handleSlotClicks(Inventory& inv, int cell, const Input& in, bool outside);

    void buildCreative(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
                       const ItemIconRenderer& icons, Inventory& inv,
                       uint32_t screenW, uint32_t screenH, const Input& in);
    // The survival inventory (2x2 crafting) and the crafting table (3x3) share this:
    // same storage/hotbar layout, different upper half.
    void buildPanel(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
                    const ItemIconRenderer& icons, Inventory& inv,
                    uint32_t screenW, uint32_t screenH, const Input& in, bool craftingTable);
};

} // namespace mc
