#include "world/Blocks.h"

#include "gfx/TextureArray.h"
#include "world/BlockMesh.h"
#include "world/BlockRegistry.h"
#include "world/World.h"

#include <cmath>

namespace mc {
namespace {

// Skip faces that lie on a cell boundary against an opaque neighbor.
uint8_t cullMask(const BlockMeshCtx& ctx, const glm::vec3& from, const glm::vec3& to) {
    uint8_t mask = kAllFaces;
    const bool touches[6] = {to.x >= 1.0f, from.x <= 0.0f, to.y >= 1.0f,
                             from.y <= 0.0f, to.z >= 1.0f, from.z <= 0.0f};
    for (int face = 0; face < 6; ++face) {
        if (!touches[face]) continue;
        BlockState nb = ctx.neighbor(kFaceNeighbor[face][0], kFaceNeighbor[face][1],
                                     kFaceNeighbor[face][2]);
        if (ctx.reg.isOpaque(nb)) mask &= static_cast<uint8_t>(~faceBit(face));
    }
    return mask;
}

constexpr float px(float p) { return p / 16.0f; } // pixel -> block units

} // namespace

int facingFromYaw(float yawDeg) {
    float r = glm::radians(yawDeg);
    float fx = std::cos(r), fz = std::sin(r);
    if (std::fabs(fx) > std::fabs(fz)) return fx > 0.0f ? 1 : 3; // +X : -X
    return fz > 0.0f ? 2 : 0;                                   // +Z : -Z
}

glm::ivec3 facingDir(int facing) {
    switch (facing & 3) {
        case 0: return {0, 0, -1};
        case 1: return {1, 0, 0};
        case 2: return {0, 0, 1};
        default: return {-1, 0, 0};
    }
}

// ------------------------------------------------------------------ SlabBlock
void SlabBlock::appendMesh(BlockMeshCtx& ctx) const {
    bool top = ctx.state.data() & kTopBit;
    glm::vec3 from(0.0f, top ? 0.5f : 0.0f, 0.0f);
    glm::vec3 to(1.0f, top ? 1.0f : 0.5f, 1.0f);
    emitBox(ctx.mesh, ctx.origin, from, to, layers_, cullMask(ctx, from, to));
}

std::vector<AABB> SlabBlock::collisionBoxes(BlockState s, const World*, const glm::ivec3&) const {
    bool top = s.data() & kTopBit;
    return {AABB{{0, top ? 0.5f : 0.0f, 0}, {1, top ? 1.0f : 0.5f, 1}}};
}

BlockState SlabBlock::placementState(const PlacementCtx& ctx) const {
    bool top = false;
    if (ctx.normal.y > 0) top = false;
    else if (ctx.normal.y < 0) top = true;
    else top = (ctx.hitPoint.y - std::floor(ctx.hitPoint.y)) > 0.5f;
    return BlockState(id, top ? kTopBit : 0);
}

bool SlabBlock::tryMergeInto(const BlockRegistry& reg, BlockState clicked,
                             const PlacementCtx& ctx, BlockState* out) const {
    // Clicking the exposed half-face of a complementary slab fills the cell (mixed
    // materials allowed via the registry's combo table).
    const SlabBlock* other = dynamic_cast<const SlabBlock*>(&reg.block(clicked));
    if (!other) return false;
    bool clickedTop = clicked.data() & kTopBit;
    BlockId combo = 0;
    if (!clickedTop && ctx.normal.y > 0) {
        combo = reg.slabCombo(clicked.id(), id); // existing bottom + new top
    } else if (clickedTop && ctx.normal.y < 0) {
        combo = reg.slabCombo(id, clicked.id()); // new bottom + existing top
    }
    if (combo == 0) return false;
    *out = BlockState(combo);
    return true;
}

bool SlabBlock::tryCombineAt(const BlockRegistry& reg, BlockState existing,
                             const PlacementCtx& ctx, BlockState* out) const {
    // The placement cell already holds a slab: fill its empty half (mixed materials OK) --
    // but only when the click actually points at that half. Clicking near the top edge of
    // a neighbor's face asks for a top slab; if the top half is already there, the
    // placement fails instead of silently slipping a bottom slab underneath.
    const SlabBlock* other = dynamic_cast<const SlabBlock*>(&reg.block(existing));
    if (!other) return false;
    bool existingTop = existing.data() & kTopBit;
    bool wantTop = placementState(ctx).data() & kTopBit; // half the click asks for
    if (wantTop == existingTop) return false;            // that half is occupied
    BlockId combo = existingTop ? reg.slabCombo(id, existing.id())  // we become the bottom
                                : reg.slabCombo(existing.id(), id); // we become the top
    if (combo == 0) return false;
    *out = BlockState(combo);
    return true;
}

// ------------------------------------------------------------ DoubleSlabBlock
void DoubleSlabBlock::appendMesh(BlockMeshCtx& ctx) const {
    const Block& bottom = ctx.reg.block(bottomSlab_);
    const Block& top = ctx.reg.block(topSlab_);
    glm::vec3 bFrom(0.0f), bTo(1.0f, 0.5f, 1.0f);
    glm::vec3 tFrom(0.0f, 0.5f, 0.0f), tTo(1.0f);
    uint8_t bMask = cullMask(ctx, bFrom, bTo) & static_cast<uint8_t>(~faceBit(FACE_PY));
    uint8_t tMask = cullMask(ctx, tFrom, tTo) & static_cast<uint8_t>(~faceBit(FACE_NY));
    emitBox(ctx.mesh, ctx.origin, bFrom, bTo, bottom.faceLayers(), bMask);
    emitBox(ctx.mesh, ctx.origin, tFrom, tTo, top.faceLayers(), tMask);
}

namespace {
// Which half of a double slab the crosshair points at (shared by break + outline).
bool aimsTopHalf(const glm::vec3& hitPoint, const glm::ivec3& cell, const glm::ivec3& normal) {
    float frac = glm::clamp(hitPoint.y - static_cast<float>(cell.y), 0.0f, 1.0f);
    return normal.y > 0 || (normal.y == 0 && frac > 0.5f);
}
} // namespace

BlockState DoubleSlabBlock::breakResult(BlockState, const glm::vec3& hitPoint,
                                        const glm::ivec3& cell, const glm::ivec3& normal) const {
    return aimsTopHalf(hitPoint, cell, normal) ? BlockState(bottomSlab_)
                                               : BlockState(topSlab_, SlabBlock::kTopBit);
}

std::vector<AABB> DoubleSlabBlock::crackBoxes(BlockState, const World*, const glm::ivec3& cell,
                                              const glm::vec3& hitPoint,
                                              const glm::ivec3& normal) const {
    bool top = aimsTopHalf(hitPoint, cell, normal);
    return {AABB{{0.0f, top ? 0.5f : 0.0f, 0.0f}, {1.0f, top ? 1.0f : 0.5f, 1.0f}}};
}

uint16_t DoubleSlabBlock::aimedPart(BlockState, const glm::ivec3& cell,
                                    const glm::vec3& hitPoint, const glm::ivec3& normal) const {
    return aimsTopHalf(hitPoint, cell, normal) ? 1 : 2;
}

std::vector<Segment> DoubleSlabBlock::aimedOutlineEdges(BlockState s, const World* w,
                                                        const glm::ivec3& cell,
                                                        const glm::vec3& hitPoint,
                                                        const glm::ivec3& normal) const {
    std::vector<Segment> segs;
    for (const AABB& b : crackBoxes(s, w, cell, hitPoint, normal)) appendBoxEdges(b, segs);
    return segs;
}

std::vector<Drop> DoubleSlabBlock::drops(BlockState, ToolType) const {
    return {}; // halves drop individually via breakResult
}

// ---------------------------------------------------------------- StairBlock
void StairBlock::appendMesh(BlockMeshCtx& ctx) const {
    int facing = ctx.state.data() & kFacingMask;
    bool top = ctx.state.data() & kTopBit;

    // Base slab.
    glm::vec3 bFrom(0.0f, top ? 0.5f : 0.0f, 0.0f);
    glm::vec3 bTo(1.0f, top ? 1.0f : 0.5f, 1.0f);
    emitBox(ctx.mesh, ctx.origin, bFrom, bTo, layers_, cullMask(ctx, bFrom, bTo));

    // Quarter box on the ascent side.
    glm::vec3 qFrom(0.0f, top ? 0.0f : 0.5f, 0.0f);
    glm::vec3 qTo(1.0f, top ? 0.5f : 1.0f, 1.0f);
    switch (facing) {
        case 0: qTo.z = 0.5f; break;  // ascends toward -Z
        case 1: qFrom.x = 0.5f; break; // +X
        case 2: qFrom.z = 0.5f; break; // +Z
        default: qTo.x = 0.5f; break;  // -X
    }
    emitBox(ctx.mesh, ctx.origin, qFrom, qTo, layers_, cullMask(ctx, qFrom, qTo));
}

std::vector<AABB> StairBlock::collisionBoxes(BlockState s, const World*, const glm::ivec3&) const {
    int facing = s.data() & kFacingMask;
    bool top = s.data() & kTopBit;
    AABB base{{0, top ? 0.5f : 0.0f, 0}, {1, top ? 1.0f : 0.5f, 1}};
    AABB quarter{{0, top ? 0.0f : 0.5f, 0}, {1, top ? 0.5f : 1.0f, 1}};
    switch (facing) {
        case 0: quarter.max.z = 0.5f; break;
        case 1: quarter.min.x = 0.5f; break;
        case 2: quarter.min.z = 0.5f; break;
        default: quarter.max.x = 0.5f; break;
    }
    return {base, quarter};
}

std::vector<Segment> StairBlock::outlineEdges(BlockState s, const World*,
                                              const glm::ivec3&) const {
    int facing = s.data() & kFacingMask;
    bool top = s.data() & kTopBit;

    // The stair silhouette is an L-shaped prism: a 6-vertex profile in (h, y) — h runs
    // 0 (low side) -> 1 (ascent side) — extruded across the cell. Emitting its exact 18
    // edges gives one clean outline with no seam between the base and the quarter.
    glm::vec2 pf[6] = {{0, 0}, {1, 0}, {1, 1}, {0.5f, 1}, {0.5f, 0.5f}, {0, 0.5f}};
    if (top) {
        for (auto& p : pf) p.y = 1.0f - p.y;
    }
    auto mapP = [&](glm::vec2 p, float w) -> glm::vec3 {
        switch (facing) {
            case 0: return {w, p.y, 1.0f - p.x};  // ascends toward -Z
            case 1: return {p.x, p.y, w};         // +X
            case 2: return {w, p.y, p.x};         // +Z
            default: return {1.0f - p.x, p.y, w}; // -X
        }
    };

    std::vector<Segment> segs;
    segs.reserve(18);
    for (int i = 0; i < 6; ++i) {
        int j = (i + 1) % 6;
        segs.push_back({mapP(pf[i], 0.0f), mapP(pf[j], 0.0f)}); // profile ring, one side
        segs.push_back({mapP(pf[i], 1.0f), mapP(pf[j], 1.0f)}); // profile ring, other side
        segs.push_back({mapP(pf[i], 0.0f), mapP(pf[i], 1.0f)}); // lateral edge
    }
    return segs;
}

BlockState StairBlock::placementState(const PlacementCtx& ctx) const {
    uint16_t data = static_cast<uint16_t>(facingFromYaw(ctx.playerYawDeg));
    bool top = false;
    if (ctx.normal.y > 0) top = false;
    else if (ctx.normal.y < 0) top = true;
    else top = (ctx.hitPoint.y - std::floor(ctx.hitPoint.y)) > 0.5f;
    if (top) data |= kTopBit;
    return BlockState(id, data);
}

// --------------------------------------------------------------- PillarBlock
void PillarBlock::appendMesh(BlockMeshCtx& ctx) const {
    int axis = ctx.state.data() & 3;
    std::array<uint32_t, 6> l = layers_;
    if (axis == 1) { // X axis: ends on the +-X faces
        uint32_t end = layers_[FACE_PY], side = layers_[FACE_PZ];
        l = {end, end, side, side, side, side};
    } else if (axis == 2) { // Z axis
        uint32_t end = layers_[FACE_PY], side = layers_[FACE_PZ];
        l = {side, side, side, side, end, end};
    }
    uint8_t mask = 0;
    for (int face = 0; face < 6; ++face) {
        BlockState nb = ctx.neighbor(kFaceNeighbor[face][0], kFaceNeighbor[face][1],
                                     kFaceNeighbor[face][2]);
        if (!ctx.reg.isOpaque(nb)) mask |= faceBit(face);
    }
    if (mask) emitBox(ctx.mesh, ctx.origin, glm::vec3(0.0f), glm::vec3(1.0f), l, mask);
}

BlockState PillarBlock::placementState(const PlacementCtx& ctx) const {
    uint16_t axis = 0;
    if (ctx.normal.x != 0) axis = 1;
    else if (ctx.normal.z != 0) axis = 2;
    return BlockState(id, axis);
}

// --------------------------------------------------------------- LeavesBlock
std::vector<Drop> LeavesBlock::drops(BlockState, ToolType held) const {
    if (held == ToolType::Shears) return {Drop{name_, 1, 1, 1.0f}};
    return {Drop{sapling_, 1, 1, 0.05f}};
}

// ---------------------------------------------------------------- LayerBlock
void LayerBlock::appendMesh(BlockMeshCtx& ctx) const {
    float h = static_cast<float>(layers(ctx.state)) / 8.0f;
    glm::vec3 from(0.0f), to(1.0f, h, 1.0f);
    emitBox(ctx.mesh, ctx.origin, from, to, layers_, cullMask(ctx, from, to));
}

std::vector<AABB> LayerBlock::collisionBoxes(BlockState s, const World*, const glm::ivec3&) const {
    int n = layers(s);
    if (n <= 1) return {}; // a single layer is walk-through
    return {AABB{{0, 0, 0}, {1, static_cast<float>(n - 1) / 8.0f, 1}}};
}

std::vector<AABB> LayerBlock::outlineBoxes(BlockState s, const World*, const glm::ivec3&) const {
    return {AABB{{0, 0, 0}, {1, static_cast<float>(layers(s)) / 8.0f, 1}}};
}

bool LayerBlock::tryMergeInto(const BlockRegistry&, BlockState clicked, const PlacementCtx&,
                              BlockState* out) const {
    if (clicked.id() != id) return false;
    int n = layers(clicked);
    if (n >= 8) return false;
    *out = BlockState(id, static_cast<uint16_t>((clicked.data() & ~kLayerMask) | n));
    return true;
}

// ---------------------------------------------------------------- FenceBlock
void FenceBlock::appendMesh(BlockMeshCtx& ctx) const {
    // Center post.
    glm::vec3 pFrom(px(6), 0.0f, px(6)), pTo(px(10), 1.0f, px(10));
    emitBox(ctx.mesh, ctx.origin, pFrom, pTo, layers_, cullMask(ctx, pFrom, pTo));

    // Arms (two rails) toward connectable neighbors.
    const int dirs[4][3] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
    for (const auto& d : dirs) {
        BlockState nb = ctx.neighbor(d[0], 0, d[2]);
        if (!ctx.reg.block(nb).connectsToFence(nb)) continue;
        for (int rail = 0; rail < 2; ++rail) {
            float y0 = rail == 0 ? px(6) : px(12);
            float y1 = rail == 0 ? px(9) : px(15);
            glm::vec3 from, to;
            if (d[0] != 0) { // along X
                from = {d[0] > 0 ? px(10) : 0.0f, y0, px(7)};
                to = {d[0] > 0 ? 1.0f : px(6), y1, px(9)};
            } else { // along Z
                from = {px(7), y0, d[2] > 0 ? px(10) : 0.0f};
                to = {px(9), y1, d[2] > 0 ? 1.0f : px(6)};
            }
            emitBox(ctx.mesh, ctx.origin, from, to, layers_, cullMask(ctx, from, to));
        }
    }
}

namespace {
// Which of the four horizontal sides this fence connects to (world may be null: none).
// Order matches facingDir: 0 -Z, 1 +X, 2 +Z, 3 -X.
std::array<bool, 4> fenceConnections(const World* world, const glm::ivec3& cell) {
    std::array<bool, 4> out{false, false, false, false};
    if (!world) return out;
    const BlockRegistry& reg = world->registry();
    for (int i = 0; i < 4; ++i) {
        glm::ivec3 n = cell + facingDir(i);
        BlockState nb = world->getState(n.x, n.y, n.z);
        if (!nb.isAir() && reg.block(nb).connectsToFence(nb)) out[i] = true;
    }
    return out;
}

// One 4px-wide arm box per connected side (post width), running to the cell edge.
// Height range is caller-chosen (collision 0..1.5, outline 0..1).
void appendFenceArms(std::vector<AABB>& boxes, const std::array<bool, 4>& conn,
                     float y0, float y1) {
    if (conn[0]) boxes.push_back({{px(6), y0, 0.0f}, {px(10), y1, px(6)}});   // -Z
    if (conn[1]) boxes.push_back({{px(10), y0, px(6)}, {1.0f, y1, px(10)}});  // +X
    if (conn[2]) boxes.push_back({{px(6), y0, px(10)}, {px(10), y1, 1.0f}});  // +Z
    if (conn[3]) boxes.push_back({{0.0f, y0, px(6)}, {px(6), y1, px(10)}});   // -X
}
} // namespace

std::vector<AABB> FenceBlock::collisionBoxes(BlockState, const World* world,
                                             const glm::ivec3& cell) const {
    // 1.5 blocks tall like MC; connected sides get arm boxes so a fence line is a
    // continuous wall (no slipping between posts).
    std::vector<AABB> boxes{AABB{{px(6), 0.0f, px(6)}, {px(10), 1.5f, px(10)}}};
    appendFenceArms(boxes, fenceConnections(world, cell), 0.0f, 1.5f);
    return boxes;
}

std::vector<AABB> FenceBlock::outlineBoxes(BlockState, const World* world,
                                           const glm::ivec3& cell) const {
    // Hugs the mesh (vanilla-style): full-height 4x4 post, and each connected arm as
    // one 2px-wide box spanning both rails (y 6..15) — not a full-height slab, so the
    // outline sits right on the fence instead of boxing empty air.
    std::vector<AABB> boxes{AABB{{px(6), 0.0f, px(6)}, {px(10), 1.0f, px(10)}}};
    auto conn = fenceConnections(world, cell);
    const float y0 = px(6), y1 = px(15);
    if (conn[0]) boxes.push_back({{px(7), y0, 0.0f}, {px(9), y1, px(6)}});   // -Z
    if (conn[1]) boxes.push_back({{px(10), y0, px(7)}, {1.0f, y1, px(9)}});  // +X
    if (conn[2]) boxes.push_back({{px(7), y0, px(10)}, {px(9), y1, 1.0f}});  // +Z
    if (conn[3]) boxes.push_back({{0.0f, y0, px(7)}, {px(6), y1, px(9)}});   // -X
    return boxes;
}

std::vector<AABB> FenceBlock::crackBoxes(BlockState s, const World* world,
                                         const glm::ivec3& cell, const glm::vec3&,
                                         const glm::ivec3&) const {
    // Union bounds of post + arms as ONE box, so the overlay is a single centered
    // pattern over the whole fence piece instead of one per sub-box.
    std::vector<AABB> boxes = outlineBoxes(s, world, cell);
    AABB u = boxes.front();
    for (const AABB& b : boxes) {
        u.min = glm::min(u.min, b.min);
        u.max = glm::max(u.max, b.max);
    }
    return {u};
}

// ------------------------------------------------------------ FenceGateBlock
void FenceGateBlock::appendMesh(BlockMeshCtx& ctx) const {
    int facing = ctx.state.data() & kFacingMask;
    bool open = ctx.state.data() & kOpenBit;
    bool alongX = (facing == 0 || facing == 2); // gate bar spans X when facing +-Z

    auto post = [&](float a0, float a1) {
        glm::vec3 from, to;
        if (alongX) { from = {a0, px(5), px(7)}; to = {a1, 1.0f, px(9)}; }
        else { from = {px(7), px(5), a0}; to = {px(9), 1.0f, a1}; }
        emitBox(ctx.mesh, ctx.origin, from, to, layers_, cullMask(ctx, from, to));
    };
    post(0.0f, px(2));
    post(px(14), 1.0f);

    if (!open) {
        for (int rail = 0; rail < 2; ++rail) {
            float y0 = rail == 0 ? px(6) : px(12);
            float y1 = rail == 0 ? px(9) : px(15);
            glm::vec3 from, to;
            if (alongX) { from = {px(2), y0, px(7)}; to = {px(14), y1, px(9)}; }
            else { from = {px(7), y0, px(2)}; to = {px(9), y1, px(14)}; }
            emitBox(ctx.mesh, ctx.origin, from, to, layers_, kAllFaces);
        }
    }
}

std::vector<AABB> FenceGateBlock::collisionBoxes(BlockState s, const World*,
                                                 const glm::ivec3&) const {
    if (s.data() & kOpenBit) return {};
    bool alongX = ((s.data() & kFacingMask) == 0 || (s.data() & kFacingMask) == 2);
    if (alongX) return {AABB{{0.0f, 0.0f, px(6)}, {1.0f, 1.5f, px(10)}}};
    return {AABB{{px(6), 0.0f, 0.0f}, {px(10), 1.5f, 1.0f}}};
}

std::vector<AABB> FenceGateBlock::outlineBoxes(BlockState s, const World*,
                                               const glm::ivec3&) const {
    bool alongX = ((s.data() & kFacingMask) == 0 || (s.data() & kFacingMask) == 2);
    if (alongX) return {AABB{{0.0f, 0.0f, px(6)}, {1.0f, 1.0f, px(10)}}};
    return {AABB{{px(6), 0.0f, 0.0f}, {px(10), 1.0f, 1.0f}}};
}

BlockState FenceGateBlock::placementState(const PlacementCtx& ctx) const {
    return BlockState(id, static_cast<uint16_t>(facingFromYaw(ctx.playerYawDeg)));
}

bool FenceGateBlock::onUse(World& world, const glm::ivec3& cell, BlockState s) const {
    world.setState(cell.x, cell.y, cell.z, s.withData(s.data() ^ kOpenBit));
    return true;
}

// ------------------------------------------------------------------ DoorBlock
DoorBlock::DoorBlock(std::string name, Properties props, std::string texBottom, std::string texTop)
    : Block(std::move(name), std::move(props)),
      texBottomHalf_(std::move(texBottom)), texTopHalf_(std::move(texTop)) {}

void DoorBlock::gatherTextureNames(std::vector<std::string>& out) const {
    out.push_back(texBottomHalf_);
    out.push_back(texTopHalf_);
}

void DoorBlock::resolveTextures(const TextureArray& tex) {
    layerBottomHalf_ = tex.layer(texBottomHalf_);
    layerTopHalf_ = tex.layer(texTopHalf_);
    layers_.fill(layerBottomHalf_);
}

namespace {
// The door panel's box for a state: closed = on the edge facing the player; open =
// swung to the hinge side (left hinge swings left, right hinge swings right, so
// double doors open apart).
AABB doorPanel(uint16_t data) {
    int facing = (data >> DoorBlock::kFacingShift) & 3;
    bool open = data & DoorBlock::kOpenBit;
    bool hingeRight = data & DoorBlock::kHingeRightBit;
    int edge = open ? (hingeRight ? ((facing + 1) & 3) : ((facing + 3) & 3))
                    : ((facing + 2) & 3); // closed: opposite the facing (near the player)
    switch (edge) {
        case 0: return {{0, 0, 0}, {1, 1, px(3)}};         // panel on -Z edge
        case 1: return {{1.0f - px(3), 0, 0}, {1, 1, 1}};  // +X edge
        case 2: return {{0, 0, 1.0f - px(3)}, {1, 1, 1}};  // +Z edge
        default: return {{0, 0, 0}, {px(3), 1, 1}};        // -X edge
    }
}
} // namespace

void DoorBlock::appendMesh(BlockMeshCtx& ctx) const {
    bool upper = ctx.state.data() & kUpperBit;
    std::array<uint32_t, 6> l;
    l.fill(upper ? layerTopHalf_ : layerBottomHalf_);
    AABB panel = doorPanel(ctx.state.data());
    emitBox(ctx.mesh, ctx.origin, panel.min, panel.max, l, cullMask(ctx, panel.min, panel.max));
}

std::vector<AABB> DoorBlock::collisionBoxes(BlockState s, const World*, const glm::ivec3&) const {
    return {doorPanel(s.data())};
}

std::vector<AABB> DoorBlock::outlineBoxes(BlockState s, const World*, const glm::ivec3&) const {
    return {doorPanel(s.data())};
}

void DoorBlock::appendItemMesh(BlockMeshCtx& ctx) const {
    // Icon/held mesh: the whole door at its TRUE size (2 blocks tall, bottom half under
    // the top half); buildSingleBlock's fit pass scales oversized item meshes into the
    // unit cell. Each half is its own box so both get clean 0..1 UVs.
    std::array<uint32_t, 6> lb, lt;
    lb.fill(layerBottomHalf_);
    lt.fill(layerTopHalf_);
    emitBox(ctx.mesh, ctx.origin, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, px(3)}, lb, kAllFaces);
    emitBox(ctx.mesh, ctx.origin + glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, px(3)}, lt, kAllFaces);
}

