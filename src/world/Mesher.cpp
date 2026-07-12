#include "world/Mesher.h"

#include "gfx/VkContext.h"
#include "world/BlockRegistry.h"
#include "world/Chunk.h"

namespace mc {

ChunkMeshData buildCubeMesh(const Chunk* self,
                            const Chunk* px, const Chunk* nx,
                            const Chunk* py, const Chunk* ny,
                            const Chunk* pz, const Chunk* nz,
                            const BlockRegistry& reg) {
    ChunkMeshData mesh;
    const int N = Chunk::kSize;

    // Only one coordinate is ever out of range per lookup (mesh boxes stay inside the
    // cell), so pick the matching neighbor cube and index directly.
    auto stateAt = [&](int x, int y, int z) -> BlockState {
        if (x < 0) return nx ? nx->get(N - 1, y, z) : BlockState();
        if (x >= N) return px ? px->get(0, y, z) : BlockState();
        if (y < 0) return ny ? ny->get(x, N - 1, z) : BlockState();
        if (y >= N) return py ? py->get(x, 0, z) : BlockState();
        if (z < 0) return nz ? nz->get(x, y, N - 1) : BlockState();
        if (z >= N) return pz ? pz->get(x, y, 0) : BlockState();
        return self->get(x, y, z);
    };

    int cx = 0, cy = 0, cz = 0; // current cell for the shared neighbor closure
    const std::function<BlockState(int, int, int)> neighbor = [&](int dx, int dy, int dz) {
        return stateAt(cx + dx, cy + dy, cz + dz);
    };

    for (int y = 0; y < N; ++y) {
        for (int z = 0; z < N; ++z) {
            for (int x = 0; x < N; ++x) {
                BlockState state = self->get(x, y, z);
                if (state.isAir()) continue;
                cx = x; cy = y; cz = z;
                BlockMeshCtx ctx{mesh,
                                 glm::vec3(static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(z)),
                                 state, reg, neighbor};
                reg.block(state).appendMesh(ctx);
            }
        }
    }
    // Tight bounds of what was actually emitted: terrain usually fills only a slice of
    // the cube, so culling/occlusion boxes hug the mesh instead of the full 32^3 volume.
    if (!mesh.vertices.empty()) {
        glm::vec3 mn = mesh.vertices[0].pos, mx = mn;
        for (const ChunkVertex& v : mesh.vertices) {
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
        mesh.boundsMin = mn;
        mesh.boundsMax = mx;
    }
    return mesh;
}

ChunkMeshData buildSingleBlock(const BlockRegistry& reg, BlockId id) {
    ChunkMeshData mesh;
    const Block& b = reg.block(id);
    static const std::function<BlockState(int, int, int)> kAirNeighbors =
        [](int, int, int) { return BlockState(); };
    BlockMeshCtx ctx{mesh, glm::vec3(0.0f), b.defaultState(), reg, kAirNeighbors};
    b.appendItemMesh(ctx);

    // Multi-cell item meshes (doors are 2 tall, beds 2 long) get uniformly scaled and
    // centered into the unit cell so icons and the held block render at a sane size.
    // Meshes already within the cell are left untouched.
    if (!mesh.vertices.empty()) {
        glm::vec3 mn(1e9f), mx(-1e9f);
        for (const ChunkVertex& v : mesh.vertices) {
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
        glm::vec3 size = mx - mn;
        float maxDim = std::max(size.x, std::max(size.y, size.z));
        if (maxDim > 1.0f + 1e-4f) {
            float s = 1.0f / maxDim;
            glm::vec3 offset = (glm::vec3(1.0f) - size * s) * 0.5f - mn * s;
            for (ChunkVertex& v : mesh.vertices) v.pos = v.pos * s + offset;
        }
    }
    return mesh;
}

ChunkMesh uploadMesh(VkContext& ctx, const ChunkMeshData& data) {
    ChunkMesh mesh;
    mesh.indexCount = static_cast<uint32_t>(data.indices.size());
    mesh.boundsMin = data.boundsMin;
    mesh.boundsMax = data.boundsMax;
    if (data.empty()) return mesh;

    mesh.vertexBuffer = createDeviceBufferWithData(
        ctx, data.vertices.data(), data.vertices.size() * sizeof(ChunkVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mesh.indexBuffer = createDeviceBufferWithData(
        ctx, data.indices.data(), data.indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    return mesh;
}

void ChunkMesh::destroy(VmaAllocator alloc) {
    destroyBuffer(alloc, vertexBuffer);
    destroyBuffer(alloc, indexBuffer);
    indexCount = 0;
}

} // namespace mc
