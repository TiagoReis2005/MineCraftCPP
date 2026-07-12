#include "game/Player.h"

#include "core/KeyBinds.h"
#include "world/Block.h"
#include "world/BlockRegistry.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc {
namespace {

constexpr float kWalkSpeed = 4.7f;
constexpr float kFlySpeed = kWalkSpeed * 2.0f; // base fly = twice walking speed
constexpr float kSprint = 1.6f;
constexpr float kFlyBoost = 3.0f;
constexpr float kSneakFactor = 0.35f; // crouch slows you down
constexpr float kGravity = 32.0f;     // = 0.08 blocks/tick^2 at 20 TPS, same as Minecraft
constexpr float kJumpSpeed = 9.8f;    // ~1.25-block jump at the 20 TPS tick (semi-implicit Euler)
constexpr float kTerminal = 60.0f;
constexpr double kDoubleTapSeconds = 0.30;

constexpr float kHalfWidth = 0.3f;
constexpr float kHeight = 1.8f;
constexpr float kEyeHeight = 1.62f;
constexpr float kCrouchDrop = 0.3f;   // how far the camera dips when sneaking

constexpr float kSprintFov = 12.0f;   // extra FOV while sprinting (on top of Player.baseFov)
constexpr float kStepHeight = 0.55f;  // auto-step up to slab height (0.5); taller = jump

// Momentum tuning (exponential approach rates, 1/s): velocity chases the wish direction
// instead of snapping to it, so the player has weight. Ground control stays snappy
// (~90% of full speed in 0.14s); air control is deliberately weak so a jump keeps its
// takeoff momentum and mid-air steering only bends the arc.
constexpr float kGroundAccel = 16.8f;
constexpr float kGroundStop = 14.7f;      // friction when no key is held
constexpr float kAirAccel = 3.7f;
constexpr float kAirDrag = 0.42f;         // gentle air resistance while coasting
constexpr float kSprintJumpBoost = 1.8f;  // vanilla-style forward leap on a sprint jump
constexpr float kFlyAccel = 12.0f;        // flight ramps up quickly...
constexpr float kFlyGlide = 8.0f;         // ...and brakes on release with a short slide
                                          // (~1.2 blk at base speed; 12 felt too grabby,
                                          // 4.2 was a long drift)
constexpr float kFlyGlideY = 12.0f;       // vertical brakes harder: releasing jump/sneak
                                          // settles the altitude quickly

// While a screen (inventory) is open, every game key reads as released so movement,
// mode switching, crouch, etc. all idle; physics still runs. Set per-frame in update().
bool g_acceptInput = true;

bool keyDown(GLFWwindow* w, int key) {
    return g_acceptInput && glfwGetKey(w, key) == GLFW_PRESS;
}

float approach(float current, float target, float t) {
    return current + (target - current) * std::min(1.0f, t);
}

} // namespace

int Player::bindKey(const char* id, int fallback) const {
    return binds_ ? binds_->key(id) : fallback;
}

// Crouch intent (hold or toggle mode). Toggle flips on each sneak-key press.
void Player::updateSneakState(GLFWwindow* window, bool acceptInput) {
    bool keyNow = acceptInput &&
                  glfwGetKey(window, bindKey("sneak", GLFW_KEY_LEFT_SHIFT)) == GLFW_PRESS;
    bool edge = keyNow && !sneakPrev_;
    sneakPrev_ = keyNow;
    if (sneakToggle_) {
        if (edge) sneakLatch_ = !sneakLatch_;
        sneakActive_ = sneakLatch_;
    } else {
        sneakActive_ = keyNow;
    }
}

const char* Player::modeName() const {
    switch (mode) {
        case GameMode::Creative:  return "Creative";
        case GameMode::Survival:  return "Survival";
        case GameMode::Spectator: return "Spectator";
    }
    return "?";
}

