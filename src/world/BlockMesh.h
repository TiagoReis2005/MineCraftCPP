#pragma once

#include "world/Block.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace mc {

class BlockRegistry;

struct ChunkVertex {
    glm::vec3 pos;   // local position within the chunk
    glm::vec2 uv;    // 0..1 within the face's texture
    float     layer; // texture-array layer index
    float     light; // baked per-face shading factor
};

struct ChunkMeshData {
    std::vector<ChunkVertex> vertices;
    std::vector<uint32_t>    indices;
    // Tight chunk-local bounds of the emitted vertices (buildCubeMesh fills these). The
    // renderer frustum-culls and occlusion-tests with this box instead of the full cube.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    bool empty() const { return indices.empty(); }
};

// Everything a Block needs to emit its mesh for one cell. `neighbor` looks up the state
// at a relative offset (crosses chunk borders; out-of-world = air).
struct BlockMeshCtx {
    ChunkMeshData& mesh;
    glm::vec3 origin;   // cell position in chunk-local space
    BlockState state;
    const BlockRegistry& reg;
    const std::function<BlockState(int, int, int)>& neighbor;
};

// Face bitmask helpers for emitBox.
constexpr uint8_t faceBit(int face) { return static_cast<uint8_t>(1u << face); }
constexpr uint8_t kAllFaces = 0x3F;

// Emits an axis-aligned box spanning [from,to] (block-local units) at `origin`, using
// per-face texture layers. UVs map to the sub-box extents so partial boxes (slabs,
// stairs, fence arms) sample the matching slice of the block texture. `faceMask`
// selects which faces to emit (skip culled/internal ones).
void emitBox(ChunkMeshData& mesh, const glm::vec3& origin, const glm::vec3& from,
             const glm::vec3& to, const std::array<uint32_t, 6>& layers,
             uint8_t faceMask = kAllFaces);

// Two crossed vertical quads (plants); cutout via texture alpha.
void emitCross(ChunkMeshData& mesh, const glm::vec3& origin, uint32_t layer);

// Neighbor offset per face (FACE_PX..FACE_NZ order).
extern const int kFaceNeighbor[6][3];

} // namespace mc
