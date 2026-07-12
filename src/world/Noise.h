#pragma once

#include <cstdint>

namespace mc {

// Classic Perlin noise with fractal Brownian motion, seeded by a permutation table.
class Noise {
public:
    explicit Noise(uint32_t seed = 1337u);

    // 2D Perlin noise in roughly [-1, 1].
    float perlin(float x, float y) const;

    // Fractal sum of `octaves` Perlin layers; result roughly in [-1, 1].
    float fbm(float x, float y, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const;

private:
    int perm_[512];
    static float fade(float t);
    static float lerp(float a, float b, float t);
    static float grad(int hash, float x, float y);
};

} // namespace mc
