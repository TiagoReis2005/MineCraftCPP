#pragma once

#include "world/Block.h"
#include "world/Noise.h"

#include <cstdint>

namespace mc {

class Chunk;
class BlockRegistry;

// Fills chunks with terrain from a height field. Pure CPU work (thread-safe).
class WorldGen {
public:
    WorldGen(uint32_t seed, const BlockRegistry& reg);

    // Conservative ceiling of the terrain surface over one chunk footprint: a coarse
    // heightAt sampling plus a safety margin, guaranteed >= every real height in the
    // footprint. Cubes whose base y is at/above it are pure air ("sky cubes") and the
    // world skips creating them. NOT a global constant on purpose: when biomes add tall
    // mountains or floating islands, THIS query is the one place that must know about
    // them (answer from the actual noise for that column, or simply "no ceiling" for
    // cubes an island layer might touch).
    int surfaceCeiling(int cx, int cz) const;

    void generate(Chunk& chunk) const;

    // World-space surface height at a column (top solid block y).
    int heightAt(int worldX, int worldZ) const;

    // Top block id for a column of the given surface height (grass, or sand on beaches).
    // Used by the far-LOD tiles to tint their fake terrain like the real surface.
    BlockId surfaceBlock(int height) const;

private:
    Noise   noise_;
    BlockId grass_, dirt_, stone_, sand_;
};

} // namespace mc
