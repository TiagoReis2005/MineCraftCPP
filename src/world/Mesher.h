#pragma once

#include "gfx/GpuBuffer.h"
#include "world/BlockMesh.h"

#include <cstdint>

namespace mc {

class Chunk;
class BlockRegistry;
class VkContext;

struct ChunkMesh {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer indexBuffer;
    uint32_t        indexCount = 0;
    glm::vec3 boundsMin{0.0f}; // tight chunk-local mesh bounds (from ChunkMeshData)
    glm::vec3 boundsMax{0.0f};
    void destroy(VmaAllocator alloc);
};

// Builds CPU-side mesh data for a cubic chunk; each block emits its own geometry via
// Block::appendMesh with neighbor-aware culling. Border cells consult the six axis
// neighbors (null = treated as air). No Vulkan calls, so this runs on worker threads.
// Neighbor order: +X, -X, +Y, -Y, +Z, -Z.
ChunkMeshData buildCubeMesh(const Chunk* self,
                            const Chunk* px, const Chunk* nx,
                            const Chunk* py, const Chunk* ny,
                            const Chunk* pz, const Chunk* nz,
                            const BlockRegistry& reg);

// One block's geometry in isolation (default state, air neighbors) at local origin 0..1.
// Used for inventory icons and the held-block mesh.
ChunkMeshData buildSingleBlock(const BlockRegistry& reg, BlockId id);

// Uploads mesh data to device-local vertex/index buffers (blocking; used outside the
// per-frame path). The streaming world uses a batched upload instead.
ChunkMesh uploadMesh(VkContext& ctx, const ChunkMeshData& data);

} // namespace mc
