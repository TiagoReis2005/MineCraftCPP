#pragma once

#include "world/ModelDisplay.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

class World;
class BlockRegistry;
class TextureArray;
struct BlockMeshCtx;
struct ChunkMeshData;

// Creative-inventory tabs (Properties::tab; the screen shows one page per tab).
constexpr uint8_t kTabBuilding = 0;
constexpr uint8_t kTabNature = 1;
constexpr uint8_t kTabFunctional = 2;
constexpr int kTabCount = 3;

// Container screens a block opens on right-click (Properties::container). Furnace and
// chest are declared ahead of their blocks/screens so recipes and UIs can key on them.
constexpr uint8_t kContainerNone = 0;
constexpr uint8_t kContainerCrafting = 1;
constexpr uint8_t kContainerFurnace = 2;
constexpr uint8_t kContainerChest = 3;

// A block TYPE is identified by a runtime handle (index into the registry). 0 is air.
using BlockId = uint16_t;
constexpr BlockId BLOCK_AIR = 0;

// Face indices; winding/UV live in the mesh emit helpers.
enum Face : int {
    FACE_PX = 0, // +X (east)
    FACE_NX,     // -X (west)
    FACE_PY,     // +Y (top)
    FACE_NY,     // -Y (bottom)
    FACE_PZ,     // +Z (south)
    FACE_NZ      // -Z (north)
};

// ---------------------------------------------------------------------------------------
// BlockState: what a world cell actually stores. Low 16 bits = BlockId, high 16 bits =
// per-block data (facing, half, open, layers, ...). Data bit 15 is reserved engine-wide
// for "waterlogged" so fluids can be added later without another storage change.
// ---------------------------------------------------------------------------------------
struct BlockState {
    uint32_t raw = 0;

    constexpr BlockState() = default;
    constexpr BlockState(BlockId id, uint16_t data = 0) // implicit from BlockId on purpose
        : raw(static_cast<uint32_t>(id) | (static_cast<uint32_t>(data) << 16)) {}

    static constexpr BlockState fromRaw(uint32_t r) {
        BlockState s;
        s.raw = r;
        return s;
    }

    constexpr BlockId  id() const { return static_cast<BlockId>(raw & 0xFFFFu); }
    constexpr uint16_t data() const { return static_cast<uint16_t>(raw >> 16); }
    constexpr bool     isAir() const { return id() == BLOCK_AIR; }

    constexpr bool operator==(const BlockState& o) const { return raw == o.raw; }
    constexpr bool operator!=(const BlockState& o) const { return raw != o.raw; }

    static constexpr uint16_t kWaterloggedBit = 1u << 15;
    constexpr bool waterlogged() const { return (data() & kWaterloggedBit) != 0; }
    constexpr BlockState withData(uint16_t d) const { return BlockState(id(), d); }
    constexpr BlockState withWaterlogged(bool on) const {
        return withData(on ? (data() | kWaterloggedBit) : (data() & ~kWaterloggedBit));
    }
};

// Axis-aligned box in block-local space (block units; a cell spans 0..1, but boxes may
// exceed it — fence collision reaches y=1.5).
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{1.0f};
};

// One axis-aligned outline edge (block-local). The renderer draws each as a thin bar.
struct Segment {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
};

enum class ToolType : uint8_t { None, Pickaxe, Axe, Shovel, Hoe, Shears, Sword };

// BLOCK-LOCAL sides for placement-support rules. Bottom/Top are absolute; the horizontal
// sides rotate with the block's facing (Block::facingOf): North is the block's FRONT,
// South its back, East its right / West its left (seen from above, facing its front).
// For an unrotated block (facing 0) local sides equal world compass directions.
enum class BlockSide : uint8_t { Bottom, Top, North, South, West, East };

// One placement-support requirement: the neighbor cell on `side` must qualify. With no
// filter (blocks and tags both empty) any neighbor offering a sturdy FULL face toward
// the block qualifies (full cubes, top-slab tops; not fence posts). With a filter, the
// neighbor's block name or one of its tags must match — its shape then doesn't matter.
struct SupportRule {
    BlockSide side = BlockSide::Bottom;
    std::vector<std::string> blocks; // qualifying block names ("grass", "dirt")
    std::vector<std::string> tags;   // qualifying block tags ("logs", "planks")
};

