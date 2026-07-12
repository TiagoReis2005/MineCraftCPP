#pragma once

#include "world/Block.h"

#include <algorithm>
#include <array>

namespace mc {

// One inventory slot: a block type and how many (survival consumes/collects them;
// creative ignores the count and never runs out).
struct ItemStack {
    BlockId id = BLOCK_AIR;
    int count = 0;

    bool empty() const { return id == BLOCK_AIR || count <= 0; }
};

// 9-slot hotbar + 27-slot main storage (the survival inventory screen), stack counts.
struct Inventory {
    static constexpr int kSlots = 9;
    static constexpr int kMainSlots = 36; // 4 rows (the user's edited panels)
    static constexpr int kMaxStack = 64;

    std::array<ItemStack, kSlots> slots{};    // hotbar
    std::array<ItemStack, kMainSlots> main{}; // storage rows of the inventory screen
    int selected = 0;

    BlockId selectedBlock() const {
        const ItemStack& s = slots[static_cast<size_t>(selected)];
        return s.empty() ? BLOCK_AIR : s.id;
    }

    // Removes one item from the selected stack (survival placing).
    void consumeSelected() {
        ItemStack& s = slots[static_cast<size_t>(selected)];
        if (s.count > 0 && --s.count == 0) s = {};
    }

    // True if at least one item of `id` would fit (a matching partial stack or an empty
    // slot). Dropped items check this before gliding to the player, so a full inventory
    // leaves them lying on the ground instead of bouncing off you.
    bool hasRoomFor(BlockId id) const {
        if (id == BLOCK_AIR) return false;
        for (const ItemStack& s : slots) {
            if (s.empty() || (s.id == id && s.count < kMaxStack)) return true;
        }
        for (const ItemStack& s : main) {
            if (s.empty() || (s.id == id && s.count < kMaxStack)) return true;
        }
        return false;
    }

    // Adds picked-up items: tops up existing stacks first (hotbar then main), then
    // fills empty slots. Returns how many did not fit (stay on the ground).
    int add(BlockId id, int n) {
        if (id == BLOCK_AIR || n <= 0) return 0;
        auto topUp = [&](ItemStack& s) {
            if (n > 0 && s.id == id && s.count > 0 && s.count < kMaxStack) {
                int take = std::min(n, kMaxStack - s.count);
                s.count += take;
                n -= take;
            }
        };
        auto fill = [&](ItemStack& s) {
            if (n > 0 && s.empty()) {
                int take = std::min(n, kMaxStack);
                s = {id, take};
                n -= take;
            }
        };
        for (ItemStack& s : slots) topUp(s);
        for (ItemStack& s : main) topUp(s);
        for (ItemStack& s : slots) fill(s);
        for (ItemStack& s : main) fill(s);
        return n;
    }
};

} // namespace mc
