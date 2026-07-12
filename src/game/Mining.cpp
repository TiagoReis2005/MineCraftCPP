#include "game/Mining.h"

#include "world/World.h"

namespace mc {

bool Mining::dig(const glm::ivec3& cell, BlockState state, float need, float dt,
                 const glm::vec3& hitPoint, const glm::ivec3& normal, uint16_t part) {
    int idx = -1;
    for (size_t i = 0; i < cracks_.size(); ++i) {
        if (cracks_[i].cell == cell) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) {
        cracks_.push_back({cell, state, 0.0f});
        idx = static_cast<int>(cracks_.size()) - 1;
    } else if (cracks_[idx].state != state || cracks_[idx].part != part) {
        // Different block in the cell OR a different part of it aimed: restart.
        cracks_[idx].state = state;
        cracks_[idx].progress = 0.0f;
    }

    Crack& c = cracks_[idx];
    c.hitPoint = hitPoint; // follow the aim while digging
    c.normal = normal;
    c.part = part;
    c.progress += dt;
    if (c.progress >= need) {
        cracks_[idx] = cracks_.back();
        cracks_.pop_back();
        dugThisFrame_ = -1;
        return true;
    }
    dugThisFrame_ = idx;
    return false;
}

void Mining::update(const World& world, float dt) {
    for (size_t i = 0; i < cracks_.size();) {
        Crack& c = cracks_[i];
        if (static_cast<int>(i) != dugThisFrame_) {
            c.progress -= 0.5f * dt; // heals at half the dig speed
        }
        bool stale = world.getState(c.cell.x, c.cell.y, c.cell.z) != c.state;
        if (c.progress <= 0.0f || stale) {
            // Swap-remove; keep the active index pointing at the entry it tracked.
            int last = static_cast<int>(cracks_.size()) - 1;
            if (dugThisFrame_ == last) dugThisFrame_ = static_cast<int>(i);
            else if (dugThisFrame_ == static_cast<int>(i)) dugThisFrame_ = -1;
            cracks_[i] = cracks_.back();
            cracks_.pop_back();
        } else {
            ++i;
        }
    }
    dugThisFrame_ = -1;
}

} // namespace mc
