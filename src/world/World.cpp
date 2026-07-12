#include "world/World.h"

#include "core/SystemStats.h"
#include "gfx/GpuBuffer.h"
#include "gfx/VkContext.h"
#include "world/BlockRegistry.h"
#include "world/WorldSave.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace mc {
namespace {

constexpr int kLodCap = 64;              // hard ceiling on lodDistance (chunks)
constexpr int kUploadBudget = 8;         // chunk meshes uploaded per frame
constexpr int kLodUploadBudget = 6;      // LOD tiles uploaded per frame
constexpr int kDestroyDelayFrames = 3;   // > frames-in-flight before freeing GPU buffers

int floorDiv(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

int cheb(int dx, int dz) {
    return std::max(std::abs(dx), std::abs(dz));
}

bool atLeastGenerated(const std::shared_ptr<Chunk>& c) {
    return c && c->state.load(std::memory_order_acquire) >= CHUNK_GENERATED;
}

// LOD detail level for a tile at Chebyshev distance `d` (chunks) from the player. The
// nearest band is BLOCK resolution (step 1): the exact terrain silhouette, just flat
// "cooked"-color instead of textured, so the handoff from HD chunks is seamless; it is a
// fixed 4-chunk ring to bound its (much higher) vertex cost, and reads real block data so
// player edits show. Farther bands coarsen (8/16/32) with thresholds scaled to the LOD
// range, so the levels sit far apart at large view distances.
void lodLevelFor(int d, int renderDistance, int lodDistance, int& step, bool& chunkSourced) {
    int range = std::max(1, lodDistance - renderDistance);
    int t0 = renderDistance + 4;                            // block-perfect ring
    int t1 = renderDistance + std::max(6, range * 2 / 5);   // ~40%
    int t2 = renderDistance + std::max(8, range * 7 / 10);  // ~70%
    if (d <= t0) {
        step = 1;
        chunkSourced = true; // show player edits close to the render boundary
    } else if (d <= t1) {
        step = 8;
        chunkSourced = false;
    } else if (d <= t2) {
        step = 16;
        chunkSourced = false;
    } else {
        step = 32;
        chunkSourced = false;
    }
}

} // namespace

void World::init(BlockRegistry* registry, uint32_t seed) {
    registry_ = registry;
    save_ = nullptr; // attach per-world via setSave after init
    gen_ = std::make_unique<WorldGen>(seed, *registry);
    pool_.start();

    // Keep enough jobs queued to saturate the workers, but no more (so the queue stays
    // short and nearest-first). Reset counters: stop() may have dropped queued jobs whose
    // in-flight increment never got decremented.
    int t = static_cast<int>(pool_.threadCount());
    genBudget_ = std::max(8, t * 4);
    meshBudget_ = std::max(8, t * 4);
    lodBudget_ = std::max(4, t * 2);
    genInFlight_.store(0, std::memory_order_relaxed);
    meshInFlight_.store(0, std::memory_order_relaxed);

    builtRingRadius_ = -1; // force a rebuild; init() runs again on every world switch
    savedChunks_.clear();  // repopulated by setSave (title worlds have no save)
    columnCeiling_.clear(); // per-seed cache
    rebuildRing(std::max(renderDistance + 1, std::min(lodDistance, kLodCap)));
}

void World::setSave(WorldSave* save) {
    save_ = save;
    savedChunks_.clear();
    if (save_) {
        for (const glm::ivec3& c : save_->listChunks()) savedChunks_.insert({c.x, c.y, c.z});
    }
}

int World::columnCeiling(int cx, int cz) {
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                   static_cast<uint32_t>(cz);
    auto it = columnCeiling_.find(key);
    if (it != columnCeiling_.end()) return it->second;
    int c = gen_->surfaceCeiling(cx, cz);
    columnCeiling_[key] = c;
    return c;
}

void World::rebuildRing(int radius) {
    if (radius == builtRingRadius_) return;
    builtRingRadius_ = radius;
    ringOffsets_.clear();
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            ringOffsets_.push_back({dx, 0, dz});
        }
    }
    std::sort(ringOffsets_.begin(), ringOffsets_.end(), [](ChunkCoord a, ChunkCoord b) {
        return (a.x * a.x + a.z * a.z) < (b.x * b.x + b.z * b.z);
    });
}

