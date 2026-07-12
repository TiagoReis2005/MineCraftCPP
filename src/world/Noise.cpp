#include "world/Noise.h"

#include <cmath>
#include <numeric>
#include <random>

namespace mc {

Noise::Noise(uint32_t seed) {
    int p[256];
    std::iota(p, p + 256, 0);
    std::mt19937 rng(seed);
    for (int i = 255; i > 0; --i) {
        std::uniform_int_distribution<int> dist(0, i);
        std::swap(p[i], p[dist(rng)]);
    }
    for (int i = 0; i < 512; ++i) perm_[i] = p[i & 255];
}

float Noise::fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float Noise::lerp(float a, float b, float t) {
    return a + t * (b - a);
}

float Noise::grad(int hash, float x, float y) {
    switch (hash & 3) {
        case 0:  return x + y;
        case 1:  return -x + y;
        case 2:  return x - y;
        default: return -x - y;
    }
}

float Noise::perlin(float x, float y) const {
    int xi = static_cast<int>(std::floor(x)) & 255;
    int yi = static_cast<int>(std::floor(y)) & 255;
    float xf = x - std::floor(x);
    float yf = y - std::floor(y);

    float u = fade(xf);
    float v = fade(yf);

    int aa = perm_[perm_[xi] + yi];
    int ab = perm_[perm_[xi] + yi + 1];
    int ba = perm_[perm_[xi + 1] + yi];
    int bb = perm_[perm_[xi + 1] + yi + 1];

    float x1 = lerp(grad(aa, xf, yf), grad(ba, xf - 1.0f, yf), u);
    float x2 = lerp(grad(ab, xf, yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f), u);
    return lerp(x1, x2, v);
}

float Noise::fbm(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(x * frequency, y * frequency);
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace mc
