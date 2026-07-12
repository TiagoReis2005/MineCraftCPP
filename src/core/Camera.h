#pragma once

#include <glm/glm.hpp>

namespace mc {

enum class Perspective { First = 0, ThirdBack = 1, ThirdFront = 2 };

// View/projection holder. Movement and input live in Player (M4b).
class Camera {
public:
    glm::vec3 position{0.0f, 100.0f, 0.0f};       // authoritative eye (physics/streaming/save)
    glm::vec3 renderPosition{0.0f, 100.0f, 0.0f};  // eye interpolated between ticks (rendering)
    float yaw = -90.0f;   // degrees; -90 looks down -Z
    float pitch = -20.0f;
    float fovDeg = 70.0f;
    float lookSensitivity = 0.08f; // mouse look factor (options menu)
    float crouchOffset = 0.0f; // renders the eye this much lower (sneaking), no physics effect
    Perspective perspective = Perspective::First;
    float thirdPersonDist = 4.0f; // camera distance in 3rd person (pulled in to avoid walls)
    float farPlane = 1000.0f; // clip distance; grows with the view/LOD distance so far
                              // terrain and clouds aren't sliced off by the far plane

    // Eye the frame is actually rendered from: the interpolated position with the crouch
    // dip applied. Anything that must match what the player SEES (view, raycast targeting,
    // 1st-person arm, sky) starts here -- physics/streaming/save use `position` directly.
    glm::vec3 renderEye() const { return renderPosition - glm::vec3(0.0f, crouchOffset, 0.0f); }

    // Apply a mouse delta to the look direction (pitch clamped, lookSensitivity applied).
    void addLook(float dx, float dy);

    glm::vec3 front() const;      // full look direction
    glm::vec3 forwardXZ() const;  // look direction flattened to the ground plane
    glm::vec3 rightXZ() const;    // ground-plane strafe direction

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
};

} // namespace mc