std::shared_ptr<Chunk> World::getChunk(int cx, int cy, int cz) const {
    auto it = chunks_.find({cx, cy, cz});
    return it == chunks_.end() ? nullptr : it->second;
}

BlockState World::getState(int wx, int wy, int wz) const {
    const int N = Chunk::kSize;
    if (wy < 0 || wy >= kVerticalChunks * N) return BlockState();
    int cx = floorDiv(wx, N), cy = floorDiv(wy, N), cz = floorDiv(wz, N);
    auto c = getChunk(cx, cy, cz);
    if (!atLeastGenerated(c)) return BlockState();
    return c->get(wx - cx * N, wy - cy * N, wz - cz * N);
}

bool World::boxCollides(const glm::vec3& mn, const glm::vec3& mx,
                        float* outTopY, float* outBottomY) const {
    bool hitAny = false;
    int minX = static_cast<int>(std::floor(mn.x)), maxX = static_cast<int>(std::floor(mx.x));
    int minY = static_cast<int>(std::floor(mn.y)), maxY = static_cast<int>(std::floor(mx.y));
    int minZ = static_cast<int>(std::floor(mn.z)), maxZ = static_cast<int>(std::floor(mx.z));
    // Collision boxes may extend above their cell (fences are 1.5 tall), so also test
    // one extra layer below the query box.
    minY -= 1;
    for (int y = minY; y <= maxY; ++y) {
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                BlockState st = getState(x, y, z);
                if (st.isAir() || !registry_->isSolid(st)) continue;
                glm::vec3 cell(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                for (const AABB& box : registry_->block(st).collisionBoxes(st, this, {x, y, z})) {
                    glm::vec3 b0 = cell + box.min;
                    glm::vec3 b1 = cell + box.max;
                    if (mx.x <= b0.x || mn.x >= b1.x || mx.y <= b0.y || mn.y >= b1.y ||
                        mx.z <= b0.z || mn.z >= b1.z) {
                        continue;
                    }
                    hitAny = true;
                    if (outTopY && b1.y > *outTopY) *outTopY = b1.y;
                    if (outBottomY && b0.y < *outBottomY) *outBottomY = b0.y;
                    if (!outTopY && !outBottomY) return true;
                }
            }
        }
    }
    return hitAny;
}

bool World::setState(int wx, int wy, int wz, BlockState state) {
    const int N = Chunk::kSize;
    if (wy < 0 || wy >= kVerticalChunks * N) return false;
    int cx = floorDiv(wx, N), cy = floorDiv(wy, N), cz = floorDiv(wz, N);
    auto c = getChunk(cx, cy, cz);
    if (!c && cy * N >= columnCeiling(cx, cz)) {
        // Building above the terrain ceiling: sky cubes aren't created by streaming, so
        // materialize an all-air, already-READY cube on demand (empty mesh; the edit
        // below marks it dirty and the normal remesh path picks it up).
        c = std::make_shared<Chunk>(cx, cy, cz);
        c->state.store(CHUNK_READY, std::memory_order_release);
        chunks_[{cx, cy, cz}] = c;
    }
    if (!c || c->state.load(std::memory_order_acquire) != CHUNK_READY) return false;

    int lx = wx - cx * N, ly = wy - cy * N, lz = wz - cz * N;
    c->set(lx, ly, lz, state);
    c->dirty.store(true, std::memory_order_release);
    c->edited.store(true, std::memory_order_release); // persists on unload/flush

    // A change on a chunk face also alters the touching neighbor's border faces.
    auto markDirty = [&](int ncx, int ncy, int ncz) {
        auto n = getChunk(ncx, ncy, ncz);
        if (n && n->state.load(std::memory_order_acquire) == CHUNK_READY) {
            n->dirty.store(true, std::memory_order_release);
        }
    };
    if (lx == 0) markDirty(cx - 1, cy, cz);
    if (lx == N - 1) markDirty(cx + 1, cy, cz);
    if (ly == 0) markDirty(cx, cy - 1, cz);
    if (ly == N - 1) markDirty(cx, cy + 1, cz);
    if (lz == 0) markDirty(cx, cy, cz - 1);
    if (lz == N - 1) markDirty(cx, cy, cz + 1);

    // Neighbors that relied on this cell for support (Properties::needsSupport) may no
    // longer stand — pop them like Minecraft instead of leaving them floating. Pops
    // recurse through setState, so chains (snow on a door on a mined block) resolve.
    static const int kSides[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& d : kSides) {
        glm::ivec3 nc(wx + d[0], wy + d[1], wz + d[2]);
        BlockState ns = getState(nc.x, nc.y, nc.z);
        if (ns.isAir()) continue;
        if (!registry_->block(ns).canSurviveAt(*this, nc, ns)) popBlock(nc);
    }
    return true;
}

