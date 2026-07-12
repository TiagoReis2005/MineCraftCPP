#pragma once

#include "game/Inventory.h"
#include "world/Block.h"

#include <glm/glm.hpp>

#include <random>
#include <vector>

namespace mc {

class World;
class BlockRegistry;

// Dropped-item entities: mini blocks that pop out of broken blocks, fall and rest on
// the ground (bobbing and spinning like Minecraft), magnet toward the player and land
// in the inventory. Unclaimed drops despawn after 5 minutes.
class Drops {
public:
    struct Draw {
        glm::mat4 m;
        uint16_t block;
    };

    struct Item {
        BlockId id = BLOCK_AIR;
        int count = 0;
        glm::vec3 pos{0.0f};     // bottom-center (authoritative, updated per tick)
        glm::vec3 prevPos{0.0f}; // position at the start of the tick (render interpolation)
        glm::vec3 vel{0.0f};
        float age = 0.0f;
        float phase = 0.0f;       // randomizes the bob/spin so piles don't move in sync
        float pickupAfter = 0.0f; // age before the magnet/pickup engages (thrown items)
    };

    // Rolls the broken block's drop pool and spawns the results at the cell.
    void spawnFromBlock(const BlockRegistry& reg, const Block& broken, BlockState state,
                        const glm::ivec3& cell);

    // Tosses items forward from the player (Q drop); short pickup delay so the throw
    // isn't vacuumed straight back into the inventory. inheritVel (the player's velocity)
    // is added on top so a running throw carries the runner's momentum.
    void spawnThrown(BlockId id, int count, const glm::vec3& eye, const glm::vec3& dir,
                     const glm::vec3& inheritVel = glm::vec3(0.0f));

    // Physics + magnet + pickup for one fixed 20 TPS tick (collect = false leaves items
    // lying, e.g. spectator). The previous position is kept so rendering can interpolate.
    void tick(const World& world, const glm::vec3& playerFeet, Inventory& inv, bool collect);

    // Interpolated between the last two ticks (partialTick 0..1) so drops move + bob smoothly.
    void buildDraws(std::vector<Draw>& out, float partialTick) const;

    void clear() { items_.clear(); } // world switch: drops belong to the old world

    // World save round-trip: live items out, saved items back in (fresh bob phases).
    const std::vector<Item>& items() const { return items_; }
    void setItems(std::vector<Item> v) {
        items_ = std::move(v);
        for (Item& it : items_) {
            it.phase = rand01() * 6.2831853f;
            it.prevPos = it.pos;
        }
    }

private:
    std::vector<Item> items_;
    std::mt19937 rng_{std::random_device{}()};

    float rand01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
};

} // namespace mc
