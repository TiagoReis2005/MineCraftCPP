#pragma once

#include "core/ThreadPool.h"
#include "world/Chunk.h"
#include "world/LodMesh.h"
#include "world/Mesher.h"
#include "world/WorldGen.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc {

class VkContext;
class BlockRegistry;
class WorldSave;

struct ChunkCoord {
    int x = 0;
    int y = 0;
    int z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(c.x)) * 73856093u) ^
               (static_cast<size_t>(static_cast<uint32_t>(c.y)) * 19349663u) ^
               (static_cast<size_t>(static_cast<uint32_t>(c.z)) * 83492791u);
    }
};

using ChunkMap = std::unordered_map<ChunkCoord, std::shared_ptr<Chunk>, ChunkCoordHash>;

// Streaming world of 32^3 cubic chunks. Generation and meshing run on a worker thread pool;
// the main thread only schedules work and performs batched GPU uploads.
class World {
public:
    // Cubes stacked vertically per column (world height = kVerticalChunks * 32). Columns
    // load/unload as a unit; the renderer hides LOD only under FULLY loaded columns.
    static constexpr int kVerticalChunks = 4;
    void init(BlockRegistry* registry, uint32_t seed = 1337u);
    void update(VkContext& ctx, const glm::vec3& cameraPos);
    void destroy(VkContext& ctx);

    // Attach the save this world persists to (null = throwaway world, e.g. the title
    // panorama). Saved chunks overlay generation; edited chunks write back on unload.
    // Also scans the save's chunk files so guaranteed-air sky cubes with saved edits
    // (towers built high) are still created. Must outlive the world (call destroy()
    // before releasing the save).
    void setSave(WorldSave* save);
    // Write every still-loaded edited chunk to the save (world quit / autosave).
    void flushEdited();

    // Full state at a cell (air outside loaded/generated chunks).
    BlockState getState(int wx, int wy, int wz) const;
    // Convenience: just the block id.
    BlockId getBlock(int wx, int wy, int wz) const { return getState(wx, wy, wz).id(); }

    // True if the AABB [mn,mx] overlaps any block's collision boxes (per-shape; fences
    // extend above their cell). When hit and requested, outTopY/outBottomY receive the
    // highest box top / lowest box bottom overlapped (caller initializes them; used for
    // landing and ceiling snapping).
    bool boxCollides(const glm::vec3& mn, const glm::vec3& mx,
                     float* outTopY = nullptr, float* outBottomY = nullptr) const;

    // Edits a cell in a loaded, ready chunk and flags it (and any touched neighbor) for
    // re-meshing. Returns false if the target chunk isn't ready. Neighbors whose support
    // rules (Properties::needsSupport) stop holding are popped like Minecraft.
    bool setState(int wx, int wy, int wz, BlockState state);
    bool setBlock(int wx, int wy, int wz, BlockId id) { return setState(wx, wy, wz, BlockState(id)); }

    // Remove a block as a CONSEQUENCE of another edit (its support disappeared, a door
    // half following its other half): clears the cell, runs onBroken, and routes the
    // drops through onBlockPopped.
    void popBlock(const glm::ivec3& cell);
    // The session points this at the dropped-item spawner so popped blocks drop as
    // items; null (title panorama world) = pop silently.
    std::function<void(const glm::ivec3&, BlockState)> onBlockPopped;

    const BlockRegistry& registry() const { return *registry_; }

    const ChunkMap& chunks() const { return chunks_; }

    // Far low-detail terrain ("distant horizons"): colored, downsampled column meshes
    // that fill the ring from renderDistance out to lodDistance without loading real
    // chunks. Colors (per BlockId top/side average) come from the texture array; without
    // them (e.g. the title world) LOD stays off.
    void setLodColors(std::vector<glm::vec3> top, std::vector<glm::vec3> side) {
        lodTop_ = std::move(top);
        lodSide_ = std::move(side);
    }
    int lodDistance = 12; // horizontal Chebyshev radius in chunks (>= renderDistance to show)

    struct LodTile {
        LodMesh mesh;
        int cx = 0, cz = 0;
        int step = 0; // detail level this tile was built at (rebuilt when its band changes)
    };
    using LodMap = std::unordered_map<ChunkCoord, LodTile, ChunkCoordHash>;
    const LodMap& lodTiles() const { return lodTiles_; }
    int lodTileCount() const { return static_cast<int>(lodTiles_.size()); }