void World::popBlock(const glm::ivec3& cell) {
    BlockState s = getState(cell.x, cell.y, cell.z);
    if (s.isAir()) return;
    if (!setState(cell.x, cell.y, cell.z, BlockState())) return;
    registry_->block(s).onBroken(*this, cell, s);
    if (onBlockPopped) onBlockPopped(cell, s);
}

void World::update(VkContext& ctx, const glm::vec3& cameraPos) {
    const int N = Chunk::kSize;
    const int pcx = static_cast<int>(std::floor(cameraPos.x / N));
    const int pcz = static_cast<int>(std::floor(cameraPos.z / N));

    // Keep the offset ring sized to whatever the current distances need.
    rebuildRing(std::max(renderDistance + 1, std::min(lodDistance, kLodCap)));

    // RAM guard (checked ~2x/second): above 85% system memory load, stop growing the
    // world and unload harder. Prevents the page-thrash freeze -> failed-allocation crash
    // seen at render distance 64 (chunks alone reach many GB and the iGPU shares RAM).
    if (++memCheckCounter_ >= 30) {
        memCheckCounter_ = 0;
        bool pressured = systemMemoryLoadPercent() >= 85;
        if (pressured != memoryPressure_) {
            memoryPressure_ = pressured;
            std::fprintf(stderr, pressured
                             ? "[World] system RAM %d%% full: pausing generation, unloading harder\n"
                             : "[World] RAM pressure over: generation resumed\n",
                         systemMemoryLoadPercent());
        }
    }

    streamChunks(pcx, pcz);
    scheduleMeshing(pcx, pcz);
    collectAndUpload(ctx);
    unloadDistant(pcx, pcz);

    streamLod(pcx, pcz);
    collectAndUploadLod(ctx);
    unloadLod(pcx, pcz);

    processDeferredDestroys(ctx);
}

void World::streamChunks(int pcx, int pcz) {
    if (memoryPressure_) return; // RAM nearly full: stop growing the world until it eases
    const int genR = renderDistance + 1; // mesh needs generated neighbors
    // ringOffsets_ is nearest-first, so returning when the budget is full leaves the pool
    // holding the chunks closest to the player -- exactly what fast flight needs generated.
    for (const ChunkCoord& off : ringOffsets_) {
        // Sorted by euclidean distance: past genR*sqrt(2) nothing can be within Chebyshev
        // genR, so stop (the ring may be sized for a much larger LOD distance).
        if (off.x * off.x + off.z * off.z > 2 * genR * genR) break;
        if (cheb(off.x, off.z) > genR) continue;
        int cx = pcx + off.x, cz = pcz + off.z;
        for (int cy = 0; cy < kVerticalChunks; ++cy) {
            // Sky cubes (entirely above this column's terrain ceiling, asked from the
            // GENERATOR so future biomes/mountains/islands stay correct) are guaranteed
            // pure air: never create/generate/store them (~25% of all cubes). They read
            // as air, mesh as air neighbors, and are materialized on demand if the player
            // builds up there — unless the save already has an edited file for this cube.
            if (cy * Chunk::kSize >= columnCeiling(cx, cz) &&
                savedChunks_.count({cx, cy, cz}) == 0) {
                continue;
            }
            if (getChunk(cx, cy, cz)) continue;
            if (genInFlight_.load(std::memory_order_relaxed) >= genBudget_) return;

            auto chunk = std::make_shared<Chunk>(cx, cy, cz);
            chunk->state.store(CHUNK_GENERATING, std::memory_order_relaxed);
            chunks_[{cx, cy, cz}] = chunk;

            WorldGen* gen = gen_.get();
            WorldSave* save = save_;
            ChunkCoord coord{cx, cy, cz};
            genInFlight_.fetch_add(1, std::memory_order_relaxed);
            pool_.enqueue([this, chunk, gen, save, coord] {
                gen->generate(*chunk);
                // Player-edited chunks were written to the save; overlay them over the
                // fresh generation (per-file handles, safe from worker threads). The
                // chunk starts un-edited: its file already matches until the next edit.
                if (save) {
                    save->loadChunk(coord.x, coord.y, coord.z, chunk->rawBlocks().data(),
                                    chunk->rawBlocks().size());
                }
                chunk->state.store(CHUNK_GENERATED, std::memory_order_release);
                genInFlight_.fetch_sub(1, std::memory_order_relaxed);
            });
        }
    }
}

