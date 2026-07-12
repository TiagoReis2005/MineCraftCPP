#pragma once

#include "anim/Rig.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace mc {

// A local pose over a Rig: one local transform per bone, composed into world matrices.
//
// Each bone's local transform is applied about the bone's pivot:
//     local_i = T(pos_i) * T(pivot_i) * R(rot_i) * T(-pivot_i)
//     world_i = parent < 0 ? local_i : world_parent * local_i
// Defaults (pos = 0, rot = identity) give the bind pose, so world_i = identity for all
// bones and meshes (authored in absolute model space) render exactly where modelled.
//
// Rotations are QUATERNIONS (P5, docs/animation.md): blending nlerps them shortest-arc so
// idle/walk/run blends don't shear the way matrix-lerped rotations would.
struct Pose {
    std::vector<glm::quat> rot; // rotation about the bone pivot (default identity)
    std::vector<glm::vec3> pos; // translation in our model space, pixels (default 0)

    Pose() = default;
    explicit Pose(const Rig& rig) { reset(rig); }

    void reset(const Rig& rig) {
        rot.assign(rig.boneCount(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        pos.assign(rig.boneCount(), glm::vec3(0.0f));
    }

    // this = lerp(a, b, t): position linear, rotation nlerp along the shortest arc. a and b
    // must be posed over the same rig (same bone count). Weight t in [0,1].
    void blend(const Pose& a, const Pose& b, float t);

    // Fills out[boneCount] with world transforms. Relies on the rig being topologically
    // sorted (parent index < child index), which Rig::loadFromGeometry guarantees.
    void compose(const Rig& rig, glm::mat4* out) const;

    // World transform of a locator (its owning bone's world times the local offset).
    // Identity if the locator index is out of range. FUTURE(mounts)/FUTURE(decals):
    // held-item / saddle / footprint attach points read through here.
    glm::mat4 socketWorld(const Rig& rig, int locatorIndex, const glm::mat4* world) const;
};

} // namespace mc
