#include "anim/FootIK.h"

#include <algorithm>
#include <cmath>

namespace mc {
namespace {

// Shortest-arc rotation taking unit `from` onto unit `to`.
glm::quat quatFromTo(const glm::vec3& from, const glm::vec3& to) {
    glm::vec3 u = glm::normalize(from);
    glm::vec3 v = glm::normalize(to);
    float d = glm::dot(u, v);
    if (d > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.99999f) { // opposite: rotate 180 about any perpendicular axis
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), u);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0, 0, 1), u);
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    glm::vec3 axis = glm::normalize(glm::cross(u, v));
    return glm::angleAxis(std::acos(std::clamp(d, -1.0f, 1.0f)), axis);
}

} // namespace

void solveTwoBoneIK(const glm::vec3& hip, const glm::vec3& target, float L1, float L2,
                    const glm::vec3& poleHint, glm::quat& upperWorld, glm::quat& lowerWorld) {
    const glm::vec3 kDown(0.0f, -1.0f, 0.0f);
    glm::vec3 toTarget = target - hip;
    float dist = glm::length(toTarget);
    glm::vec3 dir = dist > 1e-5f ? toTarget / dist : kDown;

    // Clamp reach so the triangle is solvable; foot lands as close as the leg allows.
    float d = std::clamp(dist, std::abs(L1 - L2) + 1e-3f, L1 + L2 - 1e-3f);
    glm::vec3 effTarget = hip + d * dir;

    // Knee bend plane: pole component perpendicular to the hip->target direction.
    glm::vec3 pole = poleHint - dir * glm::dot(poleHint, dir);
    if (glm::length(pole) < 1e-4f) {
        pole = glm::cross(dir, glm::vec3(1, 0, 0));
        if (glm::length(pole) < 1e-4f) pole = glm::cross(dir, glm::vec3(0, 0, 1));
    }
    pole = glm::normalize(pole);

    float cosHip = std::clamp((L1 * L1 + d * d - L2 * L2) / (2.0f * L1 * d), -1.0f, 1.0f);
    float hipAngle = std::acos(cosHip);
    glm::vec3 knee = hip + L1 * (std::cos(hipAngle) * dir + std::sin(hipAngle) * pole);

    upperWorld = quatFromTo(kDown, glm::normalize(knee - hip));
    lowerWorld = quatFromTo(kDown, glm::normalize(effTarget - knee));
}

} // namespace mc
