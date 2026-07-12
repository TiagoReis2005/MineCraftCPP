#include "ui/InventoryScreen.h"

#include "core/Lang.h"
#include "game/Recipes.h"
#include "gfx/UIRenderer.h"
#include "ui/Font.h"
#include "ui/ItemIconRenderer.h"
#include "world/BlockRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace mc {
namespace {

constexpr float kScale = 3.0f;   // GUI px -> screen px (matches the hotbar)
constexpr float kSheet = 256.0f; // texture sheet size
constexpr int kSlot = 18;        // slot stride (16px item + 1px border each side)

// Creative screen: the (edited) catalog panel and inventory panel side by side.
// All positions pixel-measured from the user's edited PNGs.
constexpr int kGuiW = 195, kGuiH = 168;
constexpr int kPanelGap = 4;           // GUI px between the two panels
constexpr int kCols = 9, kRows = 8;    // catalog grid (edited png: 8 rows, no hotbar)
constexpr int kGridX = 9, kGridY = 18;
constexpr int kBarX = 175, kBarY = 18;
constexpr int kBarW = 12, kBarH = 142, kThumbH = 15;
constexpr int kTabW = 26, kTabH = 28;  // visible tab area above the panel
constexpr int kTabStride = 27;         // vanilla tab spacing
constexpr int kTabSprH = 32;           // sprite height (bottom 4px overlap the panel)
constexpr int kMainRows = Inventory::kMainSlots / 9; // storage rows (4)
// Inventory panel (edited tab_inventory.png): 4 storage rows + hotbar + destroy slot.
constexpr int kInvPanelW = 195, kInvPanelH = 154;
constexpr int kInvGridX = 9, kInvGridY = 54; // storage item origin
constexpr int kInvHotbarY = 130;
constexpr int kDestroyX = 173, kDestroyY = 130; // X slot: trashes the carried stack

// Survival panel (edited inventory.png): a header band for the title, then 2x2 crafting
// + player box, 4 storage rows and the hotbar. The user added ~10px of header space at
// the top, so every interior sits 10px lower than the un-edited vanilla layout.
constexpr int kSurvW = 176, kSurvH = 194;
constexpr int kSurvGridX = 8, kSurvGridY = 94;
constexpr int kSurvHotbarY = 170;
constexpr int kSurvTitleX = 8, kSurvTitleY = 7; // "Inventory"/player name in the header

// Crafting table (unedited crafting_table.png): the original vanilla-height layout — it
// has NO header band, so its storage/hotbar sit 10px higher than the edited survival panel.
constexpr int kTableH = 184;
constexpr int kTableStorageY = 84;
constexpr int kTableHotbarY = 160;

// Crafting (measured from the PNGs): the survival panel's 2x2 grid + result, and the
// crafting table's 3x3 + result. Result interiors are 26px; the 16px icon centers.
constexpr int kSurvCraftX = 88, kSurvCraftY = 27;
constexpr int kSurvResultX = 148, kSurvResultY = 36;
constexpr int kTableGridX = 30, kTableGridY = 17;
constexpr int kTableResultX = 124, kTableResultY = 35;
// Cell encoding for handleSlotClicks: 0..8 hotbar, 9+ storage, kCraftCell0+i grid.
constexpr int kCraftCell0 = 100;

// Creative tab names: lang keys + English fallbacks.
const char* kTabKeys[kTabCount] = {"itemGroup.buildingBlocks", "itemGroup.natural",
                                   "itemGroup.functional"};
const char* kTabNames[kTabCount] = {"Building Blocks", "Nature", "Functional"};
const char* kTabIconBlock[kTabCount] = {"bricks", "grass", "oak_door"};

// "oak_fence_gate" -> "Oak Fence Gate" for tooltips.
std::string prettyName(const std::string& raw) {
    std::string out;
    bool up = true;
    for (char c : raw) {
        if (c == '_') {
            out += ' ';
            up = true;
        } else {
            out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            up = false;
        }
    }
    return out;
}

// Nine-slice a GUI sprite over (x,y,w,h): the corners stay `bd` screen-px, the four
// edges stretch along one axis and the center fills the rest. `buv` is the border as a
// fraction of the texture (mcmeta border / texture size).
void nineSlice(UIRenderer& ui, int tex, float x, float y, float w, float h, float bd,
               float buv) {
    float x0 = x, x1 = x + bd, x2 = x + w - bd, x3 = x + w;
    float y0 = y, y1 = y + bd, y2 = y + h - bd, y3 = y + h;
    float u0 = 0.0f, u1 = buv, u2 = 1.0f - buv, u3 = 1.0f;
    auto q = [&](float ax, float ay, float bx, float by, float au, float av, float bu, float bv) {
        ui.texQuad(tex, ax, ay, bx - ax, by - ay, au, av, bu, bv);
    };
    q(x0, y0, x1, y1, u0, u0, u1, u1); // corners
    q(x2, y0, x3, y1, u2, u0, u3, u1);
    q(x0, y2, x1, y3, u0, u2, u1, u3);
    q(x2, y2, x3, y3, u2, u2, u3, u3);
    q(x1, y0, x2, y1, u1, u0, u2, u1); // edges: top, bottom, left, right
    q(x1, y2, x2, y3, u1, u2, u2, u3);
    q(x0, y1, x1, y2, u0, u1, u1, u2);
    q(x2, y1, x3, y2, u2, u1, u3, u2);
    q(x1, y1, x2, y2, u1, u1, u2, u2); // center
}

void drawTooltip(UIRenderer& ui, const Font& font, const std::string& name, float mx, float my,
                 float W, float H, int bgTex, int frameTex) {
    float tw = font.textWidth(name, kScale);
    float th = font.lineHeight(kScale);
    // The frame art's border strip (bd) is drawn INSIDE the box, so the text needs its
    // own clearance past it — pad = border + breathing room, or the name hugs the frame.
    float bd = 4.0f * kScale;
    float pad = bd + 4.0f * kScale;
    float tx = std::clamp(mx + 12.0f * kScale, pad, std::max(pad, W - tw - pad));
    float ty = std::clamp(my - 12.0f * kScale, pad, std::max(pad, H - th - pad));
    float bx = tx - pad, by = ty - pad, bw = tw + pad * 2.0f, bh = th + pad * 2.0f;
    if (bgTex > 0 && frameTex > 0) {
        // Vanilla nine-slice tooltip (background fill + frame border over the same box).
        nineSlice(ui, bgTex, bx, by, bw, bh, bd, 9.0f / 100.0f);
        nineSlice(ui, frameTex, bx, by, bw, bh, bd, 10.0f / 100.0f);
    } else {
        // Flat fallback (no tooltip sprites present).
        glm::vec4 bg(16.0f / 255.0f, 0.0f, 16.0f / 255.0f, 0.94f);
        glm::vec4 border(80.0f / 255.0f, 0.0f, 1.0f, 0.55f);
        float bt = kScale;
        ui.quad(bx, by, bw, bh, bg);
        ui.quad(bx, by, bw, bt, border);
        ui.quad(bx, by + bh - bt, bw, bt, border);
        ui.quad(bx, by + bt, bt, bh - bt * 2.0f, border);
        ui.quad(bx + bw - bt, by + bt, bt, bh - bt * 2.0f, border);
    }
    font.drawText(ui, name, tx, ty, kScale, glm::vec4(1.0f), /*shadow=*/true);
}

// Stack count bottom-right of an icon (vanilla position), icon top-left at (ix, iy).
void drawCount(UIRenderer& ui, const Font& font, int count, float ix, float iy) {
    if (count <= 1) return;
    std::string txt = std::to_string(count);
    font.drawText(ui, txt, ix + 17.0f * kScale - font.textWidth(txt, kScale),
                  iy + 9.0f * kScale, kScale, glm::vec4(1.0f), /*shadow=*/true);
}

// Vanilla stack interaction: left click = pick/place/merge/swap.
void clickStack(ItemStack& slot, ItemStack& held) {
    if (held.empty()) {
        held = slot;
        slot = {};
    } else if (slot.empty()) {
        slot = held;
        held = {};
    } else if (slot.id == held.id && slot.count < Inventory::kMaxStack) {
        int take = std::min(held.count, Inventory::kMaxStack - slot.count);
        slot.count += take;
        held.count -= take;
        if (held.count <= 0) held = {};
    } else {
        std::swap(slot, held);
    }
}

// Right click: place one item from the carried stack.
void placeOne(ItemStack& slot, ItemStack& held) {
    if (held.empty()) return;
    if (slot.empty() || (slot.id == held.id && slot.count < Inventory::kMaxStack)) {
        slot.id = held.id;
        slot.count += 1;
        if (--held.count <= 0) held = {};
    }
}

// Right click with an empty cursor: pick up half the stack (rounded up).
void pickHalf(ItemStack& slot, ItemStack& held) {
    if (slot.empty()) return;
    int take = (slot.count + 1) / 2;
    held = {slot.id, take};
    slot.count -= take;
    if (slot.count <= 0) slot = {};
}

// Adds `count` of `id` into a slot range (top-ups first, then empties); returns leftover.
int addToRange(ItemStack* range, int n, BlockId id, int count) {
    for (int i = 0; i < n && count > 0; ++i) {
        ItemStack& s = range[i];
        if (s.id == id && s.count > 0 && s.count < Inventory::kMaxStack) {
            int take = std::min(count, Inventory::kMaxStack - s.count);
            s.count += take;
            count -= take;
        }
    }
    for (int i = 0; i < n && count > 0; ++i) {
        ItemStack& s = range[i];
        if (s.empty()) {
            int take = std::min(count, Inventory::kMaxStack);
            s = {id, take};
            count -= take;
        }
    }
    return count;
}

// Shift+LMB: move the stack to the other section (hotbar <-> storage).
void quickMove(Inventory& inv, int cell) {
    ItemStack& s = cell < 9 ? inv.slots[static_cast<size_t>(cell)]
                            : inv.main[static_cast<size_t>(cell - 9)];
    if (s.empty()) return;
    int leftover = cell < 9
                       ? addToRange(inv.main.data(), Inventory::kMainSlots, s.id, s.count)
                       : addToRange(inv.slots.data(), Inventory::kSlots, s.id, s.count);
    s.count = leftover;
    if (leftover <= 0) s = {};
}

// Plain LMB double-click while carrying: sweep every stack of the carried item from
// the whole screen — inventory AND the crafting grid — onto the cursor (up to one
// full stack), so crafting leftovers gather in one gesture.
void collectAll(Inventory& inv, ItemStack& held, ItemStack* extra, int extraN) {
    if (held.empty()) return;
    auto sweep = [&](ItemStack* range, int n) {
        for (int i = 0; i < n && held.count < Inventory::kMaxStack; ++i) {
            ItemStack& s = range[i];
            if (s.id != held.id || s.count <= 0) continue;
            int take = std::min(s.count, Inventory::kMaxStack - held.count);
            held.count += take;
            s.count -= take;
            if (s.count <= 0) s = {};
        }
    };
    sweep(inv.slots.data(), Inventory::kSlots);
    sweep(inv.main.data(), Inventory::kMainSlots);
    if (extra) sweep(extra, extraN);
}

// Shift+LMB double-click while carrying: every stack of that item in the hovered
// section (plus the carried one) quick-moves to the other section.
void massMove(Inventory& inv, ItemStack& held, bool fromHotbar) {
    if (held.empty()) return;
    BlockId id = held.id;
    ItemStack* src = fromHotbar ? inv.slots.data() : inv.main.data();
    int srcN = fromHotbar ? Inventory::kSlots : Inventory::kMainSlots;
    ItemStack* dst = fromHotbar ? inv.main.data() : inv.slots.data();
    int dstN = fromHotbar ? Inventory::kMainSlots : Inventory::kSlots;
    held.count = addToRange(dst, dstN, id, held.count);
    if (held.count <= 0) held = {};
    for (int i = 0; i < srcN; ++i) {
        ItemStack& s = src[i];
        if (s.id != id || s.count <= 0) continue;
        s.count = addToRange(dst, dstN, id, s.count);
        if (s.count <= 0) s = {};
    }
}

// LMB split-drag share for the slot at ordinal `i` when spreading `total` across `n`
// dragged slots: as even as possible with NOTHING left on the cursor. Each slot gets
// floor(total/n); the remainder (total%n) tops up the first slots by one, so the
// last-dragged slots take the smaller share. e.g. 3 over 3 -> 1/1/1; 31 over 16 ->
// fifteen 2s then one 1; 32 over 16 -> sixteen 2s.
int dragWant(int total, int n, int i) {
    if (n <= 0) return 0;
    int per = total / n;
    int rem = total % n;
    return per + (i < rem ? 1 : 0);
}

bool inRect(float mx, float my, float x, float y, float w, float h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

} // namespace

std::string InventoryScreen::blockLabel(const BlockRegistry& reg, BlockId id) const {
    const std::string& name = reg.block(BlockState(id)).name();
    std::string fallback = prettyName(name);
    return lang_ ? lang_->tr("block." + name, fallback) : fallback;
}

void InventoryScreen::handleSlotClicks(Inventory& inv, int cell, const Input& in, bool outside) {
    auto slotAt = [&](int i) -> ItemStack& {
        if (i >= kCraftCell0) return craft_[static_cast<size_t>(i - kCraftCell0)];
        return i < 9 ? inv.slots[static_cast<size_t>(i)]
                     : inv.main[static_cast<size_t>(i - 9)];
    };

    // Click off the panel while carrying: drop into the world (LMB the whole stack, like
    // shift+Q; RMB a single item). The world spawn is handled by takeDrops() in main.
    if (outside && !held_.empty()) {
        if (in.leftPressed) {
            pendingDrops_.push_back(held_);
            held_ = {};
            dragLeft_ = false;
            dragCells_.clear();
        } else if (in.rightPressed) {
            pendingDrops_.push_back({held_.id, 1});
            if (--held_.count <= 0) held_ = {};
        }
    }

    if (in.leftPressed && cell >= 0) {
        bool doubleClick = in.shiftDown && in.time - lastShiftClick_ < 0.4;
        if (in.shiftDown) lastShiftClick_ = in.time;
        // Plain double-click on the SAME slot: collect same-id stacks onto the cursor.
        bool collectClick = !in.shiftDown && !held_.empty() && cell == lastLeftCell_ &&
                            in.time - lastLeftTime_ < 0.4;
        lastLeftCell_ = cell;
        lastLeftTime_ = in.time;
        if (doubleClick && !held_.empty() && cell < kCraftCell0) {
            massMove(inv, held_, cell < 9);
        } else if (in.shiftDown) {
            if (cell >= kCraftCell0) {
                // Shift-click a crafting cell: dump it back into the inventory.
                ItemStack& s = slotAt(cell);
                if (!s.empty()) {
                    int leftover = inv.add(s.id, s.count);
                    s.count = leftover;
                    if (leftover <= 0) s = {};
                }
            } else {
                quickMove(inv, cell);
            }
        } else if (collectClick) {
            collectAll(inv, held_, craft_, 9);
            dragLeft_ = false;
            dragCells_.clear();
        } else if (!held_.empty()) {
            ItemStack& s = slotAt(cell);
            if (!s.empty() && s.id != held_.id) {
                clickStack(s, held_); // different item: plain swap
            } else {
                dragLeft_ = true; // place-or-split; commits on release
                dragCells_.assign(1, cell);
            }
        } else {
            clickStack(slotAt(cell), held_); // pick the whole stack up
        }
    }
    // Left drag while carrying: gather compatible slots; on release one slot places the
    // whole stack, several slots split it as evenly as possible (dragWant). The set is
    // capped at `count` slots so every slot still receives at least one item — e.g. 3
    // items spread across 3 slots (1 each), not stuck at 2.
    if (dragLeft_ && in.leftDown && cell >= 0 && !held_.empty()) {
        ItemStack& s = slotAt(cell);
        if ((s.empty() || s.id == held_.id) &&
            static_cast<int>(dragCells_.size()) < held_.count &&
            std::find(dragCells_.begin(), dragCells_.end(), cell) == dragCells_.end()) {
            dragCells_.push_back(cell);
        }
    }
    if (dragLeft_ && !in.leftDown) {
        if (!held_.empty() && !dragCells_.empty()) {
            if (dragCells_.size() == 1) {
                clickStack(slotAt(dragCells_[0]), held_);
            } else {
                int total = held_.count, n = static_cast<int>(dragCells_.size());
                int i = 0;
                for (int c : dragCells_) {
                    if (held_.empty()) break;
                    ItemStack& s = slotAt(c);
                    int space = Inventory::kMaxStack - (s.empty() ? 0 : s.count);
                    int amount = std::min(std::min(dragWant(total, n, i), held_.count), space);
                    if (amount > 0) {
                        s.id = held_.id;
                        s.count += amount;
                        held_.count -= amount;
                    }
                    ++i;
                }
                if (held_.count <= 0) held_ = {};
            }
        }
        dragLeft_ = false;
        dragCells_.clear();
    }

    // Right click: empty cursor picks half; carrying places one, and dragging keeps
    // placing one into each new slot passed over.
    if (in.rightPressed && cell >= 0) {
        if (held_.empty()) {
            pickHalf(slotAt(cell), held_);
        } else {
            placeOne(slotAt(cell), held_);
            dragRight_ = true;
            rightVisited_.assign(1, cell);
        }
    } else if (dragRight_ && in.rightDown && cell >= 0 && !held_.empty() &&
               std::find(rightVisited_.begin(), rightVisited_.end(), cell) ==
                   rightVisited_.end()) {
        placeOne(slotAt(cell), held_);
        rightVisited_.push_back(cell);
    }
    if (!in.rightDown) {
        dragRight_ = false;
        rightVisited_.clear();
    }

    // Q over a slot: toss one item (shift: the whole stack) out into the world.
    if (in.dropPressed && cell >= 0) {
        ItemStack& s = slotAt(cell);
        if (!s.empty()) {
            int n = in.shiftDown ? s.count : 1;
            pendingDrops_.push_back({s.id, n});
            s.count -= n;
            if (s.count <= 0) s = {};
        }
    }
}

void InventoryScreen::setOpen(bool open, Inventory* inv) {
    if (!open && inv) {
        // On close, the carried stack and the crafting grid go back into the bag; add()
        // tops up matching stacks then fills empties, so a partial stack in hand merges
        // into what's already there. Whatever doesn't fit drops into the world (vanilla).
        auto returnOrDrop = [&](const ItemStack& s) {
            if (s.empty()) return;
            int leftover = inv->add(s.id, s.count);
            if (leftover > 0) pendingDrops_.push_back({s.id, leftover});
        };
        returnOrDrop(held_);
        for (ItemStack& s : craft_) {
            returnOrDrop(s);
            s = {};
        }
    }
    if (!open) {
        for (ItemStack& s : craft_) s = {}; // no inventory to return to: clear anyway
    }
    open_ = open;
    held_ = {};
    craftResult_ = {};
    hovered_ = BLOCK_AIR;
    draggingBar_ = false;
}

void InventoryScreen::build(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
                            const ItemIconRenderer& icons, Inventory& inv,
                            uint32_t screenW, uint32_t screenH, const Input& in, Mode mode) {
    if (!open_) return;
    hovered_ = BLOCK_AIR;
    dragPreviewRemaining_ = -1;
    previewLookValid_ = false;
    ui.quad(0.0f, 0.0f, static_cast<float>(screenW), static_cast<float>(screenH),
            glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
    switch (mode) {
        case Mode::Creative:
            buildCreative(ui, font, reg, icons, inv, screenW, screenH, in);
            break;
        case Mode::Survival:
            buildPanel(ui, font, reg, icons, inv, screenW, screenH, in, false);
            break;
        case Mode::Crafting:
            buildPanel(ui, font, reg, icons, inv, screenW, screenH, in, true);
            break;
    }

    // The carried stack rides the cursor, over everything (while split-dragging, the
    // count shows what would remain after the spread).
    if (!held_.empty()) {
        int tex = icons.iconTexId(held_.id);
        if (tex >= 0) {
            float hx = in.mouseX - 8.0f * kScale, hy = in.mouseY - 8.0f * kScale;
            ui.sprite(tex, hx, hy, 16.0f * kScale, 16.0f * kScale);
            int shown = dragLeft_ && dragPreviewRemaining_ >= 0 ? dragPreviewRemaining_
                                                                : held_.count;
            drawCount(ui, font, shown, hx, hy);
        }
    }
}

void InventoryScreen::buildCreative(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
                                    const ItemIconRenderer& icons, Inventory& inv,
                                    uint32_t screenW, uint32_t screenH, const Input& in) {
    // The active tab's blocks, in registration order.
    std::vector<BlockId> catalog;
    for (BlockId id = 1; id < reg.count(); ++id) {
        const Block::Properties& p = reg.block(id).props();
        if (!p.hiddenFromHotbar && p.tab == tab_) catalog.push_back(id);
    }
    int totalRows = (static_cast<int>(catalog.size()) + kCols - 1) / kCols;
    int maxRow = std::max(0, totalRows - kRows);

    // Both panels side by side, centered as a group; tabs sit above the catalog.
    float W = static_cast<float>(screenW), H = static_cast<float>(screenH);
    float groupW = (kGuiW * 2 + kPanelGap) * kScale;
    float px = std::floor((W - groupW) * 0.5f);
    float py = std::floor((H - kGuiH * kScale) * 0.5f);
    float pxR = px + (kGuiW + kPanelGap) * kScale;
    auto gx = [&](float x) { return px + x * kScale; };
    auto gy = [&](float y) { return py + y * kScale; };
    auto rx = [&](float x) { return pxR + x * kScale; };
    float lx = (in.mouseX - px) / kScale;   // catalog-panel local GUI px
    float ly = (in.mouseY - py) / kScale;
    float rlx = (in.mouseX - pxR) / kScale; // inventory-panel local GUI px
    float rly = ly;

    // ---- Tabs above the catalog: click switches the page.
    int tabHit = -1;
    if (lx >= 0 && ly >= -kTabH && ly < 0) {
        int t = static_cast<int>(lx) / kTabStride;
        if (t >= 0 && t < kTabCount &&
            lx - static_cast<float>(t * kTabStride) < static_cast<float>(kTabW)) {
            tabHit = t;
        }
    }
    if (in.leftPressed && tabHit >= 0) {
        tab_ = tabHit;
        scrollRow_ = 0;
        return buildCreative(ui, font, reg, icons, inv, screenW, screenH,
                             Input{in.mouseX, in.mouseY, false, in.leftDown, false, 0.0});
    }

    // ---- Scrolling (only while hovering the catalog panel, like vanilla).
    bool overCatalog = lx >= 0.0f && lx < static_cast<float>(kGuiW) && ly >= 0.0f &&
                       ly < static_cast<float>(kGuiH);
    if (in.scroll != 0.0 && overCatalog) scrollRow_ -= in.scroll > 0.0 ? 1 : -1;
    if (in.leftPressed && lx >= kBarX && lx < kBarX + kBarW + 2 && ly >= kBarY &&
        ly < kBarY + kBarH) {
        draggingBar_ = true;
    }
    if (!in.leftDown) draggingBar_ = false;
    if (draggingBar_ && maxRow > 0) {
        float frac = (ly - kBarY - kThumbH * 0.5f) / static_cast<float>(kBarH - kThumbH);
        scrollRow_ = static_cast<int>(std::lround(frac * maxRow));
    }
    scrollRow_ = std::clamp(scrollRow_, 0, maxRow);

    // ---- Hit tests (18x18 slot boxes).
    int catCell = -1;
    {
        float cx = lx - (kGridX - 1), cy = ly - (kGridY - 1);
        if (cx >= 0.0f && cy >= 0.0f) {
            int col = static_cast<int>(cx) / kSlot, row = static_cast<int>(cy) / kSlot;
            if (col < kCols && row < kRows) catCell = row * kCols + col;
        }
    }
    // Inventory panel: 0..8 hotbar, 9..35 storage; destroy slot separate.
    int rcell = -1;
    bool destroyHover = false;
    {
        float cx = rlx - (kInvGridX - 1);
        float sy = rly - (kInvGridY - 1);
        float hy = rly - (kInvHotbarY - 1);
        if (cx >= 0.0f) {
            int col = static_cast<int>(cx) / kSlot;
            if (col < 9) {
                if (sy >= 0.0f && sy < static_cast<float>(kMainRows * kSlot)) {
                    rcell = 9 + (static_cast<int>(sy) / kSlot) * 9 + col;
                } else if (hy >= 0.0f && hy < static_cast<float>(kSlot)) {
                    rcell = col;
                }
            }
        }
        destroyHover = rlx >= kDestroyX - 1 && rlx < kDestroyX + 17 &&
                       rly >= kDestroyY - 1 && rly < kDestroyY + 17;
    }
    auto slotAt = [&](int i) -> ItemStack& {
        return i < 9 ? inv.slots[static_cast<size_t>(i)]
                     : inv.main[static_cast<size_t>(i - 9)];
    };
    int catIdx = catCell >= 0 ? scrollRow_ * kCols + catCell : -1;
    hovered_ = (catIdx >= 0 && catIdx < static_cast<int>(catalog.size()))
                   ? catalog[static_cast<size_t>(catIdx)]
                   : BLOCK_AIR;

    // ---- Clicks. Catalog picks: LMB adds one item to the cursor, MMB grabs a full
    // stack, RMB half a stack; shift+LMB sends a stack straight to the inventory.
    // Carrying a DIFFERENT item: the click only discards it (vanilla creative trash);
    // the next click picks the new block.
    if (hovered_ != BLOCK_AIR) {
        bool carryingOther = !held_.empty() && held_.id != hovered_;
        if (in.leftPressed) {
            if (in.shiftDown) {
                inv.add(hovered_, Inventory::kMaxStack);
            } else if (carryingOther) {
                held_ = {};
            } else if (!held_.empty()) {
                held_.count = std::min(held_.count + 1, Inventory::kMaxStack);
            } else {
                held_ = {hovered_, 1};
            }
        }
        if (in.middlePressed) held_ = carryingOther ? ItemStack{} : ItemStack{hovered_, Inventory::kMaxStack};
        if (in.rightPressed) held_ = carryingOther ? ItemStack{} : ItemStack{hovered_, Inventory::kMaxStack / 2};
    } else if (in.leftPressed && !draggingBar_) {
        if (destroyHover) {
            held_ = {};
            // Shift + double-click the X wipes the whole inventory (armor/offhand/
            // pouch slots join once those exist).
            if (in.shiftDown && in.time - lastDestroyClick_ < 0.4) {
                inv.slots.fill({});
                inv.main.fill({});
            }
            lastDestroyClick_ = in.shiftDown ? in.time : -1.0;
        } else if (catCell >= 0) {
            held_ = {}; // clicked an empty catalog cell: discard
        }
    }
    if (in.rightPressed && destroyHover) held_ = {};
    // Outside = off both panels (tabs stick up above the catalog): a click there drops
    // the carried stack into the world.
    bool overPanel =
        inRect(in.mouseX, in.mouseY, px, py - kTabH * kScale, kGuiW * kScale,
               (kGuiH + kTabH) * kScale) ||
        inRect(in.mouseX, in.mouseY, pxR, py, kInvPanelW * kScale, kInvPanelH * kScale);
    handleSlotClicks(inv, rcell, in, !overPanel);

    // ---- Draw. Unselected tabs sit BEHIND the panel (vanilla), selected in front.
    for (int t = 0; t < kTabCount; ++t) {
        if (t == tab_) continue;
        float tx = gx(static_cast<float>(t * kTabStride));
        float ty = gy(static_cast<float>(-kTabH));
        if (tex_.tabUnselected[t] > 0) {
            ui.sprite(tex_.tabUnselected[t], tx, ty, kTabW * kScale, kTabSprH * kScale);
        } else {
            ui.quad(tx, ty, kTabW * kScale, kTabH * kScale, glm::vec4(0.42f, 0.42f, 0.42f, 1.0f));
        }
    }
    if (tex_.creativePanel > 0) {
        ui.texQuad(tex_.creativePanel, px, py, kGuiW * kScale, kGuiH * kScale,
                   0.0f, 0.0f, kGuiW / kSheet, kGuiH / kSheet);
    } else {
        ui.quad(px, py, kGuiW * kScale, kGuiH * kScale, glm::vec4(0.78f, 0.78f, 0.78f, 1.0f));
    }
    if (tex_.creativeInventoryPanel > 0) {
        ui.texQuad(tex_.creativeInventoryPanel, pxR, py, kInvPanelW * kScale, kInvPanelH * kScale,
                   0.0f, 0.0f, kInvPanelW / kSheet, kInvPanelH / kSheet);
    } else {
        ui.quad(pxR, py, kInvPanelW * kScale, kInvPanelH * kScale,
                glm::vec4(0.78f, 0.78f, 0.78f, 1.0f));
    }
    // Player model in the panel's preview window (measured box: 73,6 32x43); the 1:2
    // offscreen render is letterboxed inside so proportions hold.
    if (tex_.playerPreview > 0) {
        float bh = 43.0f * kScale, bw = bh * 0.5f;
        ui.sprite(tex_.playerPreview, pxR + (73.0f + 16.0f) * kScale - bw * 0.5f,
                  py + 6.0f * kScale, bw, bh);
        // Watch the cursor: angles from the model's eye area toward the mouse.
        float ax = pxR + 89.0f * kScale, ay = py + 15.0f * kScale;
        previewLookYaw_ = glm::degrees(std::atan((in.mouseX - ax) / 300.0f));
        previewLookPitch_ = glm::degrees(std::atan((in.mouseY - ay) / 300.0f));
        previewLookValid_ = true;
    }
    {
        float tx = gx(static_cast<float>(tab_ * kTabStride));
        float ty = gy(static_cast<float>(-kTabH));
        if (tex_.tabSelected[tab_] > 0) {
            ui.sprite(tex_.tabSelected[tab_], tx, ty, kTabW * kScale, kTabSprH * kScale);
        } else {
            ui.quad(tx, ty, kTabW * kScale, (kTabH + 4.0f) * kScale,
                    glm::vec4(198 / 255.0f, 198 / 255.0f, 198 / 255.0f, 1.0f));
        }
    }
    for (int t = 0; t < kTabCount; ++t) {
        BlockId rep = reg.byName(kTabIconBlock[t]);
        int tex = rep != BLOCK_AIR ? icons.iconTexId(rep) : -1;
        if (tex >= 0) {
            ui.sprite(tex, gx(static_cast<float>(t * kTabStride + 5)),
                      gy(static_cast<float>(-kTabH + 9)), 16.0f * kScale, 16.0f * kScale);
        }
    }
    std::string tabName = lang_ ? lang_->tr(kTabKeys[tab_], kTabNames[tab_]) : kTabNames[tab_];
    font.drawText(ui, tabName, gx(8.0f), gy(6.0f), kScale,
                  glm::vec4(0.25f, 0.25f, 0.25f, 1.0f), /*shadow=*/false);

    // Catalog icons.
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            int idx = (scrollRow_ + row) * kCols + col;
            if (idx >= static_cast<int>(catalog.size())) break;
            int tex = icons.iconTexId(catalog[static_cast<size_t>(idx)]);
            if (tex < 0) continue;
            ui.sprite(tex, gx(static_cast<float>(kGridX + col * kSlot)),
                      gy(static_cast<float>(kGridY + row * kSlot)), 16.0f * kScale, 16.0f * kScale);
        }
    }
    // Inventory panel contents (hotbar + storage).
    for (int i = 0; i < 9 + Inventory::kMainSlots; ++i) {
        const ItemStack& s = slotAt(i);
        if (s.empty()) continue;
        int tex = icons.iconTexId(s.id);
        if (tex < 0) continue;
        int col = i < 9 ? i : (i - 9) % 9;
        float ix = rx(static_cast<float>(kInvGridX + col * kSlot));
        float iy = i < 9 ? gy(static_cast<float>(kInvHotbarY))
                         : gy(static_cast<float>(kInvGridY + ((i - 9) / 9) * kSlot));
        ui.sprite(tex, ix, iy, 16.0f * kScale, 16.0f * kScale);
        drawCount(ui, font, s.count, ix, iy);
    }
    // Live split preview: only once the drag has entered a SECOND slot (a single-slot
    // press is a plain place-all, not a split). Dragged slots show their projected share.
    if (dragLeft_ && !held_.empty() && dragCells_.size() >= 2) {
        int remaining = held_.count, total = held_.count;
        int n = static_cast<int>(dragCells_.size()), i = 0;
        int ghost = icons.iconTexId(held_.id);
        for (int c : dragCells_) {
            const ItemStack& s = slotAt(c);
            int space = Inventory::kMaxStack - (s.empty() ? 0 : s.count);
            int amount = std::min(std::min(dragWant(total, n, i), remaining), space);
            ++i;
            if (amount <= 0) continue;
            remaining -= amount;
            int col = c < 9 ? c : (c - 9) % 9;
            float ix = rx(static_cast<float>(kInvGridX + col * kSlot));
            float iy = c < 9 ? gy(static_cast<float>(kInvHotbarY))
                             : gy(static_cast<float>(kInvGridY + ((c - 9) / 9) * kSlot));
            if (ghost >= 0) {
                ui.sprite(ghost, ix, iy, 16.0f * kScale, 16.0f * kScale,
                          glm::vec4(1.0f, 1.0f, 1.0f, 0.65f));
            }
            drawCount(ui, font, (s.empty() ? 0 : s.count) + amount, ix, iy);
        }
        dragPreviewRemaining_ = remaining;
    }

