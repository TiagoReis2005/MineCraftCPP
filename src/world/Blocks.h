#pragma once

#include "world/Block.h"

namespace mc {

// ---- SlabBlock: data bit0 = top half. Merging two complementary halves in one cell
// produces a hidden DoubleSlabBlock (mixed materials supported).
class SlabBlock : public Block {
public:
    static constexpr uint16_t kTopBit = 1;
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    BlockState placementState(const PlacementCtx&) const override;
    bool tryMergeInto(const BlockRegistry&, BlockState clicked, const PlacementCtx&,
                      BlockState* out) const override;
    bool tryCombineAt(const BlockRegistry&, BlockState existing, const PlacementCtx&,
                      BlockState* out) const override;
    bool isOpaque(BlockState) const override { return false; }
};

// ---- DoubleSlabBlock: hidden full-cell block made of two slab halves; breaking removes
// only the aimed half.
class DoubleSlabBlock : public Block {
public:
    DoubleSlabBlock(std::string name, Properties props, BlockId bottomSlab, BlockId topSlab)
        : Block(std::move(name), std::move(props)), bottomSlab_(bottomSlab), topSlab_(topSlab) {}
    void appendMesh(BlockMeshCtx&) const override;
    BlockState breakResult(BlockState, const glm::vec3& hitPoint, const glm::ivec3& cell,
                           const glm::ivec3& normal) const override;
    // Only the AIMED half is outlined/cracked — mirrors what breakResult would remove.
    std::vector<Segment> aimedOutlineEdges(BlockState, const World*, const glm::ivec3& cell,
                                           const glm::vec3& hitPoint,
                                           const glm::ivec3& normal) const override;
    std::vector<AABB> crackBoxes(BlockState, const World*, const glm::ivec3& cell,
                                 const glm::vec3& hitPoint,
                                 const glm::ivec3& normal) const override;
    // Halves break independently: switching halves restarts dig progress.
    uint16_t aimedPart(BlockState, const glm::ivec3& cell, const glm::vec3& hitPoint,
                       const glm::ivec3& normal) const override;
    std::vector<Drop> drops(BlockState, ToolType) const override;
    BlockId bottomSlab() const { return bottomSlab_; }
    BlockId topSlab() const { return topSlab_; }

private:
    BlockId bottomSlab_ = 0;
    BlockId topSlab_ = 0;
};

// ---- StairBlock: data bits0-1 = facing (0 -Z, 1 +X, 2 +Z, 3 -X = ascent direction),
// bit2 = upside down.
class StairBlock : public Block {
public:
    static constexpr uint16_t kFacingMask = 3;
    static constexpr uint16_t kTopBit = 4;
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    std::vector<Segment> outlineEdges(BlockState, const World*, const glm::ivec3&) const override;
    BlockState placementState(const PlacementCtx&) const override;
    int facingOf(BlockState s) const override { return s.data() & kFacingMask; }
    bool isOpaque(BlockState) const override { return false; }
};

// ---- PillarBlock: data bits0-1 = axis (0 Y, 1 X, 2 Z); end texture on the axis faces.
class PillarBlock : public Block {
public:
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    BlockState placementState(const PlacementCtx&) const override;
};

// ---- GlassBlock / LeavesBlock: cube behavior, tuned culling + drops.
class GlassBlock : public Block {
public:
    using Block::Block;
    std::vector<Drop> drops(BlockState, ToolType) const override { return {}; } // shatters
};

class LeavesBlock : public Block {
public:
    LeavesBlock(std::string name, Properties props, std::string sapling)
        : Block(std::move(name), std::move(props)), sapling_(std::move(sapling)) {}
    std::vector<Drop> drops(BlockState, ToolType) const override;

private:
    std::string sapling_;
};

// ---- LayerBlock (snow layers): data bits0-2 = layers-1 (1..8). Placing onto itself
// adds a layer; a single layer has no collision.
class LayerBlock : public Block {
public:
    static constexpr uint16_t kLayerMask = 7;
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    std::vector<AABB> outlineBoxes(BlockState, const World*, const glm::ivec3&) const override;
    // A FULL stack supports like a full block (MC's snow support shape); partial stacks
    // support nothing, so snow/plates/doors can't float above them.
    std::vector<AABB> supportBoxes(BlockState s, const World* w,
                                   const glm::ivec3& c) const override {
        if (layers(s) >= 8) return {AABB{}};
        return collisionBoxes(s, w, c);
    }
    bool tryMergeInto(const BlockRegistry&, BlockState clicked, const PlacementCtx&,
                      BlockState* out) const override;
    bool tryCombineAt(const BlockRegistry& reg, BlockState existing, const PlacementCtx& ctx,
                      BlockState* out) const override {
        return tryMergeInto(reg, existing, ctx, out); // same rule: +1 layer
    }
    bool isOpaque(BlockState) const override { return false; }
    static int layers(BlockState s) { return (s.data() & kLayerMask) + 1; }
    // Snow's model files are per-height; the 1-layer one inherits thin_block's hand lift.
    std::string itemModelName() const override { return "snow_height2"; }
};

// ---- FenceBlock: center post + arms toward connectable neighbors. Visual/outline stay
// within the cell; collision is 1.5 blocks tall like MC.
class FenceBlock : public Block {
public:
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    // Neighbor-aware: 4x4 post + one 4-wide full-height arm per connected side (collision
    // is 1.5 blocks tall). The drawn outline is the union silhouette (no joint seams).
    // Outline boxes hug the MESH (vanilla-style): full-height post + one thin box per
    // arm spanning the two rails. Each box draws its own 12 edges (no silhouette
    // merging needed now that the boxes match the shape).
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    std::vector<AABB> outlineBoxes(BlockState, const World*, const glm::ivec3&) const override;
    // One merged box (union bounds of post + arms): the crack overlay reads as a single
    // centered pattern instead of repeating on the post and again on each arm.
    std::vector<AABB> crackBoxes(BlockState, const World*, const glm::ivec3& cell,
                                 const glm::vec3& hitPoint,
                                 const glm::ivec3& normal) const override;
    bool isOpaque(BlockState) const override { return false; }
    bool connectsToFence(BlockState) const override { return true; }
    // The fence's display transforms live in its _inventory model (gui yaw 135).
    std::string itemModelName() const override { return name() + "_inventory"; }
};

// ---- FenceGateBlock: data bits0-1 facing, bit2 open. Right-click toggles.
class FenceGateBlock : public Block {
public:
    static constexpr uint16_t kFacingMask = 3;
    static constexpr uint16_t kOpenBit = 4;
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    std::vector<AABB> outlineBoxes(BlockState, const World*, const glm::ivec3&) const override;
    BlockState placementState(const PlacementCtx&) const override;
    int facingOf(BlockState s) const override { return s.data() & kFacingMask; }
    bool onUse(World&, const glm::ivec3&, BlockState) const override;
    bool isOpaque(BlockState) const override { return false; }
    bool connectsToFence(BlockState) const override { return true; }
};

// ---- DoorBlock: two cells tall. data bit0 = upper half, bits1-2 = facing, bit3 = open,
// bit4 = hinge on the right (set automatically to mirror a neighboring door -> double
// doors swing apart). Placement writes both halves; breaking either removes both;
// right-click toggles.
class DoorBlock : public Block {
public:
    static constexpr uint16_t kUpperBit = 1;
    static constexpr uint16_t kFacingShift = 1; // bits 1..2
    static constexpr uint16_t kOpenBit = 8;
    static constexpr uint16_t kHingeRightBit = 16;
    DoorBlock(std::string name, Properties props, std::string texBottom, std::string texTop);
    void gatherTextureNames(std::vector<std::string>& out) const override;
    void resolveTextures(const TextureArray& tex) override;
    void appendMesh(BlockMeshCtx&) const override;
    void appendItemMesh(BlockMeshCtx&) const override; // full door (both halves) for icons
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override;
    std::vector<AABB> outlineBoxes(BlockState, const World*, const glm::ivec3&) const override;
    BlockState placementState(const PlacementCtx&) const override;
    int facingOf(BlockState s) const override { return (s.data() >> kFacingShift) & 3; }
    bool canPlaceAt(const World&, const glm::ivec3&, BlockState) const override;
    // The upper half stands on its lower half (a thin door is never "sturdy"); the
    // lower half uses the normal support rules.
    bool canSurviveAt(const World&, const glm::ivec3&, BlockState) const override;
    void onPlaced(World&, const glm::ivec3&, BlockState) const override;
    void onBroken(World&, const glm::ivec3&, BlockState) const override;
    bool onUse(World&, const glm::ivec3&, BlockState) const override;
    // The door ITEM lives on the lower half only, so the two halves breaking together
    // (either mined directly or popped by lost support) always yield exactly one door.
    std::vector<Drop> drops(BlockState s, ToolType held) const override {
        if (s.data() & kUpperBit) return {};
        return Block::drops(s, held);
    }
    bool isOpaque(BlockState) const override { return false; }
    // Doors have no <name>.json; any state model works (they define no display and fall
    // back to the block defaults, but the name resolves once the mesher reads elements).
    std::string itemModelName() const override { return name() + "_bottom_left"; }

private:
    std::string texBottomHalf_, texTopHalf_;
    uint32_t layerBottomHalf_ = 0, layerTopHalf_ = 0;
};

// ---- PressurePlateBlock: thin plate, no collision. data bit0 = pressed (future ticks).
class PressurePlateBlock : public Block {
public:
    using Block::Block;
    void appendMesh(BlockMeshCtx&) const override;
    std::vector<AABB> collisionBoxes(BlockState, const World*, const glm::ivec3&) const override {
        return {};
    }
    std::vector<AABB> outlineBoxes(BlockState, const World*, const glm::ivec3&) const override;
    bool isOpaque(BlockState) const override { return false; }
};

// Horizontal facing helpers shared by stairs/doors/gates: 0 = -Z, 1 = +X, 2 = +Z, 3 = -X.
int facingFromYaw(float yawDeg);
glm::ivec3 facingDir(int facing);

} // namespace mc