// Per-frame: sample the mouse look + mode/perspective/fly-toggle/sneak keys.
void Player::handleInput(GLFWwindow* window, bool acceptInput) {
    g_acceptInput = acceptInput;
    if (acceptInput) {
        handleLook(window);
        handleModeSwitch(window);
        handlePerspective(window);
        if (mode == GameMode::Creative) handleFlyToggle(window);
    } else {
        firstMouse_ = true; // cursor roams free (inventory open): re-sync look on resume
    }
    updateSneakState(window, acceptInput);
}

// One fixed 20 TPS physics step (Minecraft's rate). The eye at the start of the tick is
// kept so updateVisual can interpolate the render position between ticks. Movement input is
// sampled here (per tick); look input was already sampled per frame in handleInput.
void Player::tick(GLFWwindow* window, const World& world) {
    constexpr float h = 1.0f / 20.0f;
    prevPos_ = camera.position;
    switch (mode) {
        case GameMode::Survival:
            moveWalk(window, world, h);
            break;
        case GameMode::Creative:
            if (flying) moveFly(window, world, h, /*collide=*/true);
            else moveWalk(window, world, h);
            break;
        case GameMode::Spectator:
            moveFly(window, world, h, /*collide=*/false); // noclip
            break;
    }
}

// Per-frame: interpolate the render eye between the last two ticks (so 20 TPS looks smooth),
// then advance the visual-only state (crouch dip, sprint FOV, body/head yaw lag, animation,
// 3rd-person camera).
void Player::updateVisual(GLFWwindow* window, const World& world, float dt, float partialTick) {
    camera.renderPosition =
        glm::mix(prevPos_, camera.position, glm::clamp(partialTick, 0.0f, 1.0f));
    applyCameraEffects(window, dt);
    updateBodyYaw(dt);
    updateAnimation(dt);
    updateThirdPersonCamera(world);
}

void Player::updateAnimation(float dt) {
    animTime_ += dt;
    swingT_ = std::min(1.0f, swingT_ + dt / 0.3f); // one swing lasts ~0.3s
    float speedXZ = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);
    // The stride only plays with feet on the ground: flying and mid-air (jumps, falls)
    // freeze the phase and ease the swing back to idle instead of running in the air.
    bool flyingNow = (mode == GameMode::Spectator) || (mode == GameMode::Creative && flying);
    bool striding = !flyingNow && onGround_;
    if (striding) {
        // Accumulate distance travelled (meters); the animator derives the stride phase.
        limbSwing_ += speedXZ * dt;
    }
    float target = striding ? std::min(speedXZ / kWalkSpeed, 1.0f) : 0.0f;
    limbAmount_ += (target - limbAmount_) * std::min(1.0f, dt * 10.0f);
}

namespace {
// Wrap an angle difference (degrees) into [-180, 180].
float wrapDeg(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
} // namespace

void Player::updateBodyYaw(float dt) {
    if (!bodyYawInit_) {
        bodyYaw_ = camera.yaw;
        headYaw_ = camera.yaw;
        bodyYawInit_ = true;
        return;
    }

    // Head trails the camera horizontally by a small amount (soft look-around delay).
    // Pitch is applied straight from camera.pitch elsewhere, so vertical stays exact.
    headYaw_ += wrapDeg(camera.yaw - headYaw_) * std::min(1.0f, dt * 15.0f);
    headYaw_ = wrapDeg(headYaw_);

    float delta = wrapDeg(camera.yaw - bodyYaw_); // how far the head is turned from the body
    float speedXZ = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);
    bool moving = speedXZ > 0.1f;

    if (moving) {
        // Walking: the body turns to follow the look/movement direction fairly quickly.
        bodyYaw_ += delta * std::min(1.0f, dt * 12.0f);
    } else {
        // Standing: the head can turn up to maxHead before the body starts following,
        // then the body eases to keep the head within that limit.
        const float maxHead = 45.0f;
        if (std::fabs(delta) > maxHead) {
            float excess = delta - (delta > 0.0f ? maxHead : -maxHead);
            bodyYaw_ += excess * std::min(1.0f, dt * 10.0f);
        }
    }
    bodyYaw_ = wrapDeg(bodyYaw_);
}

