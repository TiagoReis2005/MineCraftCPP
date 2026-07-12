#include "anim/Pose.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace mc {

void Pose::blend(const Pose& a, const Pose& b, float t) {
    const size_t n = std::min(a.rot.size(), b.rot.size());
    rot.resize(n);
    pos.resize(n);
    for (size_t i = 0; i < n; ++i) {
        pos[i] = glm::mix(a.pos[i], b.pos[i], t);
        // nlerp along the shortest arc: flip b if the quaternions are more than 90 apart.
        glm::quat qb = b.rot[i];
        if (glm::dot(a.rot[i], qb) < 0.0f) qb = -qb;
        rot[i] = glm::normalize(a.rot[i] * (1.0f - t) + qb * t);
    }
}

void Pose::compose(const Rig& rig, glm::mat4* out) const {
    const int n = rig.boneCount();
    for (int i = 0; i < n; ++i) {
        const RigBone& b = rig.bone(i);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), pos[i] + b.pivot) *
                          glm::mat4_cast(rot[i]) * glm::translate(glm::mat4(1.0f), -b.pivot);
        out[i] = b.parent < 0 ? local : out[b.parent] * local;
    }
}

glm::mat4 Pose::socketWorld(const Rig& rig, int locatorIndex, const glm::mat4* world) const {
    if (locatorIndex < 0 || locatorIndex >= static_cast<int>(rig.locators().size()))
        return glm::mat4(1.0f);
    const RigLocator& loc = rig.locators()[locatorIndex];
    if (loc.bone < 0) return glm::mat4(1.0f);
    return world[loc.bone] * glm::translate(glm::mat4(1.0f), loc.pos);
}

} // namespace mc