void World::scheduleMeshing(int pcx, int pcz) {
    const int meshR = renderDistance;
    for (const ChunkCoord& off : ringOffsets_) {
        if (off.x * off.x + off.z * off.z > 2 * meshR * meshR) break; // sorted: none closer left
        if (cheb(off.x, off.z) > meshR) continue;
        int cx = pcx + off.x, cz = pcz + off.z;
        for (int cy = 0; cy < kVerticalChunks; ++cy) {
            auto self = getChunk(cx, cy, cz);
            if (!self || self->meshInFlight.load(std::memory_order_acquire)) continue;

            int st = self->state.load(std::memory_order_acquire);
            bool wantInitial = (st == CHUNK_GENERATED);
            bool wantRemesh = (st == CHUNK_READY) && self->dirty.load(std::memory_order_acquire);
            if (!wantInitial && !wantRemesh) continue;
            if (meshInFlight_.load(std::memory_order_relaxed) >= meshBudget_) return; // nearest-first

            auto px = getChunk(cx + 1, cy, cz);
            auto nx = getChunk(cx - 1, cy, cz);
            auto pz = getChunk(cx, cy, cz + 1);
            auto nz = getChunk(cx, cy, cz - 1);
            auto py = getChunk(cx, cy + 1, cz);
            auto ny = getChunk(cx, cy - 1, cz);

            // A neighbor is settled when generated, or when it is a skipped sky cube that
            // will never exist (guaranteed air, no saved file) — the mesher already treats
            // a null neighbor as air.
            auto settled = [&](const std::shared_ptr<Chunk>& n, int ncx, int ncy, int ncz) {
                if (atLeastGenerated(n)) return true;
                return !n && ncy * Chunk::kSize >= columnCeiling(ncx, ncz) &&
                       savedChunks_.count({ncx, ncy, ncz}) == 0;
            };
            bool ready = settled(px, cx + 1, cy, cz) && settled(nx, cx - 1, cy, cz) &&
                         settled(pz, cx, cy, cz + 1) && settled(nz, cx, cy, cz - 1) &&
                         (cy + 1 >= kVerticalChunks || settled(py, cx, cy + 1, cz)) &&
                         (cy - 1 < 0 || settled(ny, cx, cy - 1, cz));
            if (!ready) continue;

            self->dirty.store(false, std::memory_order_release);
            self->meshInFlight.store(true, std::memory_order_release);
            meshInFlight_.fetch_add(1, std::memory_order_relaxed);
            ChunkCoord coord{cx, cy, cz};
            BlockRegistry* reg = registry_;
            pool_.enqueue([this, coord, self, px, nx, py, ny, pz, nz, reg] {
                ChunkMeshData data = buildCubeMesh(self.get(), px.get(), nx.get(), py.get(),
                                                   ny.get(), pz.get(), nz.get(), *reg);
                std::lock_guard<std::mutex> lock(completedMutex_);
                completed_.push_back({coord, std::move(data)});
                meshInFlight_.fetch_sub(1, std::memory_order_relaxed);
            });
        }
    }
}

