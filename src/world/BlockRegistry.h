#pragma once

#include "world/Block.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc {

class TextureArray;

// Runtime block registry: one Block instance per type, ids assigned in add() order.
// Blocks are registered in registerDefaults() (Blocks.cpp) — one statement per block.
class BlockRegistry {
public:
    void registerDefaults();

    // Registers a block, assigns its id, and returns it for texture chaining.
    template <typename T>
    T& add(std::unique_ptr<T> block) {
        T* raw = block.get();
        addImpl(std::move(block));
        return *raw;
    }

    const Block& block(BlockId id) const { return *blocks_[id]; }
    const Block& block(BlockState s) const { return *blocks_[s.id()]; }

    // Id by registered name; 0 (air) when unknown.
    BlockId byName(const std::string& name) const {
        for (const auto& b : blocks_) {
            if (b->name() == name) return b->id;
        }
        return 0;
    }
    bool isOpaque(BlockState s) const { return blocks_[s.id()]->isOpaque(s); }
    bool isSolid(BlockState s) const { return blocks_[s.id()]->props().solid; }
    uint16_t count() const { return static_cast<uint16_t>(blocks_.size()); }

    // Block tags (Properties::tag / burns): every block carrying `tag`, in id order.
    // Recipes iterate/reference these ("#planks" ingredients, log->planks families);
    // the furnace queries "fuel" + props().fuelSeconds.
    const std::vector<BlockId>& withTag(const std::string& tag) const {
        static const std::vector<BlockId> kNone;
        auto it = tags_.find(tag);
        return it != tags_.end() ? it->second : kNone;
    }
    bool hasTag(BlockId id, const std::string& tag) const {
        const std::vector<BlockId>& v = withTag(tag);
        return std::find(v.begin(), v.end(), id) != v.end();
    }

    // Unique texture base names referenced by all blocks (for the texture array build).
    std::vector<std::string> textureNames() const;
    // Resolve each block's texture layers from the loaded texture array.
    void resolveTextures(const TextureArray& tex);
    // Load each block's gui/hand display transforms from its model json (parent-chain
    // aware; vanilla defaults when a model or entry is missing).
    void loadDisplayTransforms(const std::string& modelsDir) {
        for (auto& b : blocks_) b->setDisplay(loadModelDisplay(modelsDir, b->itemModelName()));
    }

    // The hidden full-cell block for two merged slab halves (bottom/top material =
    // their SlabBlock ids); 0 when the pair isn't registered.
    BlockId slabCombo(BlockId bottomMaterial, BlockId topMaterial) const {
        auto it = slabCombos_.find(comboKey(bottomMaterial, topMaterial));
        return it != slabCombos_.end() ? it->second : 0;
    }

    // Named handles for blocks referenced directly in code (world generation).
    BlockId grass = 0, dirt = 0, stone = 0, sand = 0;

private:
    void addImpl(std::unique_ptr<Block> block);
    static uint32_t comboKey(BlockId b, BlockId t) {
        return (static_cast<uint32_t>(b) << 16) | t;
    }

    std::vector<std::unique_ptr<Block>> blocks_;
    std::unordered_map<uint32_t, BlockId> slabCombos_;
    std::unordered_map<std::string, std::vector<BlockId>> tags_;
};

} // namespace mc
