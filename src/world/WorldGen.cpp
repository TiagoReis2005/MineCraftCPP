#include "world/WorldGen.h"

#include "world/BlockRegistry.h"
#include "world/Chunk.h"

#include <algorithm>

namespace mc {

namespace {
constexpr int kWorldHeight = 128; // 4 cubes tall
constexpr int kBaseHeight = 64;
constexpr int kAmplitude = 24;
constexpr int kSandLevel = 52;
// Added on top of the sampled max height in surfaceCeiling: covers peaks that fall
// between the 4-block sampling grid (the height field is smooth at that scale).
constexpr int kSkyMargin = 8;
}

WorldGen::WorldGen(uint32_t seed, const BlockRegistry& reg)
    : noise_(seed), grass_(reg.grass), dirt_(reg.dirt), stone_(reg.stone), sand_(reg.sand) {}

int WorldGen::heightAt(int worldX, int worldZ) const {
    // Low-frequency base for broad, gently rolling hills; a small high-frequency layer adds
    // subtle detail without making slopes choppy.
    float base = noise_.fbm(worldX * 0.006f, worldZ * 0.006f, 4);
    float detail = noise_.fbm(worldX * 0.03f, worldZ * 0.03f, 2);
    float n = base + 0.15f * detail;
    int h = kBaseHeight + static_cast<int>(n * kAmplitude);
    return std::clamp(h, 4, kWorldHeight - 2);
}

int WorldGen::surfaceCeiling(int cx, int cz) const {
    const int S = Chunk::kSize;
    int maxH = 0;
    for (int z = 0; z <= S; z += 4) {
        for (int x = 0; x <= S; x += 4) {
            maxH = std::max(maxH, heightAt(cx * S + std::min(x, S - 1),
                                           cz * S + std::min(z, S - 1)));
        }
    }
    return maxH + kSkyMargin;
}

BlockId WorldGen::surfaceBlock(int height) const {
    return height <= kSandLevel ? sand_ : grass_;
}

void WorldGen::generate(Chunk& chunk) const {
    const int N = Chunk::kSize;
    const int baseX = chunk.cx() * N;
    const int baseY = chunk.cy() * N;
    const int baseZ = chunk.cz() * N;

    for (int z = 0; z < N; ++z) {
        for (int x = 0; x < N; ++x) {
            int height = heightAt(baseX + x, baseZ + z);
            bool beach = height <= kSandLevel;

            // Skip columns entirely below or above this cube's vertical span.
            for (int y = 0; y < N; ++y) {
                int worldY = baseY + y;
                if (worldY >= height) break; // air above the surface
                BlockId block;
                if (worldY == height - 1) {
                    block = beach ? sand_ : grass_;
                } else if (worldY >= height - 4) {
                    block = beach ? sand_ : dirt_;
                } else {
                    block = stone_;
                }
                chunk.set(x, y, z, block);
            }
        }
    }
}

} // namespace mc
