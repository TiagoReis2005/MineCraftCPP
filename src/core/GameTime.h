#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>

namespace mc {

// In-game clock: a full day lasts kDayLengthSeconds (20 real minutes, like MC).
// Drives the sky color now; the sun/moon pass and weather hook into it later.
class GameTime {
public:
    static constexpr float kDayLengthSeconds = 1200.0f;

    void advance(float dt) { seconds_ += static_cast<double>(dt); }

    // 0 = dawn (06:00), 0.25 = noon, 0.5 = dusk (18:00), 0.75 = midnight.
    float dayFraction() const {
        return static_cast<float>(std::fmod(seconds_ / kDayLengthSeconds, 1.0));
    }
    int dayCount() const { return static_cast<int>(seconds_ / kDayLengthSeconds) + 1; }
    bool isNight() const { return dayFraction() > 0.5f; }

    // Sun elevation factor: 1 at noon, 0 at the horizon, negative at night.
    float sunHeight() const {
        return std::sin(dayFraction() * 2.0f * 3.14159265f);
    }
    // Sun angle for a future sun/moon billboard pass (radians around the world X axis).
    float sunAngle() const { return dayFraction() * 2.0f * 3.14159265f; }

    // Sky clear-color: day blue -> warm dusk -> near-black night.
    glm::vec3 skyColor() const {
        const glm::vec3 day(0.47f, 0.65f, 1.0f);
        const glm::vec3 dusk(0.98f, 0.55f, 0.32f);
        const glm::vec3 night(0.015f, 0.02f, 0.06f);
        float h = sunHeight();
        if (h > 0.25f) return day;
        if (h > 0.0f) { // blend day -> dusk near the horizon
            float t = h / 0.25f;
            return glm::mix(dusk, day, t);
        }
        if (h > -0.25f) { // dusk -> night
            float t = -h / 0.25f;
            return glm::mix(dusk, night, t);
        }
        return night;
    }

    // "HH:MM" of the in-game clock (dawn = 06:00).
    void clockText(char* out, size_t n) const {
        float f = dayFraction();
        int minutes = static_cast<int>(std::fmod(f * 24.0f * 60.0f + 6.0f * 60.0f, 24.0f * 60.0f));
        std::snprintf(out, n, "%02d:%02d", minutes / 60, minutes % 60);
    }

    // Raw clock for the world save (setSeconds restores a loaded world's time).
    double seconds() const { return seconds_; }
    void setSeconds(double s) { seconds_ = s; }
    static double freshWorldSeconds() { return kDayLengthSeconds * 0.1; }

    // Spawn mid-morning so worlds don't start at pitch-black dawn edge cases.
    GameTime() : seconds_(freshWorldSeconds()) {}

private:
    double seconds_ = 0.0;
};

} // namespace mc
