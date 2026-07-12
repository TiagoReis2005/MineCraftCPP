#include "world/LodMesh.h"

#include "gfx/GpuBuffer.h"
#include "world/Chunk.h"
#include "world/WorldGen.h"
#include "world/WorldSave.h"

#include <algorithm>
#include <memory>

namespace mc {

void LodMesh::destroy(VmaAllocator alloc) {
    destroyBuffer(alloc, vertexBuffer);
    destroyBuffer(alloc, indexBuffer);
    indexCount = 0;
}

namespace {

// Matches World's kVerticalChunks (128-block-tall columns).
constexpr int kVerticalChunks = 4;

// Directional face shading, matching the world's Minecraft-style scheme so the LOD
// blends with the real chunks at the boundary: top brightest, N/S mid, E/W darkest.
constexpr float kTopShade  = 1.0f;
constexpr float kSideShadeX = 0.6f; // +/-X faces
constexpr float kSideShadeZ = 0.8f; // +/-Z faces

// A macro-cell surface sample: the top y and the block that sits there.
struct Sample {
    int height;
    BlockId surface;
};

glm::vec3 topColor(const std::vector<glm::vec3>& top, BlockId id) {
    return (id < top.size() ? top[id] : glm::vec3(0.5f)) * kTopShade;
}
glm::vec3 sideColor(const std::vector<glm::vec3>& side, BlockId id, float shade) {
    return (id < side.size() ? side[id] : glm::vec3(0.35f)) * shade;
}

// Emit one macro-cell (flat top + skirts down to any lower orthogonal neighbor) given
// its own sample and the four neighbor heights. Appends to m and tracks minY/maxY.
void emitCell(LodMeshData& m, const std::vector<glm::vec3>& top,
              const std::vector<glm::vec3>& side, float x0, float z0, int step,
              const Sample& s, int hpx, int hnx, int hpz, int hnz, float& minY, float& maxY) {
    float x1 = x0 + static_cast<float>(step);
    float z1 = z0 + static_cast<float>(step);
    float fy = static_cast<float>(s.height);
    minY = std::min(minY, fy);
    maxY = std::max(maxY, fy);

    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 col) {
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back({a, col});
        m.vertices.push_back({b, col});
        m.vertices.push_back({c, col});
        m.vertices.push_back({d, col});
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 1);
        m.indices.push_back(base + 2);
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 2);
        m.indices.push_back(base + 3);
    };

    quad({x0, fy, z0}, {x0, fy, z1}, {x1, fy, z1}, {x1, fy, z0}, topColor(top, s.surface));

    glm::vec3 sx = sideColor(side, s.surface, kSideShadeX);
    glm::vec3 sz = sideColor(side, s.surface, kSideShadeZ);
    if (hpx < s.height) { float b = static_cast<float>(hpx);
        quad({x1, fy, z0}, {x1, fy, z1}, {x1, b, z1}, {x1, b, z0}, sx); minY = std::min(minY, b); }
    if (hnx < s.height) { float b = static_cast<float>(hnx);
        quad({x0, fy, z1}, {x0, fy, z0}, {x0, b, z0}, {x0, b, z1}, sx); minY = std::min(minY, b); }
    if (hpz < s.height) { float b = static_cast<float>(hpz);
        quad({x0, fy, z1}, {x1, fy, z1}, {x1, b, z1}, {x0, b, z1}, sz); minY = std::min(minY, b); }
    if (hnz < s.height) { float b = static_cast<float>(hnz);
        quad({x1, fy, z0}, {x0, fy, z0}, {x0, b, z0}, {x1, b, z0}, sz); minY = std::min(minY, b); }
}

