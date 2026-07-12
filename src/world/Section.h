#pragma once

#include "world/Block.h"

#include <array>

namespace mc {

// A 16x16x16 cube of voxels. Storage is a flat array; default-initialized to Air.
class Section {
public:
    static constexpr int kSize = 16;

    BlockId get(int x, int y, int z) const { return blocks_[index(x, y, z)]; }
    void set(int x, int y, int z, BlockId b) { blocks_[index(x, y, z)] = b; }

    static bool inBounds(int x, int y, int z) {
        return x >= 0 && x < kSize && y >= 0 && y < kSize && z >= 0 && z < kSize;
    }

    static int index(int x, int y, int z) {
        return x + kSize * (z + kSize * y);
    }

private:
    std::array<BlockId, kSize * kSize * kSize> blocks_{}; // all Air by default
};

} // namespace mc