BlockState DoorBlock::placementState(const PlacementCtx& ctx) const {
    int facing = facingFromYaw(ctx.playerYawDeg);
    uint16_t data = static_cast<uint16_t>(facing << kFacingShift);
    // Mirror the hinge when a same-facing door sits to the right -> double doors.
    if (ctx.world) {
        glm::ivec3 rightCell = ctx.cell + facingDir((facing + 1) & 3);
        BlockState nb = ctx.world->getState(rightCell.x, rightCell.y, rightCell.z);
        if (nb.id() == id && ((nb.data() >> kFacingShift) & 3) == facing &&
            !(nb.data() & kHingeRightBit)) {
            data |= kHingeRightBit;
        }
    }
    return BlockState(id, data);
}

bool DoorBlock::canPlaceAt(const World& world, const glm::ivec3& cell, BlockState s) const {
    return world.getBlock(cell.x, cell.y + 1, cell.z) == BLOCK_AIR && // room for the top half
           Block::canPlaceAt(world, cell, s);                         // support rules (bottom)
}

bool DoorBlock::canSurviveAt(const World& world, const glm::ivec3& cell, BlockState s) const {
    if (s.data() & kUpperBit) return world.getBlock(cell.x, cell.y - 1, cell.z) == id;
    return Block::canSurviveAt(world, cell, s);
}