// Cheap path: the surface is the worldgen height field; each cell samples its center and
// its four neighbor centers (globally, so tiles are seamless).
LodMeshData buildFromWorldGen(const WorldGen& gen, const std::vector<glm::vec3>& top,
                              const std::vector<glm::vec3>& side, int cx, int cz, int step) {
    const int S = Chunk::kSize;
    const int n = std::max(1, S / step);
    const int baseX = cx * S, baseZ = cz * S;
    LodMeshData m;
    float minY = 1e9f, maxY = -1e9f;

    auto sampleAt = [&](int i, int j) -> Sample {
        int h = gen.heightAt(baseX + i * step + step / 2, baseZ + j * step + step / 2);
        return {h, gen.surfaceBlock(h)};
    };
    auto heightAt = [&](int i, int j) {
        return gen.heightAt(baseX + i * step + step / 2, baseZ + j * step + step / 2);
    };

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            Sample s = sampleAt(i, j);
            emitCell(m, top, side, static_cast<float>(baseX + i * step),
                     static_cast<float>(baseZ + j * step), step, s,
                     heightAt(i + 1, j), heightAt(i - 1, j), heightAt(i, j + 1),
                     heightAt(i, j - 1), minY, maxY);
        }
    }
    m.minY = (minY > maxY) ? 0.0f : minY;
    m.maxY = (minY > maxY) ? 0.0f : maxY;
    return m;
}

// Real-data path: reconstruct the column's actual blocks (worldgen + saved edits) so
// player-modified terrain shows at distance. Scans each cell's representative column for
// the highest non-air block. Cells outside the tile (for edge skirts) fall back to the
// worldgen height field.
LodMeshData buildFromChunks(const WorldGen& gen, const WorldSave& save,
                            const std::vector<glm::vec3>& top,
                            const std::vector<glm::vec3>& side, int cx, int cz, int step) {
    const int S = Chunk::kSize;
    const int n = std::max(1, S / step);
    const int baseX = cx * S, baseZ = cz * S;

    // Build the real block cubes for this footprint (generation overlaid with saves).
    // Chunk holds atomics (not movable), so hold them by pointer.
    std::vector<std::unique_ptr<Chunk>> cubes;
    cubes.reserve(kVerticalChunks);
    for (int cy = 0; cy < kVerticalChunks; ++cy) {
        auto c = std::make_unique<Chunk>(cx, cy, cz);
        gen.generate(*c);
        save.loadChunk(cx, cy, cz, c->rawBlocks().data(), c->rawBlocks().size());
        cubes.push_back(std::move(c));
    }

    // Scan the representative column (cell center) top-down for the highest non-air block.
    auto scan = [&](int i, int j) -> Sample {
        int lx = std::min(S - 1, i * step + step / 2);
        int lz = std::min(S - 1, j * step + step / 2);
        for (int worldY = kVerticalChunks * S - 1; worldY >= 0; --worldY) {
            int cy = worldY / S, ly = worldY % S;
            BlockState b = cubes[cy]->get(lx, ly, lz);
            if (!b.isAir()) return {worldY + 1, b.id()};
        }
        int h = gen.heightAt(baseX + lx, baseZ + lz);
        return {h, gen.surfaceBlock(h)};
    };

    std::vector<Sample> cells(static_cast<size_t>(n) * n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) cells[j * n + i] = scan(i, j);

    // Neighbor height: interior from the scan, tile-edge from the worldgen field.
    auto heightAt = [&](int i, int j) {
        if (i >= 0 && i < n && j >= 0 && j < n) return cells[j * n + i].height;
        return gen.heightAt(baseX + i * step + step / 2, baseZ + j * step + step / 2);
    };

    LodMeshData m;
    float minY = 1e9f, maxY = -1e9f;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            emitCell(m, top, side, static_cast<float>(baseX + i * step),
                     static_cast<float>(baseZ + j * step), step, cells[j * n + i],
                     heightAt(i + 1, j), heightAt(i - 1, j), heightAt(i, j + 1),
                     heightAt(i, j - 1), minY, maxY);
        }
    }
    m.minY = (minY > maxY) ? 0.0f : minY;
    m.maxY = (minY > maxY) ? 0.0f : maxY;
    return m;
}

} // namespace

LodMeshData buildLodTile(const WorldGen& gen, const WorldSave* save,
                         const std::vector<glm::vec3>& topColors,
                         const std::vector<glm::vec3>& sideColors,
                         int cx, int cz, int step, bool chunkSourced) {
    // Only pay for real block reconstruction where the player actually edited terrain;
    // everywhere else the worldgen height field is identical and far cheaper.
    if (chunkSourced && save) {
        bool edited = false;
        for (int cy = 0; cy < kVerticalChunks && !edited; ++cy) edited = save->hasChunk(cx, cy, cz);
        if (edited) return buildFromChunks(gen, *save, topColors, sideColors, cx, cz, step);
    }
    return buildFromWorldGen(gen, topColors, sideColors, cx, cz, step);
}

} // namespace mc
