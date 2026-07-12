#include "world/Raycast.h"

#include "world/Block.h"
#include "world/BlockRegistry.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mc {
namespace {

// Ray vs AABB (slab method). On hit fills the entry distance and the entered face's
// outward normal; tNear < 0 means the origin is inside the box (normal stays zero).
bool rayBox(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& bmin,
            const glm::vec3& bmax, float& tNear, glm::ivec3& normal) {
    float t0 = -std::numeric_limits<float>::infinity();
    float t1 = std::numeric_limits<float>::infinity();
    int axis = -1;
    int sign = 0;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-9f) {
            if (origin[a] < bmin[a] || origin[a] > bmax[a]) return false;
            continue;
        }
        float entry = dir[a] > 0.0f ? bmin[a] : bmax[a];
        float exit = dir[a] > 0.0f ? bmax[a] : bmin[a];
        float tEnter = (entry - origin[a]) / dir[a];
        float tExit = (exit - origin[a]) / dir[a];
        if (tEnter > t0) {
            t0 = tEnter;
            axis = a;
            sign = dir[a] > 0.0f ? -1 : 1;
        }
        t1 = std::min(t1, tExit);
    }
    if (t0 > t1 || t1 < 0.0f) return false;
    tNear = t0;
    normal = glm::ivec3(0);
    if (axis >= 0 && t0 >= 0.0f) normal[axis] = sign;
    return true;
}

} // namespace

RaycastHit raycastVoxel(const World& world, const glm::vec3& origin,
                        const glm::vec3& dir, float maxDistance) {
    RaycastHit result;

    glm::vec3 d = dir;
    float len = glm::length(d);
    if (len < 1e-6f) return result;
    d /= len;

    const BlockRegistry& reg = world.registry();

    // Current voxel.
    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    int stepX = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
    int stepY = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    int stepZ = d.z > 0 ? 1 : (d.z < 0 ? -1 : 0);

    const float inf = std::numeric_limits<float>::infinity();
    float tDeltaX = d.x != 0.0f ? std::abs(1.0f / d.x) : inf;
    float tDeltaY = d.y != 0.0f ? std::abs(1.0f / d.y) : inf;
    float tDeltaZ = d.z != 0.0f ? std::abs(1.0f / d.z) : inf;

    // Distance along the ray to the first voxel boundary on each axis.
    auto firstBoundary = [](float o, int step) {
        float cell = std::floor(o);
        return step > 0 ? (cell + 1.0f - o) : (o - cell);
    };
    float tMaxX = d.x != 0.0f ? firstBoundary(origin.x, stepX) * tDeltaX : inf;
    float tMaxY = d.y != 0.0f ? firstBoundary(origin.y, stepY) * tDeltaY : inf;
    float tMaxZ = d.z != 0.0f ? firstBoundary(origin.z, stepZ) * tDeltaZ : inf;

    float t = 0.0f;
    while (t <= maxDistance) {
        // Shape-aware: test the ray against the block's actual selection boxes, so
        // partial blocks (slabs, fences, doors, plates) hit on their real surfaces and
        // the ray passes through their empty parts.
        BlockState state = world.getState(x, y, z);
        if (!state.isAir()) {
            glm::vec3 cell(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            float bestT = inf;
            glm::ivec3 bestN{0};
            bool found = false;
            for (const AABB& box : reg.block(state).outlineBoxes(state, &world, {x, y, z})) {
                float tn;
                glm::ivec3 n;
                if (rayBox(origin, d, cell + box.min, cell + box.max, tn, n) &&
                    tn <= maxDistance && tn < bestT) {
                    bestT = tn;
                    bestN = n;
                    found = true;
                }
            }
            if (found) {
                result.hit = true;
                result.block = {x, y, z};
                result.normal = bestN;
                result.point = origin + d * std::max(bestT, 0.0f);
                return result;
            }
        }
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            x += stepX;
            t = tMaxX;
            tMaxX += tDeltaX;
        } else if (tMaxY <= tMaxZ) {
            y += stepY;
            t = tMaxY;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            t = tMaxZ;
            tMaxZ += tDeltaZ;
        }
    }
    return result;
}

} // namespace mc
