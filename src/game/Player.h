#pragma once

#include "core/Camera.h"
#include "world/Block.h"

#include <glm/glm.hpp>

struct GLFWwindow;

namespace mc {

class World;
class BlockRegistry;
class KeyBinds;

enum class GameMode { Creative, Survival, Spectator };

// Owns the camera plus movement/physics, branching on the game mode:
//   Creative  - walks like survival; double-tap jump toggles flight (horizontal WASD +
//               jump/crouch for up/down), collides with blocks, can edit
//   Survival  - gravity + jump + AABB collision, can edit
//   Spectator - free look-direction flight, noclip, cannot edit
class Player {
public:
    Camera camera;
    GameMode mode = GameMode::Creative;
    bool flying = true; // hovers only until the spawn column generates; then set on foot

    // Camera tuning, loaded from assets/cameras/*.json (Bedrock camera presets).
    float baseFov = 70.0f;            // field_of_view (sprint FOV boost adds on top)
    float thirdPersonRadius = 4.0f;   // camera_orbit.radius
    float thirdPersonMinDist = 0.3f;  // camera_avoidance.distance_constraint_min
    bool invertFrontLookX = false;    // camera_orbit.invert_x_input (front view)

    // Split for the fixed 20 TPS tick (like Minecraft): handleInput samples the mouse/keys
    // once per frame; tick() runs one fixed physics step; updateVisual() runs per frame and
    // interpolates the render eye between the last two ticks so movement stays smooth at any
    // framerate. acceptInput=false (a screen is open) freezes look/movement/mode keys but
    // physics keeps running (the player still falls while the inventory is open).
    void handleInput(GLFWwindow* window, bool acceptInput);
    void tick(GLFWwindow* window, const World& world);
    void updateVisual(GLFWwindow* window, const World& world, float dt, float partialTick);
    // Snap the interpolation to the current position (call after a teleport/spawn/load so
    // the render eye doesn't slide across the map from a stale previous tick).
    void resetInterp() { prevPos_ = camera.position; camera.renderPosition = camera.position; }
    bool canEdit() const { return mode != GameMode::Spectator; }

    // Drop the next mouse delta (re-read the cursor as the new origin). Call when the
    // cursor was free (menu/pause) and is being recaptured, so the accumulated offset
    // doesn't snap the camera on resume.
    void syncLook() { firstMouse_ = true; }

    // Rebindable movement keys (from the Key Binds screen); null = built-in defaults.
    void setKeyBinds(const KeyBinds* binds) { binds_ = binds; }

    // Input options from the menu (applied every frame).
    void setInputOptions(bool invertMouse, bool sneakToggle, bool sprintToggle, bool autoJump) {
        invertMouse_ = invertMouse;
        sneakToggle_ = sneakToggle;
        sprintToggle_ = sprintToggle;
        autoJump_ = autoJump;
    }

    // World-space feet position (interpolated render eye minus eye height), for placing the
    // body model smoothly and the drops magnet target.
    glm::vec3 feetPosition() const { return camera.renderPosition - glm::vec3(0.0f, 1.62f, 0.0f); }

    // Current velocity (all modes) — thrown items inherit it so drops carry your momentum.
    glm::vec3 velocity() const { return velocity_; }

    // Body yaw in degrees. Lags behind camera.yaw so the head can turn a bit before the
    // body follows (Minecraft-style). Same convention as camera.yaw.
    float bodyYaw() const { return bodyYaw_; }

    // Head yaw in degrees: trails camera.yaw slightly for a soft look-around delay
    // (horizontal only; the head pitch tracks the camera exactly).
    float headYaw() const { return headYaw_; }

    // Animation state for the body model.
    float limbSwing() const { return limbSwing_; }        // walk phase (radians)
    float limbSwingAmount() const { return limbAmount_; } // 0..1 blend for swing amplitude
    float animTime() const { return animTime_; }          // seconds, for idle motion

    // Arm-swing (attack) animation: triggerSwing() starts it; progress runs 0->1 (1 = idle).
    void triggerSwing() { swingT_ = 0.0f; }
    float swingProgress() const { return swingT_; }

    // True while sneaking (drives the crouched body lean in the model).
    bool crouching() const { return crouching_; }

    // True when standing on ground (drives jump/fall/land animation transitions).
    bool onGround() const { return onGround_; }

    // True if the player's body box would overlap `placed` at that cell (placement guard;
    // shape-aware, so a top slab can go in a cell whose lower half you occupy).
    bool collidesWithBlock(const World& world, const BlockRegistry& reg, BlockState placed,
                           const glm::ivec3& block) const;

    const char* modeName() const;

private:
    glm::vec3 velocity_{0.0f};
    bool onGround_ = false;
    glm::vec3 prevPos_{0.0f, 100.0f, 0.0f}; // eye at the start of the current tick (interpolation)
    bool firstMouse_ = true;
    double lastX_ = 0.0, lastY_ = 0.0;
    bool cyclePrev_ = false;
    bool f5Prev_ = false;
    bool spacePrev_ = false;
    bool sprinting_ = false; // latched: stays on while moving, off when you stop
    double lastSpaceTap_ = -1.0;
    float bodyYaw_ = -90.0f;   // lagged body facing (degrees)
    float headYaw_ = -90.0f;   // softly-lagged head facing (degrees)
    bool bodyYawInit_ = false; // snap body/head yaw to camera on the first update
    float limbSwing_ = 0.0f;   // accumulated walk phase
    float limbAmount_ = 0.0f;  // smoothed 0..1 swing amplitude
    float animTime_ = 0.0f;    // accumulated time for idle motion
    float swingT_ = 1.0f;      // attack-swing progress (1 = finished/idle)
    bool crouching_ = false;   // sneaking this frame
    const KeyBinds* binds_ = nullptr; // rebindable movement keys

    // Input options + sneak/sprint toggle state.
    bool invertMouse_ = false, sneakToggle_ = false, sprintToggle_ = false, autoJump_ = false;
    bool sneakActive_ = false, sneakLatch_ = false, sneakPrev_ = false, sprintPrev_ = false;

    int bindKey(const char* id, int fallback) const;
    void updateSneakState(GLFWwindow* window, bool acceptInput);

    void handleLook(GLFWwindow* window);
    void handleModeSwitch(GLFWwindow* window);
    void handlePerspective(GLFWwindow* window); // F5 cycles camera perspective
    void updateThirdPersonCamera(const World& world); // pull the 3rd-person camera out of walls
    void updateBodyYaw(float dt); // lag the body behind the head/camera yaw
    void updateAnimation(float dt); // advance walk/idle animation state
    void handleFlyToggle(GLFWwindow* window); // creative double-tap jump
    void applyCameraEffects(GLFWwindow* window, float dt); // crouch dip + sprint FOV
    void moveWalk(GLFWwindow* window, const World& world, float dt);
    void moveFly(GLFWwindow* window, const World& world, float dt, bool collide);
    bool collidesAt(const World& world, const glm::vec3& eye) const;
    bool hasGroundBelow(const World& world, const glm::vec3& eye) const;
};

} // namespace mc
