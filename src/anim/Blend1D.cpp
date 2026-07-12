#include "anim/Blend1D.h"

#include "anim/AnimClip.h"
#include "anim/Pose.h"
#include "anim/Rig.h"

#include <algorithm>

namespace mc {

void Blend1D::add(float value, const AnimClip* clip) {
    Point p{value, clip};
    auto at = std::lower_bound(points_.begin(), points_.end(), value,
                               [](const Point& a, float v) { return a.value < v; });
    points_.insert(at, p);
}

void Blend1D::bracket(float x, int& i0, int& i1, float& t) const {
    const int n = static_cast<int>(points_.size());
    if (x <= points_.front().value) { i0 = i1 = 0; t = 0.0f; return; }
    if (x >= points_.back().value) { i0 = i1 = n - 1; t = 0.0f; return; }
    for (int i = 1; i < n; ++i) {
        if (x <= points_[i].value) {
            i0 = i - 1;
            i1 = i;
            float span = points_[i].value - points_[i - 1].value;
            t = span > 0.0f ? (x - points_[i - 1].value) / span : 0.0f;
            return;
        }
    }
    i0 = i1 = n - 1;
    t = 0.0f;
}

float Blend1D::duration(float x) const {
    if (points_.empty()) return 0.0f;
    int i0, i1;
    float t;
    bracket(x, i0, i1, t);
    float d0 = points_[i0].clip ? points_[i0].clip->duration() : 0.0f;
    float d1 = points_[i1].clip ? points_[i1].clip->duration() : 0.0f;
    return d0 + (d1 - d0) * t;
}

void Blend1D::sample(float x, float phase, const Rig& rig, Pose& pose, MolangContext& ctx) const {
    if (points_.empty()) { pose.reset(rig); return; }
    int i0, i1;
    float t;
    bracket(x, i0, i1, t);

    // Each clip samples at the SAME normalized phase, scaled to its own length → the stride
    // stays locked across the blend (no foot slide).
    auto sampleClip = [&](const AnimClip* clip, Pose& out) {
        out.reset(rig);
        if (!clip) return;
        float ct = clip->looping() && clip->duration() > 0.0f ? phase * clip->duration() : phase;
        clip->sample(ct, rig, out, ctx);
    };

    if (i0 == i1 || t <= 0.0f) { sampleClip(points_[i0].clip, pose); return; }
    if (t >= 1.0f) { sampleClip(points_[i1].clip, pose); return; }

    Pose a(rig), b(rig);
    sampleClip(points_[i0].clip, a);
    sampleClip(points_[i1].clip, b);
    pose.blend(a, b, t);
}

const AnimClip* Blend1D::dominantClip(float x) const {
    if (points_.empty()) return nullptr;
    int i0, i1;
    float t;
    bracket(x, i0, i1, t);
    return (t < 0.5f ? points_[i0].clip : points_[i1].clip);
}

} // namespace mc