void Player::updateThirdPersonCamera(const World& world) {
    if (camera.perspective == Perspective::First) return;

    glm::vec3 eye = camera.renderEye();
    glm::vec3 dir = (camera.perspective == Perspective::ThirdBack) ? -camera.front() : camera.front();

    const float maxDist = thirdPersonRadius, step = 0.1f, margin = 0.2f;
    float dist = maxDist;
    for (float t = step; t <= maxDist; t += step) {
        glm::vec3 p = eye + dir * t;
        if (world.getBlock(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                           static_cast<int>(std::floor(p.z))) != BLOCK_AIR) {
            dist = t - margin;
            break;
        }
    }
    camera.thirdPersonDist = dist < thirdPersonMinDist ? thirdPersonMinDist : dist;
}

void Player::handleLook(GLFWwindow* window) {
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    if (firstMouse_) {
        lastX_ = mx;
        lastY_ = my;
        firstMouse_ = false;
    }
    float dx = static_cast<float>(mx - lastX_);
    float dy = static_cast<float>(my - lastY_);
    lastX_ = mx;
    lastY_ = my;
    // Front 3rd-person view mirrors horizontal look: you're facing the camera, so moving
    // the mouse right should swing the view the intuitive way (vanilla behavior).
    if (camera.perspective == Perspective::ThirdFront) dx = -dx;
    if (invertMouse_) dy = -dy; // Invert Mouse option (vertical)
    camera.addLook(dx, dy);
    (void)invertFrontLookX; // camera-preset override, currently always-on for front view
}

void Player::handleModeSwitch(GLFWwindow* window) {
    bool now = keyDown(window, bindKey("gamemode", GLFW_KEY_G));
    if (now && !cyclePrev_) {
        mode = static_cast<GameMode>((static_cast<int>(mode) + 1) % 3);
        // Momentum carries across the switch: falling stays falling (a mid-air switch
        // used to zero the velocity and visibly freeze the player for a beat).
        // Switching into creative starts on foot (double-tap Space to fly); only the
        // initial spawn hovers. Spectator ignores this flag and always flies.
        flying = false;
        std::fprintf(stderr, "[Mode] %s\n", modeName());
    }
    cyclePrev_ = now;
}

void Player::handlePerspective(GLFWwindow* window) {
    bool now = keyDown(window, bindKey("perspective", GLFW_KEY_F5));
    if (now && !f5Prev_) {
        int next = (static_cast<int>(camera.perspective) + 1) % 3;
        camera.perspective = static_cast<Perspective>(next);
    }
    f5Prev_ = now;
}

void Player::handleFlyToggle(GLFWwindow* window) {
    bool now = keyDown(window, bindKey("jump", GLFW_KEY_SPACE));
    if (now && !spacePrev_) {
        double t = glfwGetTime();
        if (t - lastSpaceTap_ < kDoubleTapSeconds) {
            flying = !flying;
            velocity_.y = 0.0f;
            std::fprintf(stderr, "[Creative] flight %s\n", flying ? "on" : "off");
        }
        lastSpaceTap_ = t;
    }
    spacePrev_ = now;
}

void Player::applyCameraEffects(GLFWwindow* window, float dt) {
    bool walking = (mode == GameMode::Survival) || (mode == GameMode::Creative && !flying);
    bool crouching = walking && sneakActive_;
    crouching_ = crouching;
    camera.crouchOffset = approach(camera.crouchOffset, crouching ? kCrouchDrop : 0.0f, dt * 12.0f);

    float fovTarget = baseFov + (sprinting_ ? kSprintFov : 0.0f);
    camera.fovDeg = approach(camera.fovDeg, fovTarget, dt * 10.0f);
}