    int loadedChunkCount() const { return static_cast<int>(chunks_.size()); }
    int readyChunkCount() const {
        int n = 0;
        for (const auto& [coord, chunk] : chunks_) {
            if (chunk->state.load(std::memory_order_relaxed) == CHUNK_READY &&
                chunk->mesh.indexCount > 0) {
                ++n;
            }
        }
        return n;
    }

    int renderDistance = 5; // horizontal Chebyshev radius in chunks

private:
    std::shared_ptr<Chunk> getChunk(int cx, int cy, int cz) const;

    // WorldGen::surfaceCeiling for the column, cached (worldgen is fixed per seed; the
    // cache clears on world switch). Cubes based at/above it are guaranteed-air sky cubes.
    int columnCeiling(int cx, int cz);

    // Rebuild the nearest-first offset ring to the given Chebyshev radius. Called when the
    // render/LOD distance changes so the ring is only as big as currently needed (a large
    // render distance no longer inflates the per-frame scan when distances are small).
    void rebuildRing(int radius);

    void streamChunks(int pcx, int pcz);
    void scheduleMeshing(int pcx, int pcz);
    void collectAndUpload(VkContext& ctx);
    void unloadDistant(int pcx, int pcz);
    void processDeferredDestroys(VkContext& ctx);

    void streamLod(int pcx, int pcz);
    void collectAndUploadLod(VkContext& ctx);
    void unloadLod(int pcx, int pcz);

    BlockRegistry* registry_ = nullptr;
    WorldSave* save_ = nullptr;
    // Chunk coords that have a saved file. Sky cubes (guaranteed air) are normally never
    // created; ones listed here still are, so saved high builds load. Updated on save.
    std::unordered_set<ChunkCoord, ChunkCoordHash> savedChunks_;
    std::unordered_map<uint64_t, int> columnCeiling_; // cached surfaceCeiling per (x,z)
    std::unique_ptr<WorldGen> gen_;
    ChunkMap chunks_;
    ThreadPool pool_;

    std::vector<ChunkCoord> ringOffsets_; // 2D (x,z), sorted nearest-first
    int builtRingRadius_ = -1;            // radius ringOffsets_ currently covers

    // System RAM guard: above ~85% system memory load, generation pauses and unloading
    // speeds up, so a too-large render distance degrades gracefully (world stops growing)
    // instead of paging the machine into a freeze and dying on a failed allocation.
    bool memoryPressure_ = false;
    int memCheckCounter_ = 0;

    // Bounded, nearest-first scheduling: only so many gen/mesh/LOD jobs are ever in the
    // pool at once, refilled nearest-first each frame. This keeps the queue short so the
    // chunks closest to the player (which flying fast most needs) are never stuck behind a
    // huge backlog of now-distant work -- like Minecraft's priority loading.
    std::atomic<int> genInFlight_{0};
    std::atomic<int> meshInFlight_{0};
    int genBudget_ = 32;
    int meshBudget_ = 32;
    int lodBudget_ = 16;

    struct CompletedMesh {
        ChunkCoord coord;
        ChunkMeshData data;
    };
    std::mutex completedMutex_;
    std::vector<CompletedMesh> completed_;

    struct PendingDestroy {
        ChunkMesh mesh;
        int framesLeft;
    };
    std::vector<PendingDestroy> pendingDestroy_;

    // --- Far LOD ---
    std::vector<glm::vec3> lodTop_, lodSide_; // per-BlockId average colors
    LodMap lodTiles_;                          // ready GPU tiles (keyed x,0,z)
    // Coord -> the step (detail level) currently being generated, so a tile isn't queued
    // twice and a band change re-queues it at the new step.
    std::unordered_map<ChunkCoord, int, ChunkCoordHash> lodInFlight_;

    struct CompletedLod {
        ChunkCoord coord;
        LodMeshData data;
        int step;
    };
    std::mutex lodMutex_;
    std::vector<CompletedLod> completedLod_;

    struct PendingLodDestroy {
        LodMesh mesh;
        int framesLeft;
    };
    std::vector<PendingLodDestroy> pendingLodDestroy_;
};

} // namespace mc