    // Scrollbar thumb.
    {
        float frac = maxRow > 0 ? static_cast<float>(scrollRow_) / static_cast<float>(maxRow) : 0.0f;
        float ty = kBarY + frac * static_cast<float>(kBarH - kThumbH);
        int sprite = maxRow > 0 ? tex_.scroller : tex_.scrollerDisabled;
        if (sprite > 0) {
            ui.sprite(sprite, gx(static_cast<float>(kBarX)), gy(ty), kBarW * kScale,
                      kThumbH * kScale);
        } else {
            ui.quad(gx(static_cast<float>(kBarX)), gy(ty), kBarW * kScale, kThumbH * kScale,
                    glm::vec4(0.45f, 0.45f, 0.45f, 1.0f));
        }
    }

    // Hover highlight + tooltip.
    BlockId tipBlock = BLOCK_AIR;
    if (catCell >= 0) {
        ui.quad(gx(static_cast<float>(kGridX + (catCell % kCols) * kSlot)),
                gy(static_cast<float>(kGridY + (catCell / kCols) * kSlot)),
                16.0f * kScale, 16.0f * kScale, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
        tipBlock = hovered_;
    } else if (rcell >= 0 || destroyHover) {
        int col = destroyHover ? 0 : (rcell < 9 ? rcell : (rcell - 9) % 9);
        float ix = destroyHover ? rx(static_cast<float>(kDestroyX))
                                : rx(static_cast<float>(kInvGridX + col * kSlot));
        float iy = destroyHover || rcell < 9
                       ? gy(static_cast<float>(kInvHotbarY))
                       : gy(static_cast<float>(kInvGridY + ((rcell - 9) / 9) * kSlot));
        ui.quad(ix, iy, 16.0f * kScale, 16.0f * kScale, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
        if (rcell >= 0) tipBlock = slotAt(rcell).id;
    }
    if (held_.empty() && tipBlock != BLOCK_AIR) {
        drawTooltip(ui, font, blockLabel(reg, tipBlock), in.mouseX, in.mouseY, W, H,
                    tex_.tooltipBg, tex_.tooltipFrame);
    }
}

void InventoryScreen::buildPanel(UIRenderer& ui, const Font& font, const BlockRegistry& reg,
                                 const ItemIconRenderer& icons, Inventory& inv,
                                 uint32_t screenW, uint32_t screenH, const Input& in,
                                 bool craftingTable) {
    // Both panels are 176 wide with the same storage/hotbar COLUMNS, but the edited
    // survival image is 10px taller (a header band) so its rows sit lower; the crafting
    // table keeps the original vanilla positions. Everything here uses the per-panel
    // height/rows so the two never drag each other around again.
    const int gridN = craftingTable ? 3 : 2;
    const int panelH = craftingTable ? kTableH : kSurvH;
    const int gridY = craftingTable ? kTableStorageY : kSurvGridY;
    const int hotbarY = craftingTable ? kTableHotbarY : kSurvHotbarY;
    const int craftX = craftingTable ? kTableGridX : kSurvCraftX;
    const int craftY = craftingTable ? kTableGridY : kSurvCraftY;
    const int resX = craftingTable ? kTableResultX : kSurvResultX;
    const int resY = craftingTable ? kTableResultY : kSurvResultY;

    float W = static_cast<float>(screenW), H = static_cast<float>(screenH);
    float px = std::floor((W - kSurvW * kScale) * 0.5f);
    float py = std::floor((H - panelH * kScale) * 0.5f);
    auto gx = [&](float x) { return px + x * kScale; };
    auto gy = [&](float y) { return py + y * kScale; };
    float lx = (in.mouseX - px) / kScale;
    float ly = (in.mouseY - py) / kScale;

    // Slot under the cursor: 0..8 = hotbar, 9+ = storage, kCraftCell0+ = crafting grid.
    int cell = -1;
    {
        float cx = lx - (kSurvGridX - 1);
        float sy = ly - (gridY - 1);
        float hy = ly - (hotbarY - 1);
        if (cx >= 0.0f) {
            int col = static_cast<int>(cx) / kSlot;
            if (col < 9) {
                if (sy >= 0.0f && sy < static_cast<float>(kMainRows * kSlot)) {
                    cell = 9 + (static_cast<int>(sy) / kSlot) * 9 + col;
                } else if (hy >= 0.0f && hy < static_cast<float>(kSlot)) {
                    cell = col;
                }
            }
        }
        float ccx = lx - (craftX - 1);
        float ccy = ly - (craftY - 1);
        if (cell < 0 && ccx >= 0.0f && ccy >= 0.0f) {
            int col = static_cast<int>(ccx) / kSlot, row = static_cast<int>(ccy) / kSlot;
            if (col < gridN && row < gridN) cell = kCraftCell0 + row * gridN + col;
        }
    }
    bool resultHover = lx >= static_cast<float>(resX - 5) && lx < static_cast<float>(resX + 21) &&
                       ly >= static_cast<float>(resY - 5) && ly < static_cast<float>(resY + 21);
    auto slotAt = [&](int i) -> ItemStack& {
        if (i >= kCraftCell0) return craft_[static_cast<size_t>(i - kCraftCell0)];
        return i < 9 ? inv.slots[static_cast<size_t>(i)]
                     : inv.main[static_cast<size_t>(i - 9)];
    };
    auto slotPos = [&](int i, float& ix, float& iy) {
        if (i >= kCraftCell0) {
            int c = i - kCraftCell0;
            ix = gx(static_cast<float>(craftX + (c % gridN) * kSlot));
            iy = gy(static_cast<float>(craftY + (c / gridN) * kSlot));
            return;
        }
        int col = i < 9 ? i : (i - 9) % 9;
        ix = gx(static_cast<float>(kSurvGridX + col * kSlot));
        iy = i < 9 ? gy(static_cast<float>(hotbarY))
                   : gy(static_cast<float>(gridY + ((i - 9) / 9) * kSlot));
    };

    // Outside the panel entirely: a click drops the carried stack into the world.
    bool overPanel = inRect(in.mouseX, in.mouseY, px, py, kSurvW * kScale, panelH * kScale);
    handleSlotClicks(inv, cell, in, !overPanel);

    // ---- Crafting: match the grid, then let the result slot be taken.
    auto computeResult = [&]() -> ItemStack {
        if (!recipes_) return {};
        BlockId ids[9] = {};
        for (int i = 0; i < gridN * gridN; ++i) {
            ids[i] = craft_[i].empty() ? BLOCK_AIR : craft_[i].id;
        }
        return recipes_->match(ids, gridN, gridN);
    };
    auto consumeGrid = [&]() {
        for (int i = 0; i < gridN * gridN; ++i) {
            ItemStack& s = craft_[i];
            if (!s.empty() && --s.count <= 0) s = {};
        }
    };
    craftResult_ = computeResult();
    if (resultHover && !craftResult_.empty() && in.leftPressed) {
        // Double-click the result: keep crafting onto the cursor until the cursor is
        // full or the ingredients run out (vanilla).
        bool doubleCraft = in.time - lastResultClick_ < 0.4;
        lastResultClick_ = in.time;
        if (doubleCraft && !in.shiftDown) {
            while (true) {
                ItemStack r = computeResult();
                if (r.empty()) break;
                if (!held_.empty() &&
                    (held_.id != r.id || held_.count + r.count > Inventory::kMaxStack)) {
                    break;
                }
                held_.id = r.id;
                held_.count += r.count;
                consumeGrid();
            }
        } else if (in.shiftDown) {
            // Craft everything the ingredients (and inventory space) allow.
            while (true) {
                ItemStack r = computeResult();
                if (r.empty()) break;
                int space = 0;
                for (const ItemStack& s : inv.slots) {
                    space += s.empty() ? Inventory::kMaxStack
                                       : (s.id == r.id ? Inventory::kMaxStack - s.count : 0);
                }
                for (const ItemStack& s : inv.main) {
                    space += s.empty() ? Inventory::kMaxStack
                                       : (s.id == r.id ? Inventory::kMaxStack - s.count : 0);
                }
                if (space < r.count) break;
                inv.add(r.id, r.count);
                consumeGrid();
            }
        } else if (held_.empty()) {
            held_ = craftResult_;
            consumeGrid();
        } else if (held_.id == craftResult_.id &&
                   held_.count + craftResult_.count <= Inventory::kMaxStack) {
            held_.count += craftResult_.count;
            consumeGrid();
        }
        craftResult_ = computeResult();
    }

    // ---- Draw.
    int panelTex = craftingTable ? tex_.craftingPanel : tex_.survivalPanel;
    if (panelTex > 0) {
        ui.texQuad(panelTex, px, py, kSurvW * kScale, panelH * kScale,
                   0.0f, 0.0f, kSurvW / kSheet, panelH / kSheet);
    } else {
        ui.quad(px, py, kSurvW * kScale, panelH * kScale, glm::vec4(0.78f, 0.78f, 0.78f, 1.0f));
    }
    // Header title (the space the user added at the top): player name, else "Inventory".
    if (!craftingTable) {
        std::string title = !headerTitle_.empty()
                                ? headerTitle_
                                : (lang_ ? lang_->tr("container.inventory", "Inventory")
                                         : "Inventory");
        font.drawText(ui, title, gx(static_cast<float>(kSurvTitleX)),
                      gy(static_cast<float>(kSurvTitleY)), kScale,
                      glm::vec4(0.25f, 0.25f, 0.25f, 1.0f), /*shadow=*/false);
    }
    // Player model in the survival panel's preview window (box shifted with the header:
    // ~26,18 52x70); the 1:2 offscreen render is letterboxed inside so proportions hold.
    if (!craftingTable && tex_.playerPreview > 0) {
        float bh = 70.0f * kScale, bw = bh * 0.5f;
        ui.sprite(tex_.playerPreview, gx(26.0f + 26.0f) - bw * 0.5f, gy(18.0f), bw, bh);
        // Watch the cursor: angles from the model's eye area toward the mouse.
        float ax = gx(52.0f), ay = gy(32.0f);
        previewLookYaw_ = glm::degrees(std::atan((in.mouseX - ax) / 300.0f));
        previewLookPitch_ = glm::degrees(std::atan((in.mouseY - ay) / 300.0f));
        previewLookValid_ = true;
    }
    for (int i = 0; i < 9 + Inventory::kMainSlots; ++i) {
        const ItemStack& s = slotAt(i);
        if (s.empty()) continue;
        int tex = icons.iconTexId(s.id);
        if (tex < 0) continue;
        float ix = 0, iy = 0;
        slotPos(i, ix, iy);
        ui.sprite(tex, ix, iy, 16.0f * kScale, 16.0f * kScale);
        drawCount(ui, font, s.count, ix, iy);
    }
    for (int i = 0; i < gridN * gridN; ++i) {
        const ItemStack& s = craft_[i];
        if (s.empty()) continue;
        int tex = icons.iconTexId(s.id);
        if (tex < 0) continue;
        float ix = 0, iy = 0;
        slotPos(kCraftCell0 + i, ix, iy);
        ui.sprite(tex, ix, iy, 16.0f * kScale, 16.0f * kScale);
        drawCount(ui, font, s.count, ix, iy);
    }
    if (!craftResult_.empty()) {
        int tex = icons.iconTexId(craftResult_.id);
        if (tex >= 0) {
            float ix = gx(static_cast<float>(resX)), iy = gy(static_cast<float>(resY));
            ui.sprite(tex, ix, iy, 16.0f * kScale, 16.0f * kScale);
            drawCount(ui, font, craftResult_.count, ix, iy);
        }
    }
    // Live split preview: only once the drag has entered a SECOND slot (a single-slot
    // press is a plain place-all, not a split). Dragged slots show their projected share.
    if (dragLeft_ && !held_.empty() && dragCells_.size() >= 2) {
        int remaining = held_.count, total = held_.count;
        int n = static_cast<int>(dragCells_.size()), i = 0;
        int ghost = icons.iconTexId(held_.id);
        for (int c : dragCells_) {
            const ItemStack& s = slotAt(c);
            int space = Inventory::kMaxStack - (s.empty() ? 0 : s.count);
            int amount = std::min(std::min(dragWant(total, n, i), remaining), space);
            ++i;
            if (amount <= 0) continue;
            remaining -= amount;
            float ix = 0, iy = 0;
            slotPos(c, ix, iy);
            if (ghost >= 0) {
                ui.sprite(ghost, ix, iy, 16.0f * kScale, 16.0f * kScale,
                          glm::vec4(1.0f, 1.0f, 1.0f, 0.65f));
            }
            drawCount(ui, font, (s.empty() ? 0 : s.count) + amount, ix, iy);
        }
        dragPreviewRemaining_ = remaining;
    }
    if (cell >= 0) {
        float ix = 0, iy = 0;
        slotPos(cell, ix, iy);
        ui.quad(ix, iy, 16.0f * kScale, 16.0f * kScale, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
        const ItemStack& s = slotAt(cell);
        if (held_.empty() && !s.empty()) {
            drawTooltip(ui, font, blockLabel(reg, s.id), in.mouseX, in.mouseY, W, H,
                        tex_.tooltipBg, tex_.tooltipFrame);
        }
    } else if (resultHover) {
        ui.quad(gx(static_cast<float>(resX)), gy(static_cast<float>(resY)),
                16.0f * kScale, 16.0f * kScale, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
        if (held_.empty() && !craftResult_.empty()) {
            drawTooltip(ui, font, blockLabel(reg, craftResult_.id), in.mouseX, in.mouseY, W, H,
                        tex_.tooltipBg, tex_.tooltipFrame);
        }
    }
}

} // namespace mc