// One entry of a block's drop pool (items don't exist yet as entities; this is the data
// model that mining will hand to the inventory system later).
struct Drop {
    std::string item;     // dropped block/item name
    int   min = 1;
    int   max = 1;
    float chance = 1.0f;  // roll probability
};

// Everything the player/placement code knows at placement time.
struct PlacementCtx {
    glm::ivec3 cell{0};        // where the new block goes
    glm::ivec3 clickedCell{0}; // the existing block that was clicked
    glm::ivec3 normal{0};      // clicked face normal
    glm::vec3  hitPoint{0.0f}; // world-space point on the clicked face
    float playerYawDeg = 0.0f; // camera yaw (deg, -90 = looking -Z)
    bool  sneaking = false;
    const World* world = nullptr; // for neighbor-aware placement (double doors)
};

// ---------------------------------------------------------------------------------------
// Block: one instance per block TYPE, registered once; behavior via virtuals (Java-style).
// The three geometries are independent: appendMesh (visuals), collisionBoxes (physics),
// outlineBoxes (selection wireframe).
// ---------------------------------------------------------------------------------------
class Block {
public:
    struct Properties {
        float    strength = 1.0f;      // seconds of bare-hand mining at 1x (<0 = unbreakable)
        ToolType tool = ToolType::None; // tool class that mines this fastest
        bool     requiresTool = false; // without the right tool: slower + no drops
        bool     solid = true;         // participates in collision at all
        bool     opaque = true;        // full opaque cube: hides all neighbor faces
        bool     cullSame = false;     // hide faces shared with the same block (glass)
        bool     replaceable = false;  // placement may overwrite this block (air, plants)
        bool     hiddenFromHotbar = false;
        uint8_t  tab = 0;              // creative-inventory tab (kTab* below)
        uint8_t  container = 0;        // kContainer*: right-click opens this screen
        int      lightLevel = 0;       // future lighting
        float    fuelSeconds = 0.0f;   // furnace burn time (>0 implies the "fuel" tag)
        std::string sound = "stone";   // future sound type
        std::vector<std::string> tags; // block tags ("logs", "planks", "fuel"...); the
                                       // registry indexes them for recipes/furnace/etc.
        // Placement support (empty = the block can float). Rules are OR'd: the block
        // places/survives when ANY rule is satisfied; when support disappears the world
        // pops the block as a drop instead of leaving it floating.
        std::vector<SupportRule> support;

        Properties& strengthIs(float s) { strength = s; return *this; }
        Properties& needs(ToolType t) { tool = t; requiresTool = true; return *this; }
        Properties& prefers(ToolType t) { tool = t; return *this; }
        Properties& notSolid() { solid = false; return *this; }
        Properties& transparent() { opaque = false; return *this; }
        Properties& cullsSelf() { cullSame = true; return *this; }
        Properties& canReplace() { replaceable = true; return *this; }
        Properties& hidden() { hiddenFromHotbar = true; return *this; }
        Properties& light(int level) { lightLevel = level; return *this; }
        Properties& soundIs(std::string s) { sound = std::move(s); return *this; }
        Properties& inTab(uint8_t t) { tab = t; return *this; }
        Properties& opens(uint8_t kind) { container = kind; return *this; }
        Properties& tag(const char* t) { tags.emplace_back(t); return *this; }
        // Usable as furnace fuel for this many seconds (also applies the "fuel" tag).
        Properties& burns(float seconds) { fuelSeconds = seconds; return tag("fuel"); }
        // Cannot float: needs a sturdy full face on this block-local side.
        Properties& needsSupport(BlockSide side) {
            support.push_back({side, {}, {}});
            return *this;
        }
        // Cannot float: needs one of these blocks/tags on this block-local side.
        Properties& needsSupport(BlockSide side, std::vector<std::string> blocks,
                                 std::vector<std::string> tags = {}) {
            support.push_back({side, std::move(blocks), std::move(tags)});
            return *this;
        }
    };