void DoorBlock::onPlaced(World& world, const glm::ivec3& cell, BlockState s) const {
    world.setState(cell.x, cell.y + 1, cell.z, s.withData(s.data() | kUpperBit));
}

void DoorBlock::onBroken(World& world, const glm::ivec3& cell, BlockState s) const {
    if (s.data() & kUpperBit) {
        // Breaking the top: pop the lower half so ITS drop (the door item) still spawns.
        if (world.getBlock(cell.x, cell.y - 1, cell.z) == id) {
            world.popBlock({cell.x, cell.y - 1, cell.z});
        }
    } else if (world.getBlock(cell.x, cell.y + 1, cell.z) == id) {
        // Breaking the bottom: the top half drops nothing, remove it silently.
        world.setState(cell.x, cell.y + 1, cell.z, BlockState());
    }
}

bool DoorBlock::onUse(World& world, const glm::ivec3& cell, BlockState s) const {
    BlockState toggled = s.withData(s.data() ^ kOpenBit);
    world.setState(cell.x, cell.y, cell.z, toggled);
    int other = (s.data() & kUpperBit) ? cell.y - 1 : cell.y + 1;
    BlockState o = world.getState(cell.x, other, cell.z);
    if (o.id() == id) {
        world.setState(cell.x, other, cell.z, o.withData(o.data() ^ kOpenBit));
    }
    return true;
}

// -------------------------------------------------------- PressurePlateBlock
void PressurePlateBlock::appendMesh(BlockMeshCtx& ctx) const {
    glm::vec3 from(px(1), 0.0f, px(1)), to(px(15), px(1), px(15));
    emitBox(ctx.mesh, ctx.origin, from, to, layers_, cullMask(ctx, from, to));
}

std::vector<AABB> PressurePlateBlock::outlineBoxes(BlockState, const World*,
                                                   const glm::ivec3&) const {
    return {AABB{{px(1), 0.0f, px(1)}, {px(15), px(1), px(15)}}};
}

} // namespace mc