void Player::moveWalk(GLFWwindow* window, const World& world, float dt) {
    // Key CODES (suffixed Key so they don't shadow the speed constants kSprint etc.).
    int fwdKeyCode = bindKey("forward", GLFW_KEY_W), backKeyCode = bindKey("back", GLFW_KEY_S);
    int leftKeyCode = bindKey("left", GLFW_KEY_A), rightKeyCode = bindKey("right", GLFW_KEY_D);
    int jumpKeyCode = bindKey("jump", GLFW_KEY_SPACE), sneakKeyCode = bindKey("sneak", GLFW_KEY_LEFT_SHIFT);
    int sprintKeyCode = bindKey("sprint", GLFW_KEY_LEFT_CONTROL);
    glm::vec3 wish(0.0f);
    if (keyDown(window, fwdKeyCode)) wish += camera.forwardXZ();
    if (keyDown(window, backKeyCode)) wish -= camera.forwardXZ();
    if (keyDown(window, rightKeyCode)) wish += camera.rightXZ();
    if (keyDown(window, leftKeyCode)) wish -= camera.rightXZ();
    bool moving = glm::length(wish) > 0.0001f;
    if (moving) wish = glm::normalize(wish);

    bool crouching = sneakActive_; // hold or toggle, computed in updateSneakState

    // Sprint: Hold = sprint while the key is held; Toggle = tap to latch. Either way it
    // only applies while moving forward and not crouching.
    bool forward = keyDown(window, fwdKeyCode);
    bool sprintNow = keyDown(window, sprintKeyCode);
    if (sprintToggle_) {
        bool edge = sprintNow && !sprintPrev_;
        if (edge && forward && !crouching) sprinting_ = !sprinting_;
    } else {
        if (sprintNow && !crouching && forward) sprinting_ = true;
    }
    sprintPrev_ = sprintNow;
    if (!moving || !forward || crouching) sprinting_ = false;

    float speed = kWalkSpeed * (sprinting_ ? kSprint : 1.0f);
    if (crouching) speed *= kSneakFactor;

    float rate = onGround_ ? (moving ? kGroundAccel : kGroundStop)
                           : (moving ? kAirAccel : kAirDrag);
    float blend = 1.0f - std::exp(-rate * dt);
    velocity_.x += (wish.x * speed - velocity_.x) * blend;
    velocity_.z += (wish.z * speed - velocity_.z) * blend;

    if (onGround_ && keyDown(window, jumpKeyCode)) {
        velocity_.y = kJumpSpeed;
        if (sprinting_ && moving) { // sprint jump leaps forward past the run speed
            velocity_.x += wish.x * kSprintJumpBoost;
            velocity_.z += wish.z * kSprintJumpBoost;
        }
        onGround_ = false;
    }
    velocity_.y -= kGravity * dt;
    if (velocity_.y < -kTerminal) velocity_.y = -kTerminal;

    bool wasOnGround = onGround_;
    glm::vec3 delta = velocity_ * dt;
    glm::vec3 eye = camera.position;

    // On-ground obstacles up to slab height (0.5) are walked up automatically; anything
    // taller needs a jump. The feet land flush on the blocker's actual top in the same
    // frame -- lifting by a fixed step height left the player floating over low steps
    // (snow layers) and visibly falling the difference.
    auto tryStepUp = [&](const glm::vec3& blocked) -> bool {
        if (!wasOnGround || velocity_.y > 0.0f) return false;
        constexpr float e = 1e-3f;
        float feet = blocked.y - kEyeHeight;
        // Highest collision-box top overlapping the body's step-height slice = the exact
        // surface to land on. A full wall pokes above the slice and rejects on height.
        glm::vec3 mn(blocked.x - kHalfWidth + e, feet + e, blocked.z - kHalfWidth + e);
        glm::vec3 mx(blocked.x + kHalfWidth - e, feet + kStepHeight, blocked.z + kHalfWidth - e);
        float topY = -1e9f;
        if (!world.boxCollides(mn, mx, &topY)) return false; // blocked above the step zone
        float lift = topY - feet;
        if (lift <= e || lift > kStepHeight) return false;
        glm::vec3 up = blocked;
        up.y += lift;
        if (collidesAt(world, up)) return false; // no headroom over the step
        eye = up;
        return true;
    };

    // Horizontal resolution. While sneaking on the ground, also refuse moves that would
    // leave the player unsupported (edge protection). `bumpedWall` records a blocked move
    // that couldn't step up, so Auto-Jump can hop it.
    bool bumpedWall = false;
    eye.x += delta.x;
    if (collidesAt(world, eye)) {
        if (!tryStepUp(eye)) {
            eye.x -= delta.x;
            velocity_.x = 0.0f;
            sprinting_ = false; // stumbled into a wall
            bumpedWall = true;
        }
    } else if (crouching && wasOnGround && !hasGroundBelow(world, eye)) {
        eye.x -= delta.x;
        velocity_.x = 0.0f;
    }

    eye.z += delta.z;
    if (collidesAt(world, eye)) {
        if (!tryStepUp(eye)) {
            eye.z -= delta.z;
            velocity_.z = 0.0f;
            sprinting_ = false; // stumbled into a wall
            bumpedWall = true;
        }
    } else if (crouching && wasOnGround && !hasGroundBelow(world, eye)) {
        eye.z -= delta.z;
        velocity_.z = 0.0f;
    }

    // Auto-Jump: hop a one-block obstacle when walking into it (there must be headroom
    // one block up so we don't jump into a ceiling). Disabled while sneaking.
    if (autoJump_ && bumpedWall && wasOnGround && moving && !crouching && velocity_.y <= 0.0f) {
        glm::vec3 up = eye;
        up.y += 1.0f;
        if (!collidesAt(world, up)) {
            velocity_.y = kJumpSpeed;
            wasOnGround = false;
        }
    }

    onGround_ = false;
    eye.y += delta.y;
    {
        constexpr float e = 1e-3f;
        float feet = eye.y - kEyeHeight;
        glm::vec3 mn(eye.x - kHalfWidth + e, feet + e, eye.z - kHalfWidth + e);
        glm::vec3 mx(eye.x + kHalfWidth - e, feet + kHeight - e, eye.z + kHalfWidth - e);
        float topY = -1e9f, bottomY = 1e9f;
        if (world.boxCollides(mn, mx, &topY, &bottomY)) {
            if (delta.y < 0.0f) {
                eye.y = topY + kEyeHeight; // land flush on the highest box (slab = +0.5)
                onGround_ = true;
            } else if (delta.y > 0.0f) {
                eye.y = bottomY - kHeight + kEyeHeight; // bumped the ceiling
            }
            velocity_.y = 0.0f;
        }
    }

    camera.position = eye;
}

