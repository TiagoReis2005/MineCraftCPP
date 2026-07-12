#pragma once

#include <glm/glm.hpp>

namespace mc {

class World;

struct RaycastHit {
    bool       hit = false;
    glm::ivec3 block{0};  // the cell whose shape was hit
    glm::ivec3 normal{0}; // hit surface's outward normal (zero when starting inside)
    glm::vec3  point{0};  // world-space hit point on the shape's surface
};

// Steps a ray through the voxel grid (Amanatides & Woo) and, per non-air cell, tests the
// block's actual selection boxes — so slabs/fences/doors hit on their real surfaces and
// the ray passes through the empty part of partial blocks. Used for targeting.
RaycastHit raycastVoxel(const World& world, const glm::vec3& origin,
                        const glm::vec3& dir, float maxDistance);

} // namespace mc