    Block(std::string name, Properties props) : name_(std::move(name)), props_(std::move(props)) {}
    virtual ~Block() = default;

    BlockId id = 0; // assigned by the registry

    const std::string& name() const { return name_; }
    const Properties&  props() const { return props_; }

    // ---- textures (folder-based pipeline: names resolve to texture-array layers) ----
    Block& allFaces(std::string t) { texTop_ = texBottom_ = texSide_ = std::move(t); return *this; }
    Block& faces(std::string top, std::string bottom, std::string side) {
        texTop_ = std::move(top); texBottom_ = std::move(bottom); texSide_ = std::move(side);
        return *this;
    }
    Block& column(std::string ends, std::string side) {
        texTop_ = texBottom_ = std::move(ends); texSide_ = std::move(side);
        return *this;
    }
    virtual void gatherTextureNames(std::vector<std::string>& out) const;
    virtual void resolveTextures(const TextureArray& tex);
    const std::array<uint32_t, 6>& faceLayers() const { return layers_; }

    // ---- state ----
    virtual BlockState defaultState() const { return BlockState(id); }

    // ---- the three geometries ----
    // Visual mesh for one cell (default: neighbor-culled full cube).
    virtual void appendMesh(BlockMeshCtx& ctx) const;
    // Mesh for icons / the held block (default: the in-world mesh with air neighbors).
    virtual void appendItemMesh(BlockMeshCtx& ctx) const { appendMesh(ctx); }
    // Physics boxes, block-local (default: full cube; empty when not solid). `world`/
    // `cell` allow neighbor-aware shapes (fence arms); world may be null (icons, guards
    // without context) in which case the standalone shape is returned.
    virtual std::vector<AABB> collisionBoxes(BlockState state, const World* world,
                                             const glm::ivec3& cell) const;
    // Boxes the SUPPORT sturdiness check (needsSupport) tests: what neighbors may stand
    // on. Defaults to collision; blocks whose support differs override (a full 8-layer
    // snow stack supports like a full cube even though its collision tops out at 7/8).
    virtual std::vector<AABB> supportBoxes(BlockState state, const World* world,
                                           const glm::ivec3& cell) const {
        return collisionBoxes(state, world, cell);
    }
    // Selection outline boxes, block-local (default: same as collision, or full cube).
    // These drive TARGETING (raycast); the drawn wireframe uses outlineEdges.
    virtual std::vector<AABB> outlineBoxes(BlockState state, const World* world,
                                           const glm::ivec3& cell) const;
    // The wireframe edges actually drawn (default: 12 edges per outline box). Compound
    // shapes override this with their union silhouette (stairs: L-prism, no inner seams).
    virtual std::vector<Segment> outlineEdges(BlockState state, const World* world,
                                              const glm::ivec3& cell) const;
    // Outline for what the crosshair actually AIMS at (default: the whole outline).
    // Blocks whose parts break independently narrow it (a double slab outlines only the
    // aimed half, matching what breakResult would remove).
    virtual std::vector<Segment> aimedOutlineEdges(BlockState state, const World* world,
                                                   const glm::ivec3& cell,
                                                   const glm::vec3& hitPoint,
                                                   const glm::ivec3& normal) const {
        (void)hitPoint; (void)normal;
        return outlineEdges(state, world, cell);
    }
    // The 12 edges of one box (helper for outlineEdges overrides).
    static void appendBoxEdges(const AABB& box, std::vector<Segment>& out);
    // Boxes the mining crack overlay wraps, aim-aware. Defaults to the OUTLINE boxes
    // (the visual silhouette — NOT collision, so a fence cracks over its in-cell shape
    // instead of its 1.5-tall collision volume). A double slab cracks the aimed half.
    virtual std::vector<AABB> crackBoxes(BlockState state, const World* world,
                                         const glm::ivec3& cell, const glm::vec3& hitPoint,
                                         const glm::ivec3& normal) const {
        (void)hitPoint; (void)normal;
        return outlineBoxes(state, world, cell);
    }
    // Which independently-breakable PART the crosshair points at (0 = whole block).
    // Digging a different part restarts break progress — a double slab's two halves
    // don't share it.
    virtual uint16_t aimedPart(BlockState, const glm::ivec3& cell, const glm::vec3& hitPoint,
                               const glm::ivec3& normal) const {
        (void)cell; (void)hitPoint; (void)normal;
        return 0;
    }
    // Whether this state hides the touching face of a neighboring cube.
    virtual bool isOpaque(BlockState) const { return props_.opaque; }
    // Whether fences/walls draw a connecting arm toward this state.
    virtual bool connectsToFence(BlockState s) const { return isOpaque(s); }

