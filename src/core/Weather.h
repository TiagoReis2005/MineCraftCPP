#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

namespace mc {

// Clear <-> rain state machine with random durations and a soft fade, like Minecraft's
// weather cycle (thunder comes later). intensity() (0..1) drives the rain curtain's
// alpha, the sky darkening and the sun/moon dimming.
//
// The RNG is seeded from the WORLD SEED, so two worlds with the same seed get the same
// weather sequence, and the current state is saved/restored so it survives reload -- it is
// advanced on the fixed game tick (not real time), so it also pauses with the game.
class Weather {
public:
    Weather() { seed(0); }

    // Reseed from the world seed and roll the first shower's countdown deterministically.
    void seed(uint32_t s) {
        rng_.seed(s);
        raining_ = false;
        intensity_ = 0.0f;
        timer_ = range(120.0f, 300.0f); // first shower fairly soon after a fresh start
    }

    void advance(float dt) {
        timer_ -= dt;
        if (timer_ <= 0.0f) toggle();
        float target = raining_ ? 1.0f : 0.0f;
        intensity_ += (target - intensity_) * std::min(1.0f, dt / 4.0f); // ~4s fade
    }

    // Flips the state immediately (also the R debug key); durations stay random.
    void toggle() {
        raining_ = !raining_;
        timer_ = raining_ ? range(90.0f, 240.0f) : range(300.0f, 900.0f);
    }

    bool raining() const { return raining_; }
    float intensity() const { return intensity_; }

    // Persisted state (world save): the current shower's remaining time, whether it is
    // raining, and the fade level, so reload resumes exactly where it left off.
    float timer() const { return timer_; }
    void setState(bool raining, float timer, float intensity) {
        raining_ = raining;
        timer_ = timer;
        intensity_ = intensity;
    }

private:
    float range(float a, float b) {
        return std::uniform_real_distribution<float>(a, b)(rng_);
    }

    std::mt19937 rng_;
    bool raining_ = false;
    float timer_ = 0.0f;
    float intensity_ = 0.0f;
};

} // namespace mc
