#pragma once

#include "gfx/GpuBuffer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace mc {

class WorldGen;
class WorldSave;

// Vertex for the far-distance LOD terrain: a world-space position and a baked linear
// color (average block color * face shade). No texture sampling — this is the cheap
// "fake horizon" that stands in for real chunks past the render distance.
struct LodVertex {
    glm::vec3 pos;
    glm::vec3 color;
};

// CPU-side LOD tile mesh (built on a worker thread; no Vulkan calls).
struct LodMeshData {
    std::vector<LodVertex> vertices;
    std::vector<uint32_t>  indices;
    float minY = 0.0f;
    float maxY = 0.0f;
    bool empty() const { return indices.empty(); }
};

// GPU-resident LOD tile (one per far chunk-column).
struct LodMesh {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer indexBuffer;
    uint32_t indexCount = 0;
    float minY = 0.0f;
    float maxY = 0.0f;
    void destroy(VmaAllocator alloc);
};

// Builds a downsampled, flat-colored surface mesh for the chunk-column footprint at
// (cx,cz): the surface height is sampled every `step` blocks into macro-cells, each a
// flat top quad plus vertical "skirt" faces dropping to any lower orthogonal neighbor
// (so cliffs/mountainsides show). topColors/sideColors are indexed by BlockId. `step`
// selects the detail level (smaller = finer, used for nearer tiles).
//
// When `chunkSourced` is set and `save` holds edited chunks for this column, the tile is
// built from the REAL block data (worldgen + saved edits), so player edits show at
// distance; otherwise it falls back to the cheap worldgen height field. Adjacent tiles
// stay seamless because shared-edge heights come from the same global field.
LodMeshData buildLodTile(const WorldGen& gen, const WorldSave* save,
                         const std::vector<glm::vec3>& topColors,
                         const std::vector<glm::vec3>& sideColors,
                         int cx, int cz, int step, bool chunkSourced);

} // namespace mc
