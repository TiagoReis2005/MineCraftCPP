#pragma once

#include "world/Block.h"

#include <glm/glm.hpp>

#include <vector>

namespace mc {

class World;

// Survival break progress, per block. Digging advances a cell's crack; anything not
// actively dug heals at HALF the dig speed (a crack that took 2s to dig fades over 4s),
// so releasing the button doesn't instantly reset progress — come back in time and the
// dig resumes where it left off. Multiple blocks can carry cracks at once.
// This state belongs to the world save (serialize with M6 save/load).
class Mining {
public:
    struct Crack {
        glm::ivec3 cell{0};
        BlockState state = 0;  // the block the progress belongs to (changed = stale)
        float progress = 0.0f; // seconds of digging accumulated
        // Last aim (world hit point + face normal), so the overlay wraps the aimed
        // PART of the block (a double slab's half) even while the crack heals.
        glm::vec3 hitPoint{0.0f};
        glm::ivec3 normal{0};
        uint16_t part = 0; // Block::aimedPart at the last dig
    };

    // Advance the actively-dug cell by dt (need = total seconds to break). Restarts
    // if the cell now holds a different block OR the aim moved to a different PART
    // (a double slab's halves have independent progress). Returns true when the dig
    // completes (the entry is removed; the caller breaks the block).
    bool dig(const glm::ivec3& cell, BlockState state, float need, float dt,
             const glm::vec3& hitPoint, const glm::ivec3& normal, uint16_t part);

    // Heal every crack that wasn't dug this frame and drop entries whose block changed
    // under them. Call once per frame after any dig().
    void update(const World& world, float dt);

    const std::vector<Crack>& cracks() const { return cracks_; }

    // Replace all break progress (world load; {} on world exit).
    void setCracks(std::vector<Crack> cracks) {
        cracks_ = std::move(cracks);
        dugThisFrame_ = -1;
    }

private:
    std::vector<Crack> cracks_;
    int dugThisFrame_ = -1; // index dig() touched; update() skips healing it
};

} // namespace mc
