#pragma once

#include "anim/Molang.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace mc {

struct JsonValue; // anim/Json.h

// One channel component set: per-axis Molang expressions, optionally keyframed. Shared by
// the legacy PlayerAnimator and by AnimClip (docs/animation.md §5).
struct AnimVec3 {
    MolangExpr x, y, z;
};
struct AnimKey {
    float time = 0.0f;
    AnimVec3 value;
};
struct AnimChannel {
    std::vector<AnimKey> keys; // 1 key = expression channel; >1 = keyframed (lerped)
    bool present() const { return !keys.empty(); }
};
struct AnimBone {
    AnimChannel rotation; // degrees, Bedrock axes
    AnimChannel position; // pixels, Bedrock axes
};

std::string toLowerName(std::string s);

// A channel is either [x,y,z] (single key) or {"time": [x,y,z], ...} (keyframes).
AnimChannel parseChannel(const JsonValue& v);

// Evaluate a channel at time t (linear between keys). ctx.thisValue is set per component
// from `accum` so Bedrock `this`-relative expressions resolve.
glm::vec3 sampleAnimChannel(const AnimChannel& ch, float t, MolangContext& ctx,
                            const glm::vec3& accum);

// Bedrock-space rotation (deg) as a pure pivot-origin rotation quaternion (no pivot/
// position baked in) — what Pose::rot expects; Pose::compose applies the rig pivot.
glm::quat boneRotation(const glm::vec3& rotDeg);

// Bedrock-space rotation (deg) + position (px) -> our-space local transform about `pivot`.
// (Used by the legacy first-person delta path.)
glm::mat4 boneTransform(const glm::vec3& rotDeg, const glm::vec3& posPx, const glm::vec3& pivot);

} // namespace mc