void Player::moveFly(GLFWwindow* window, const World& world, float dt, bool collide) {
    int fwdKeyCode = bindKey("forward", GLFW_KEY_W), backKeyCode = bindKey("back", GLFW_KEY_S);
    int leftKeyCode = bindKey("left", GLFW_KEY_A), rightKeyCode = bindKey("right", GLFW_KEY_D);
    int jumpKeyCode = bindKey("jump", GLFW_KEY_SPACE), sneakKeyCode = bindKey("sneak", GLFW_KEY_LEFT_SHIFT);
    int sprintKeyCode = bindKey("sprint", GLFW_KEY_LEFT_CONTROL);
    glm::vec3 forward = camera.forwardXZ(); // horizontal WASD; jump/crouch handle vertical
    glm::vec3 move(0.0f);
    if (keyDown(window, fwdKeyCode)) move += forward;
    if (keyDown(window, backKeyCode)) move -= forward;
    if (keyDown(window, rightKeyCode)) move += camera.rightXZ();
    if (keyDown(window, leftKeyCode)) move -= camera.rightXZ();
    if (keyDown(window, jumpKeyCode)) move += glm::vec3(0, 1, 0);
    if (keyDown(window, sneakKeyCode)) move -= glm::vec3(0, 1, 0);

    bool moving = glm::length(move) > 0.0001f;
    if (moving) move = glm::normalize(move);
    // Fly boost only engages and holds while flying forward (W held), like sprint.
    bool fwdKey = keyDown(window, fwdKeyCode);
    if (keyDown(window, sprintKeyCode) && fwdKey) sprinting_ = true;
    if (!moving || !fwdKey) sprinting_ = false;

    // Flight keeps a little momentum but stops SHORT: velocity ramps toward the wish
    // direction while keys are held and brakes hard on release (vertical hardest, so
    // letting go of jump/sneak pins the altitude right away).
    float speed = kFlySpeed * (sprinting_ ? kFlyBoost : 1.0f);
    float blendXZ = 1.0f - std::exp(-(moving ? kFlyAccel : kFlyGlide) * dt);
    float blendY = 1.0f - std::exp(-(moving ? kFlyAccel : kFlyGlideY) * dt);
    velocity_.x += (move.x * speed - velocity_.x) * blendXZ;
    velocity_.z += (move.z * speed - velocity_.z) * blendXZ;
    velocity_.y += (move.y * speed - velocity_.y) * blendY;
    glm::vec3 delta = velocity_ * dt;

    if (!collide) {
        camera.position += delta;
        return;
    }

    glm::vec3 eye = camera.position;
    eye.x += delta.x;
    if (collidesAt(world, eye)) { eye.x -= delta.x; velocity_.x = 0.0f; sprinting_ = false; }
    eye.z += delta.z;
    if (collidesAt(world, eye)) { eye.z -= delta.z; velocity_.z = 0.0f; sprinting_ = false; }
    eye.y += delta.y;
    if (collidesAt(world, eye)) {
        eye.y -= delta.y;
        velocity_.y = 0.0f;
        if (delta.y < 0.0f) flying = false; // descended onto the ground -> land
    }
    camera.position = eye;
}

