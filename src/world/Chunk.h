#pragma once

#include "world/Block.h"
#include "world/Mesher.h"

#include <atomic>
#include <vector>

namespace mc {

// Lifecycle state: main(Empty->Generating), worker(Generating->Generated), then meshing is
// tracked by the meshInFlight flag (so a Ready chunk being re-meshed after an edit keeps
// drawing its old mesh and never flickers). Block reads only happen once >= Generated.
enum ChunkState {
    CHUNK_EMPTY = 0,
    CHUNK_GENERATING,
    CHUNK_GENERATED,
    CHUNK_READY
};

// A 32x32x32 cube of voxels; each cell stores a full BlockState (id + data bits).
class Chunk {
public:
    static constexpr int kSize = 32;

    Chunk(int cx, int cy, int cz)
        : cx_(cx), cy_(cy), cz_(cz), blocks_(kSize * kSize * kSize, 0u) {}

    int cx() const { return cx_; }
    int cy() const { return cy_; }
    int cz() const { return cz_; }

    BlockState get(int x, int y, int z) const { return BlockState::fromRaw(blocks_[index(x, y, z)]); }
    void set(int x, int y, int z, BlockState s) { blocks_[index(x, y, z)] = s.raw; }

    static int index(int x, int y, int z) {
        return x + kSize * (z + kSize * y);
    }

    std::atomic<int> state{CHUNK_EMPTY};
    std::atomic<bool> meshInFlight{false}; // a (re)mesh job is queued/running
    std::atomic<bool> dirty{false};        // blocks changed since last mesh; needs re-mesh
    std::atomic<bool> edited{false};       // player-modified since generation/load: save me

    // GPU occlusion-query result (render thread only): was any pixel of this chunk visible
    // last time it was tested? Defaults visible so new chunks are always drawn once.
    bool occVisible = true;

    // Raw block storage for the world save (RLE round-trip).
    std::vector<uint32_t>& rawBlocks() { return blocks_; }
    const std::vector<uint32_t>& rawBlocks() const { return blocks_; }

    ChunkMesh mesh; // valid once state == CHUNK_READY (may be empty for all-air cubes)

private:
    int cx_;
    int cy_;
    int cz_;
    std::vector<uint32_t> blocks_; // raw BlockStates
};

} // namespace mc
