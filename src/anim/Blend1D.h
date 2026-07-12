#pragma once

#include <vector>

namespace mc {

class AnimClip;
class Rig;
struct Pose;
struct MolangContext;

// A 1D blend space: clips placed at points along a parameter (e.g. speed → idle/walk/run).
// Sampling at x picks the two bracketing clips and nlerp-blends them, both sampled at a
// SHARED normalized phase so stride-locked clips never foot-slide. The clips form one
// implicit sync group; the leader (higher weight) sets the phase, followers match — the
// A3 blend node + sync group (docs/animation.md §7/§8).
//
// FUTURE(parkour): Blend2D (height × distance) is the same idea over two params.
// FUTURE(combat): additive layering composes on top of a blend node like this one.
class Blend1D {
public:
    void clear() { points_.clear(); }
    void add(float value, const AnimClip* clip); // inserts sorted by value
    bool empty() const { return points_.empty(); }

    // Blended clip duration at x (weighted lerp of the two bracketing clips). Advance the
    // shared phase by dt / duration(x) each frame so stride frequency scales with the param.
    float duration(float x) const;

    // Samples at parameter x and shared normalized phase [0,1) into `pose` (bones no clip
    // touches are left identity). `ctx` evaluates any Molang in the clips.
    void sample(float x, float phase, const Rig& rig, Pose& pose, MolangContext& ctx) const;

    // The higher-weight of the two bracketing clips at x — the sync-group "leader" whose
    // events fire (so a blended step emits one foot_down, not two). Null if empty.
    const AnimClip* dominantClip(float x) const;

private:
    struct Point {
        float            value = 0.0f;
        const AnimClip*  clip = nullptr;
    };
    std::vector<Point> points_; // sorted ascending by value

    // Bracketing points i0<=i1 and the fraction t in [0,1] between them for parameter x.
    void bracket(float x, int& i0, int& i1, float& t) const;
};

} // namespace mc