    // ---- item display (the model json's "display" table) ----
    // Model whose display transforms orient this block in the gui slot and in the hand
    // (fences point at their _inventory model). Resolved once by the registry.
    virtual std::string itemModelName() const { return name_; }
    const DisplaySet& display() const { return display_; }
    void setDisplay(const DisplaySet& d) { display_ = d; }

    // ---- placement / interaction ----
    virtual BlockState placementState(const PlacementCtx&) const { return defaultState(); }
    // Merge into the CLICKED cell instead of placing into the empty one (slab -> double
    // slab, snow layers). Returns true and fills `out` when it applies.
    virtual bool tryMergeInto(const BlockRegistry&, BlockState clicked, const PlacementCtx&,
                              BlockState* out) const {
        (void)clicked; (void)out;
        return false;
    }
    // Placement landed on a cell that already holds `existing` (e.g. aiming through a
    // slab's empty half at the block behind): can this item fill into it? (slab ->
    // double slab, snow -> +1 layer.) The ctx says which part the click points at, so a
    // slab only combines when the aimed half is actually the empty one.
    virtual bool tryCombineAt(const BlockRegistry&, BlockState existing, const PlacementCtx&,
                              BlockState* out) const {
        (void)existing; (void)out;
        return false;
    }
    // Yaw facing baked in the state (0 = -Z north, 1 = +X east, 2 = +Z south, 3 = -X
    // west); rotates the block-local sides of support rules. Oriented blocks override.
    virtual int facingOf(BlockState) const { return 0; }
    // Default: the Properties support rules hold (no rules = placeable anywhere).
    virtual bool canPlaceAt(const World& world, const glm::ivec3& cell, BlockState state) const {
        return canSurviveAt(world, cell, state);
    }
    // Whether an already-placed state keeps standing here. Same support check as
    // placement by default; the world re-asks this on every neighbor change and pops the
    // block when it fails. Split from canPlaceAt so placement-only conditions (a door
    // needs free headroom) don't evict existing blocks.
    virtual bool canSurviveAt(const World& world, const glm::ivec3& cell, BlockState state) const;
    virtual void onPlaced(World&, const glm::ivec3&, BlockState) const {}
    // Right-click interaction (doors/gates toggle). True = handled, don't place.
    virtual bool onUse(World&, const glm::ivec3&, BlockState) const { return false; }

    // ---- mining ----
    // What remains in the cell after this state is broken (AIR normally; a double slab
    // keeps its other half — hitPoint/normal say which half was aimed at).
    virtual BlockState breakResult(BlockState, const glm::vec3& hitPoint,
                                   const glm::ivec3& cell, const glm::ivec3& normal) const {
        (void)hitPoint; (void)cell; (void)normal;
        return BlockState();
    }
    virtual void onBroken(World&, const glm::ivec3&, BlockState) const {} // doors: other half
    // Seconds to mine with the given tool (MC-style: wrong tool on a requiresTool block
    // is 5x slower and yields no drops). <0 = unbreakable.
    float breakSeconds(ToolType held) const;
    virtual std::vector<Drop> drops(BlockState, ToolType held) const; // default: itself x1

protected:
    std::string name_;
    Properties  props_;
    std::string texTop_, texBottom_, texSide_;
    std::array<uint32_t, 6> layers_{};
    DisplaySet  display_{}; // gui/hand orientation from the model json
};

} // namespace mc
