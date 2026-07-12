#include "world/Block.h"

#include "gfx/TextureArray.h"
#include "world/BlockMesh.h"
#include "world/BlockRegistry.h"
#include "world/World.h"

#include <algorithm>

namespace mc {

namespace {

// World-space offset of a block-local side, rotated by the state's facing. Bottom/Top
// are absolute; horizontals rotate 90 deg clockwise (from above) per facing step, so
// local North always points out of the block's front.
glm::ivec3 sideDir(BlockSide side, int facing) {
    switch (side) {
        case BlockSide::Bottom: return {0, -1, 0};
        case BlockSide::Top: return {0, 1, 0};
        default: break;
    }
    int x = 0, z = 0;
    switch (side) {
        case BlockSide::North: z = -1; break;
        case BlockSide::South: z = 1; break;
        case BlockSide::East: x = 1; break;
        default: x = -1; break; // West
    }
    for (int i = 0; i < (facing & 3); ++i) {
        int nx = -z, nz = x; // 90 deg clockwise seen from above
        x = nx;
        z = nz;
    }
    return {x, 0, z};
}

// True when the block at `cell` offers a sturdy FULL face toward `toward` (unit axis
// pointing at the supported block): some collision box must reach that face of the cell
// and span it completely. Full cubes and top-slab tops qualify; fence posts don't.
bool sturdyFace(const World& world, const glm::ivec3& cell, const glm::ivec3& toward) {
    BlockState s = world.getState(cell.x, cell.y, cell.z);
    if (s.isAir()) return false;
    int axis = toward.x != 0 ? 0 : (toward.y != 0 ? 1 : 2);
    bool positive = toward[axis] > 0;
    for (const AABB& b : world.registry().block(s).supportBoxes(s, &world, cell)) {
        constexpr float e = 1e-4f;
        if (positive ? b.max[axis] < 1.0f - e : b.min[axis] > e) continue; // not at the face
        int t0 = (axis + 1) % 3, t1 = (axis + 2) % 3;
        if (b.min[t0] <= e && b.max[t0] >= 1.0f - e && b.min[t1] <= e && b.max[t1] >= 1.0f - e) {
            return true;
        }
    }
    return false;
}

} // namespace

void Block::gatherTextureNames(std::vector<std::string>& out) const {
    if (!texTop_.empty()) out.push_back(texTop_);
    if (!texBottom_.empty()) out.push_back(texBottom_);
    if (!texSide_.empty()) out.push_back(texSide_);
}

void Block::resolveTextures(const TextureArray& tex) {
    uint32_t top = tex.layer(texTop_);
    uint32_t bottom = tex.layer(texBottom_);
    uint32_t side = tex.layer(texSide_);
    layers_[FACE_PX] = side;
    layers_[FACE_NX] = side;
    layers_[FACE_PY] = top;
    layers_[FACE_NY] = bottom;
    layers_[FACE_PZ] = side;
    layers_[FACE_NZ] = side;
}

void Block::appendMesh(BlockMeshCtx& ctx) const {
    // Default: full cube with per-face neighbor culling.
    uint8_t mask = 0;
    for (int face = 0; face < 6; ++face) {
        BlockState nb = ctx.neighbor(kFaceNeighbor[face][0], kFaceNeighbor[face][1],
                                     kFaceNeighbor[face][2]);
        if (ctx.reg.isOpaque(nb)) continue;
        if (nb.id() == id && props_.cullSame) continue;
        mask |= faceBit(face);
    }
    if (mask) emitBox(ctx.mesh, ctx.origin, glm::vec3(0.0f), glm::vec3(1.0f), layers_, mask);
}

std::vector<AABB> Block::collisionBoxes(BlockState, const World*, const glm::ivec3&) const {
    if (!props_.solid) return {};
    return {AABB{}};
}

std::vector<AABB> Block::outlineBoxes(BlockState state, const World* world,
                                      const glm::ivec3& cell) const {
    if (props_.solid) return collisionBoxes(state, world, cell);
    return {AABB{}}; // non-solid blocks are still targetable
}

void Block::appendBoxEdges(const AABB& box, std::vector<Segment>& out) {
    const glm::vec3& n = box.min;
    const glm::vec3& x = box.max;
    // 4 edges along each axis.
    out.push_back({{n.x, n.y, n.z}, {x.x, n.y, n.z}});
    out.push_back({{n.x, x.y, n.z}, {x.x, x.y, n.z}});
    out.push_back({{n.x, n.y, x.z}, {x.x, n.y, x.z}});
    out.push_back({{n.x, x.y, x.z}, {x.x, x.y, x.z}});
    out.push_back({{n.x, n.y, n.z}, {n.x, x.y, n.z}});
    out.push_back({{x.x, n.y, n.z}, {x.x, x.y, n.z}});
    out.push_back({{n.x, n.y, x.z}, {n.x, x.y, x.z}});
    out.push_back({{x.x, n.y, x.z}, {x.x, x.y, x.z}});
    out.push_back({{n.x, n.y, n.z}, {n.x, n.y, x.z}});
    out.push_back({{x.x, n.y, n.z}, {x.x, n.y, x.z}});
    out.push_back({{n.x, x.y, n.z}, {n.x, x.y, x.z}});
    out.push_back({{x.x, x.y, n.z}, {x.x, x.y, x.z}});
}

std::vector<Segment> Block::outlineEdges(BlockState state, const World* world,
                                         const glm::ivec3& cell) const {
    std::vector<Segment> segs;
    for (const AABB& box : outlineBoxes(state, world, cell)) appendBoxEdges(box, segs);
    return segs;
}

bool Block::canSurviveAt(const World& world, const glm::ivec3& cell, BlockState state) const {
    if (props_.support.empty()) return true; // no rules: can float
    const BlockRegistry& reg = world.registry();
    for (const SupportRule& r : props_.support) {
        glm::ivec3 dir = sideDir(r.side, facingOf(state));
        glm::ivec3 n = cell + dir;
        if (r.blocks.empty() && r.tags.empty()) {
            if (sturdyFace(world, n, -dir)) return true;
            continue;
        }
        BlockState ns = world.getState(n.x, n.y, n.z);
        if (ns.isAir()) continue;
        const std::string& name = reg.block(ns).name();
        if (std::find(r.blocks.begin(), r.blocks.end(), name) != r.blocks.end()) return true;
        for (const std::string& t : r.tags) {
            if (reg.hasTag(ns.id(), t)) return true;
        }
    }
    return false;
}

float Block::breakSeconds(ToolType held) const {
    if (props_.strength < 0.0f) return -1.0f; // unbreakable
    // MC-style: adequate tooling = hardness * 1.5s; missing a required tool = 5x.
    bool adequate = !props_.requiresTool || held == props_.tool;
    return props_.strength * (adequate ? 1.5f : 5.0f);
}

std::vector<Drop> Block::drops(BlockState, ToolType held) const {
    if (props_.requiresTool && held != props_.tool) return {};
    return {Drop{name_, 1, 1, 1.0f}};
}

} // namespace mc