void World::collectAndUpload(VkContext& ctx) {
    std::vector<CompletedMesh> batch;
    {
        std::lock_guard<std::mutex> lock(completedMutex_);
        size_t n = std::min<size_t>(kUploadBudget, completed_.size());
        batch.insert(batch.end(), std::make_move_iterator(completed_.begin()),
                     std::make_move_iterator(completed_.begin() + n));
        completed_.erase(completed_.begin(), completed_.begin() + n);
    }
    if (batch.empty()) return;

    struct Upload {
        ChunkCoord coord;
        AllocatedBuffer vb, ib, vstage, istage;
        uint32_t indexCount;
        glm::vec3 boundsMin, boundsMax;
    };
    std::vector<Upload> uploads;

    for (CompletedMesh& cm : batch) {
        auto chunk = getChunk(cm.coord.x, cm.coord.y, cm.coord.z);
        // Discard if the chunk was unloaded or replaced (we only finish meshes we queued).
        if (!chunk || !chunk->meshInFlight.load(std::memory_order_acquire)) continue;

        if (cm.data.empty()) {
            if (chunk->mesh.indexCount > 0) {
                pendingDestroy_.push_back({std::move(chunk->mesh), kDestroyDelayFrames});
            }
            chunk->mesh = ChunkMesh{};
            chunk->state.store(CHUNK_READY, std::memory_order_release);
            chunk->meshInFlight.store(false, std::memory_order_release);
            continue;
        }

        VkDeviceSize vsize = cm.data.vertices.size() * sizeof(ChunkVertex);
        VkDeviceSize isize = cm.data.indices.size() * sizeof(uint32_t);

        Upload u{};
        u.coord = cm.coord;
        u.indexCount = static_cast<uint32_t>(cm.data.indices.size());
        u.boundsMin = cm.data.boundsMin;
        u.boundsMax = cm.data.boundsMax;
        u.vb = createDeviceBuffer(ctx.allocator, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        u.ib = createDeviceBuffer(ctx.allocator, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        u.vstage = createHostBuffer(ctx.allocator, vsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        u.istage = createHostBuffer(ctx.allocator, isize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(u.vstage.mapped, cm.data.vertices.data(), vsize);
        std::memcpy(u.istage.mapped, cm.data.indices.data(), isize);
        uploads.push_back(u);
    }

    if (!uploads.empty()) {
        ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            for (const Upload& u : uploads) {
                VkBufferCopy vc{0, 0, u.vb.size};
                vkCmdCopyBuffer(cmd, u.vstage.buffer, u.vb.buffer, 1, &vc);
                VkBufferCopy ic{0, 0, u.ib.size};
                vkCmdCopyBuffer(cmd, u.istage.buffer, u.ib.buffer, 1, &ic);
            }
        });

        for (Upload& u : uploads) {
            destroyBuffer(ctx.allocator, u.vstage);
            destroyBuffer(ctx.allocator, u.istage);
            auto chunk = getChunk(u.coord.x, u.coord.y, u.coord.z);
            if (chunk && chunk->meshInFlight.load(std::memory_order_acquire)) {
                // Replacing an existing mesh (re-mesh): defer-free the old GPU buffers.
                if (chunk->mesh.indexCount > 0) {
                    pendingDestroy_.push_back({std::move(chunk->mesh), kDestroyDelayFrames});
                }
                chunk->mesh.vertexBuffer = u.vb;
                chunk->mesh.indexBuffer = u.ib;
                chunk->mesh.indexCount = u.indexCount;
                chunk->mesh.boundsMin = u.boundsMin;
                chunk->mesh.boundsMax = u.boundsMax;
                chunk->state.store(CHUNK_READY, std::memory_order_release);
                chunk->meshInFlight.store(false, std::memory_order_release);
            } else {
                destroyBuffer(ctx.allocator, u.vb);
                destroyBuffer(ctx.allocator, u.ib);
            }
        }
    }
}

void World::unloadDistant(int pcx, int pcz) {
    const int unloadR = renderDistance + 4;
    // Unload whole COLUMNS (all vertical cubes together), farthest first, capped per frame.
    // Erasing individual cubes in hash-map order left half-loaded columns (surface gone,
    // underground still drawn) — the renderer's per-column LOD gate then hid the LOD under
    // a hole ("neither HD nor LOD"). Column-atomic unload makes partial columns impossible,
    // farthest-first makes the HD ring recede coherently, and the caps keep a big render-
    // distance drop spread over frames instead of one giant hitch.
    // Under RAM pressure, unload much harder to free memory fast.
    const int kColumnBudget = memoryPressure_ ? 256 : 64; // columns/frame (x kVerticalChunks)
    const int kScanCap = memoryPressure_ ? 1024 : 256;    // candidates gathered per frame
    struct FarColumn {
        int d, cx, cz;
    };
    std::vector<FarColumn> cols;
    std::unordered_map<uint64_t, bool> seen;
    for (const auto& [coord, chunk] : chunks_) {
        int d = cheb(coord.x - pcx, coord.z - pcz);
        if (d <= unloadR) continue;
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32) |
                       static_cast<uint32_t>(coord.z);
        if (!seen.emplace(key, true).second) continue;
        cols.push_back({d, coord.x, coord.z});
        if (static_cast<int>(cols.size()) >= kScanCap) break;
    }
    std::sort(cols.begin(), cols.end(),
              [](const FarColumn& a, const FarColumn& b) { return a.d > b.d; });
    int n = std::min(static_cast<int>(cols.size()), kColumnBudget);
    for (int i = 0; i < n; ++i) {
        for (int cy = 0; cy < kVerticalChunks; ++cy) {
            auto it = chunks_.find({cols[i].cx, cy, cols[i].cz});
            if (it == chunks_.end()) continue;
            auto& chunk = it->second;
            // Edited chunks persist to the save before they vanish, so walking away and
            // coming back (or relaunching) keeps player changes.
            if (save_ && chunk->edited.load(std::memory_order_acquire) &&
                chunk->state.load(std::memory_order_acquire) >= CHUNK_GENERATED) {
                save_->saveChunk(cols[i].cx, cy, cols[i].cz, chunk->rawBlocks().data(),
                                 chunk->rawBlocks().size());
                savedChunks_.insert({cols[i].cx, cy, cols[i].cz}); // sky cubes reload next visit
            }
            if (chunk->mesh.indexCount > 0) {
                pendingDestroy_.push_back({std::move(chunk->mesh), kDestroyDelayFrames});
            }
            chunks_.erase(it);
        }
    }
}

void World::streamLod(int pcx, int pcz) {
    if (lodTop_.empty() || lodDistance <= renderDistance) return; // LOD off (no colors / disabled)
    if (memoryPressure_) return; // RAM nearly full: no new LOD tiles either
    // HD priority lives in the POOL, not here: LOD jobs go to the low-priority queue, and
    // workers always drain every queued chunk gen/mesh job before touching a LOD job. So
    // queuing LOD work never slows the HD ring — it just waits its turn — and LOD can still
    // trickle in on leftover worker time even while flying (an earlier "don't queue while
    // chunks are busy" gate here meant LOD only ever generated when standing still).
    const int inner = renderDistance + 1;                          // real chunks cover <= renderDistance
    const int outer = std::min(lodDistance, kLodCap);
    for (const ChunkCoord& off : ringOffsets_) {
        if (off.x * off.x + off.z * off.z > 2 * outer * outer) break; // sorted: all farther
        int d = cheb(off.x, off.z);
        if (d < inner || d > outer) continue;
        ChunkCoord coord{pcx + off.x, 0, pcz + off.z};

        int step = 0;
        bool chunkSourced = false;
        lodLevelFor(d, renderDistance, lodDistance, step, chunkSourced);

        if (lodInFlight_.count(coord)) continue; // already generating (at some step)
        auto it = lodTiles_.find(coord);
        if (it != lodTiles_.end() && it->second.step == step) continue; // already at this level
        // In-progress LOD jobs can't be preempted, so while chunk gen/mesh work exists keep
        // at most a couple of workers on LOD — the nearest full-detail chunks get (almost)
        // the whole pool, so flying never outruns the ground. With the chunk pipeline idle,
        // LOD gets full parallelism to fill the horizon fast.
        int chunkWork = genInFlight_.load(std::memory_order_relaxed) +
                        meshInFlight_.load(std::memory_order_relaxed);
        int cap = chunkWork > 0 ? 2 : lodBudget_;
        if (static_cast<int>(lodInFlight_.size()) >= cap) return;
        lodInFlight_[coord] = step;

        WorldGen* gen = gen_.get();
        WorldSave* save = save_;
        const std::vector<glm::vec3>* top = &lodTop_;
        const std::vector<glm::vec3>* side = &lodSide_;
        pool_.enqueue(
            [this, coord, gen, save, top, side, step, chunkSourced] {
                LodMeshData data =
                    buildLodTile(*gen, save, *top, *side, coord.x, coord.z, step, chunkSourced);
                std::lock_guard<std::mutex> lock(lodMutex_);
                completedLod_.push_back({coord, std::move(data), step});
            },
            /*lowPriority=*/true);
    }
}

void World::collectAndUploadLod(VkContext& ctx) {
    std::vector<CompletedLod> batch;
    {
        std::lock_guard<std::mutex> lock(lodMutex_);
        size_t n = std::min<size_t>(kLodUploadBudget, completedLod_.size());
        batch.insert(batch.end(), std::make_move_iterator(completedLod_.begin()),
                     std::make_move_iterator(completedLod_.begin() + n));
        completedLod_.erase(completedLod_.begin(), completedLod_.begin() + n);
    }
    if (batch.empty()) return;

    struct Upload {
        ChunkCoord coord;
        AllocatedBuffer vb, ib, vstage, istage;
        uint32_t indexCount;
        float minY, maxY;
        int step;
    };
    std::vector<Upload> uploads;

    // Defer-free a tile's existing mesh (a rebuild at a new detail level replaces it).
    auto retireOld = [&](const ChunkCoord& coord) {
        auto it = lodTiles_.find(coord);
        if (it != lodTiles_.end() && it->second.mesh.indexCount > 0) {
            pendingLodDestroy_.push_back({std::move(it->second.mesh), kDestroyDelayFrames});
        }
    };

    for (CompletedLod& cm : batch) {
        lodInFlight_.erase(cm.coord); // no longer queued regardless of outcome

        if (cm.data.empty()) {
            // Record an empty tile so it isn't re-enqueued every frame.
            retireOld(cm.coord);
            LodTile t;
            t.cx = cm.coord.x;
            t.cz = cm.coord.z;
            t.step = cm.step;
            lodTiles_[cm.coord] = std::move(t);
            continue;
        }

        VkDeviceSize vsize = cm.data.vertices.size() * sizeof(LodVertex);
        VkDeviceSize isize = cm.data.indices.size() * sizeof(uint32_t);
        Upload u{};
        u.coord = cm.coord;
        u.indexCount = static_cast<uint32_t>(cm.data.indices.size());
        u.minY = cm.data.minY;
        u.maxY = cm.data.maxY;
        u.step = cm.step;
        u.vb = createDeviceBuffer(ctx.allocator, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        u.ib = createDeviceBuffer(ctx.allocator, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        u.vstage = createHostBuffer(ctx.allocator, vsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        u.istage = createHostBuffer(ctx.allocator, isize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(u.vstage.mapped, cm.data.vertices.data(), vsize);
        std::memcpy(u.istage.mapped, cm.data.indices.data(), isize);
        uploads.push_back(u);
    }

    if (uploads.empty()) return;

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        for (const Upload& u : uploads) {
            VkBufferCopy vc{0, 0, u.vb.size};
            vkCmdCopyBuffer(cmd, u.vstage.buffer, u.vb.buffer, 1, &vc);
            VkBufferCopy ic{0, 0, u.ib.size};
            vkCmdCopyBuffer(cmd, u.istage.buffer, u.ib.buffer, 1, &ic);
        }
    });

    for (Upload& u : uploads) {
        destroyBuffer(ctx.allocator, u.vstage);
        destroyBuffer(ctx.allocator, u.istage);
        retireOld(u.coord); // free the previous detail level's mesh if this is a rebuild
        LodTile t;
        t.cx = u.coord.x;
        t.cz = u.coord.z;
        t.step = u.step;
        t.mesh.vertexBuffer = u.vb;
        t.mesh.indexBuffer = u.ib;
        t.mesh.indexCount = u.indexCount;
        t.mesh.minY = u.minY;
        t.mesh.maxY = u.maxY;
        lodTiles_[u.coord] = std::move(t);
    }
}

void World::unloadLod(int pcx, int pcz) {
    // Only unload tiles that are truly OUT OF RANGE (past the LOD distance). Tiles the real
    // chunks now cover are KEPT, not destroyed -- the renderer hides them under the chunks,
    // so when the render distance drops (chunks unload) the LOD is instantly there again
    // instead of leaving a void while it regenerates. (streamLod still won't CREATE new
    // tiles inside the chunk ring, so this only retains ones that drifted in.)
    const int outer = std::min(lodDistance, kLodCap) + 2;
    constexpr int kUnloadBudget = 128; // cap/frame so an RD/LOD change doesn't hitch
    std::vector<ChunkCoord> toErase;
    for (const auto& [coord, tile] : lodTiles_) {
        int d = cheb(coord.x - pcx, coord.z - pcz);
        if (d > outer) {
            toErase.push_back(coord);
            if (static_cast<int>(toErase.size()) >= kUnloadBudget) break;
        }
    }
    for (const ChunkCoord& coord : toErase) {
        auto it = lodTiles_.find(coord);
        if (it == lodTiles_.end()) continue;
        if (it->second.mesh.indexCount > 0) {
            pendingLodDestroy_.push_back({std::move(it->second.mesh), kDestroyDelayFrames});
        }
        lodTiles_.erase(it);
    }
}

void World::flushEdited() {
    if (!save_) return;
    for (auto& [coord, chunk] : chunks_) {
        if (chunk->edited.load(std::memory_order_acquire) &&
            chunk->state.load(std::memory_order_acquire) >= CHUNK_GENERATED) {
            save_->saveChunk(coord.x, coord.y, coord.z, chunk->rawBlocks().data(),
                             chunk->rawBlocks().size());
            savedChunks_.insert(coord);
            chunk->edited.store(false, std::memory_order_release);
        }
    }
}

void World::processDeferredDestroys(VkContext& ctx) {
    for (auto it = pendingDestroy_.begin(); it != pendingDestroy_.end();) {
        if (--it->framesLeft <= 0) {
            it->mesh.destroy(ctx.allocator);
            it = pendingDestroy_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pendingLodDestroy_.begin(); it != pendingLodDestroy_.end();) {
        if (--it->framesLeft <= 0) {
            it->mesh.destroy(ctx.allocator);
            it = pendingLodDestroy_.erase(it);
        } else {
            ++it;
        }
    }
}

void World::destroy(VkContext& ctx) {
    pool_.stop(); // join workers; no more tasks touch chunks after this
    {
        std::lock_guard<std::mutex> lock(completedMutex_);
        completed_.clear();
    }
    for (auto& [coord, chunk] : chunks_) {
        chunk->mesh.destroy(ctx.allocator);
    }
    chunks_.clear();
    for (auto& pd : pendingDestroy_) pd.mesh.destroy(ctx.allocator);
    pendingDestroy_.clear();

    {
        std::lock_guard<std::mutex> lock(lodMutex_);
        completedLod_.clear();
    }
    for (auto& [coord, tile] : lodTiles_) tile.mesh.destroy(ctx.allocator);
    lodTiles_.clear();
    lodInFlight_.clear();
    for (auto& pd : pendingLodDestroy_) pd.mesh.destroy(ctx.allocator);
    pendingLodDestroy_.clear();
}

} // namespace mc