bool Player::collidesAt(const World& world, const glm::vec3& eye) const {
    constexpr float e = 1e-3f;
    float feet = eye.y - kEyeHeight;
    glm::vec3 mn(eye.x - kHalfWidth + e, feet + e, eye.z - kHalfWidth + e);
    glm::vec3 mx(eye.x + kHalfWidth - e, feet + kHeight - e, eye.z + kHalfWidth - e);
    return world.boxCollides(mn, mx);
}

bool Player::hasGroundBelow(const World& world, const glm::vec3& eye) const {
    constexpr float e = 1e-3f;
    float feet = eye.y - kEyeHeight;
    // Thin box just under the feet (shape-aware: standing on a bottom slab counts).
    glm::vec3 mn(eye.x - kHalfWidth + e, feet - 0.05f, eye.z - kHalfWidth + e);
    glm::vec3 mx(eye.x + kHalfWidth - e, feet - 0.001f, eye.z + kHalfWidth - e);
    return world.boxCollides(mn, mx);
}

bool Player::collidesWithBlock(const World& world, const BlockRegistry& reg, BlockState placed,
                               const glm::ivec3& block) const {
    float feet = camera.position.y - kEyeHeight;
    glm::vec3 pMin(camera.position.x - kHalfWidth, feet, camera.position.z - kHalfWidth);
    glm::vec3 pMax(camera.position.x + kHalfWidth, feet + kHeight, camera.position.z + kHalfWidth);
    glm::vec3 cell(static_cast<float>(block.x), static_cast<float>(block.y), static_cast<float>(block.z));

    for (const AABB& box : reg.block(placed).collisionBoxes(placed, &world, block)) {
        glm::vec3 b0 = cell + box.min;
        glm::vec3 b1 = cell + box.max;
        if (pMin.x < b1.x && pMax.x > b0.x && pMin.y < b1.y && pMax.y > b0.y &&
            pMin.z < b1.z && pMax.z > b0.z) {
            return true;
        }
    }
    return false;
}

} // namespace mc
