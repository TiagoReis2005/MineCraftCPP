#include "game/Drops.h"

#include "world/BlockRegistry.h"
#include "world/World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>

namespace mc {
namespace {

constexpr float kTick = 1.0f / 20.0f; // fixed 20 TPS step (matches the world tick)
constexpr float kGravity = 18.0f;
constexpr float kHalf = 0.1f;        // item collision half-width
constexpr float kHeight = 0.2f;      // item collision height
constexpr float kMagnetDist = 1.6f;  // starts gliding toward the player
constexpr float kPickupDist = 0.55f; // lands in the inventory
constexpr float kDespawn = 300.0f;

bool itemCollides(const World& world, const glm::vec3& pos) {
    glm::vec3 mn(pos.x - kHalf, pos.y, pos.z - kHalf);
    glm::vec3 mx(pos.x + kHalf, pos.y + kHeight, pos.z + kHalf);
    return world.boxCollides(mn, mx);
}

} // namespace

void Drops::spawnFromBlock(const BlockRegistry& reg, const Block& broken, BlockState state,
                           const glm::ivec3& cell) {
    for (const Drop& d : broken.drops(state, ToolType::None)) {
        if (d.chance < 1.0f && rand01() > d.chance) continue;
        int count = d.min + static_cast<int>(rand01() * static_cast<float>(d.max - d.min + 1));
        count = std::min(count, d.max);
        if (count <= 0) continue;
        BlockId id = reg.byName(d.item);
        if (id == BLOCK_AIR) { // future non-block items (sticks, saplings...)
            std::fprintf(stderr, "[Drops] no block for item '%s' yet\n", d.item.c_str());
            continue;
        }
        Item item;
        item.id = id;
        item.count = count;
        item.pos = glm::vec3(cell) + glm::vec3(0.5f, 0.3f, 0.5f);
        item.prevPos = item.pos;
        item.vel = glm::vec3((rand01() - 0.5f) * 2.2f, 2.4f + rand01(), (rand01() - 0.5f) * 2.2f);
        item.phase = rand01() * 6.2831853f;
        items_.push_back(item);
    }
}

void Drops::spawnThrown(BlockId id, int count, const glm::vec3& eye, const glm::vec3& dir,
                        const glm::vec3& inheritVel) {
    if (id == BLOCK_AIR || count <= 0) return;
    Item item;
    item.id = id;
    item.count = count;
    item.pos = eye + dir * 0.4f; // straight from the head
    item.prevPos = item.pos;
    item.vel = inheritVel + dir * 5.2f +
               glm::vec3((rand01() - 0.5f) * 0.6f, 1.05f, (rand01() - 0.5f) * 0.6f);
    item.phase = rand01() * 6.2831853f;
    item.pickupAfter = 1.2f;
    items_.push_back(item);
}

void Drops::tick(const World& world, const glm::vec3& playerFeet, Inventory& inv, bool collect) {
    const float dt = kTick;
    glm::vec3 playerCenter = playerFeet + glm::vec3(0.0f, 0.9f, 0.0f);
    for (size_t i = 0; i < items_.size();) {
        Item& it = items_[i];
        it.prevPos = it.pos; // start-of-tick position for render interpolation
        it.age += dt;
        if (it.age > kDespawn) {
            items_[i] = items_.back();
            items_.pop_back();
            continue;
        }

        // Magnet glide once the player is close (thrown items wait out their delay).
        // Only pull toward the player if the inventory can actually take the item — a
        // full bag leaves it on the ground instead of gliding in and bouncing off.
        bool collectable = collect && it.age >= it.pickupAfter && inv.hasRoomFor(it.id);
        glm::vec3 toPlayer = playerCenter - (it.pos + glm::vec3(0.0f, 0.1f, 0.0f));
        float dist = glm::length(toPlayer);
        if (collectable && dist < kMagnetDist && dist > 1e-4f) {
            it.pos += toPlayer / dist * std::min(6.0f * dt, dist);
        }

        // Gravity + axis-wise integration against the world's collision shapes.
        it.vel.y -= kGravity * dt;
        glm::vec3 step = it.vel * dt;
        bool onGround = false;
        glm::vec3 p = it.pos;
        p.x += step.x;
        if (itemCollides(world, p)) { p.x = it.pos.x; it.vel.x = 0.0f; }
        p.z += step.z;
        if (itemCollides(world, p)) { p.z = it.pos.z; it.vel.z = 0.0f; }
        p.y += step.y;
        if (itemCollides(world, p)) {
            p.y = it.pos.y;
            if (it.vel.y < 0.0f) onGround = true;
            it.vel.y = 0.0f;
        }
        if (onGround) { // dirt-grade friction: a short skid on landing, not an ice slide
            float f = std::max(0.0f, 1.0f - 20.0f * dt);
            it.vel.x *= f;
            it.vel.z *= f;
            if (it.vel.x * it.vel.x + it.vel.z * it.vel.z < 0.25f * 0.25f) {
                it.vel.x = 0.0f; // crawl-speed leftovers stop dead
                it.vel.z = 0.0f;
            }
        }
        it.pos = p;
        if (it.pos.y < -64.0f) { // fell out of the world
            items_[i] = items_.back();
            items_.pop_back();
            continue;
        }

        // Pickup.
        if (collectable && dist < kPickupDist) {
            int leftover = inv.add(it.id, it.count);
            if (leftover == 0) {
                items_[i] = items_.back();
                items_.pop_back();
                continue;
            }
            it.count = leftover; // inventory full: the rest stays on the ground
        }
        ++i;
    }

    // Merge same-id piles that end up close: the pile sits at the count-weighted middle
    // and the counts combine (never past a full stack, so once a pile is full the next
    // drops start a new agglomerate beside it).
    constexpr float kMergeDist = 1.0f;
    for (size_t i = 0; i < items_.size(); ++i) {
        for (size_t j = i + 1; j < items_.size();) {
            Item& a = items_[i];
            Item& b = items_[j];
            if (a.id == b.id && a.count + b.count <= Inventory::kMaxStack &&
                glm::distance(a.pos, b.pos) < kMergeDist) {
                float wa = static_cast<float>(a.count), wb = static_cast<float>(b.count);
                a.pos = (a.pos * wa + b.pos * wb) / (wa + wb);
                a.prevPos = a.pos; // don't interp-slide the merged pile from its old spot
                a.vel = (a.vel * wa + b.vel * wb) / (wa + wb);
                // Keep the longer remaining pickup delay of the two.
                float remain = std::max(a.pickupAfter - a.age, b.pickupAfter - b.age);
                a.pickupAfter = a.age + std::max(0.0f, remain);
                a.count += b.count;
                items_[j] = items_.back();
                items_.pop_back();
            } else {
                ++j;
            }
        }
    }
}

void Drops::buildDraws(std::vector<Draw>& out, float partialTick) const {
    // Bigger piles draw as a cluster of mini blocks (visual cap 16; the real count can
    // be anything up to a full stack). Offsets are hashed from the pile's phase so the
    // cluster is stable frame to frame.
    float partial = glm::clamp(partialTick, 0.0f, 1.0f);
    auto hash01 = [](float x) {
        float v = std::sin(x) * 43758.5453f;
        return v - std::floor(v);
    };
    for (const Item& it : items_) {
        // Interpolate the position and the bob/spin clock between the last two ticks so the
        // drop glides and bobs smoothly instead of stepping at 20 Hz.
        glm::vec3 renderPos = glm::mix(it.prevPos, it.pos, partial);
        float renderAge = it.age - kTick * (1.0f - partial);
        float bob = 0.06f * std::sin(renderAge * 2.2f + it.phase) + 0.1f;
        float spin = renderAge * 1.4f + it.phase;
        int visuals = std::min(it.count, 16);
        for (int v = 0; v < visuals; ++v) {
            glm::vec3 off(0.0f);
            if (v > 0) {
                off = glm::vec3((hash01(it.phase * 91.7f + v * 12.99f) - 0.5f) * 0.30f,
                                static_cast<float>(v) * 0.012f,
                                (hash01(it.phase * 45.3f + v * 78.23f) - 0.5f) * 0.30f);
            }
            glm::mat4 m =
                glm::translate(glm::mat4(1.0f), renderPos + off + glm::vec3(0.0f, bob, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), spin + v * 0.4f, glm::vec3(0, 1, 0)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(0.25f)) *
                glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, -0.5f));
            out.push_back({m, it.id});
        }
    }
}

} // namespace mc
