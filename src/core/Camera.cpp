#include "core/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace mc {

void Camera::addLook(float dx, float dy) {
    yaw += dx * lookSensitivity;
    pitch -= dy * lookSensitivity;
    pitch = std::clamp(pitch, -89.0f, 89.0f);
}

glm::vec3 Camera::front() const {
    float yawR = glm::radians(yaw);
    float pitchR = glm::radians(pitch);
    return glm::normalize(glm::vec3(
        std::cos(yawR) * std::cos(pitchR),
        std::sin(pitchR),
        std::sin(yawR) * std::cos(pitchR)));
}

glm::vec3 Camera::forwardXZ() const {
    float yawR = glm::radians(yaw);
    return glm::normalize(glm::vec3(std::cos(yawR), 0.0f, std::sin(yawR)));
}

glm::vec3 Camera::rightXZ() const {
    return glm::normalize(glm::cross(forwardXZ(), glm::vec3(0, 1, 0)));
}

glm::mat4 Camera::view() const {
    glm::vec3 eye = renderEye();
    glm::vec3 up(0, 1, 0);
    if (perspective == Perspective::First) {
        return glm::lookAt(eye, eye + front(), up);
    }
    // Third-person: orbit the eye from behind or in front (distance clamped by Player to
    // avoid clipping into walls).
    glm::vec3 dir = (perspective == Perspective::ThirdBack) ? -front() : front();
    return glm::lookAt(eye + dir * thirdPersonDist, eye, up);
}

glm::mat4 Camera::projection(float aspect) const {
    glm::mat4 proj = glm::perspective(glm::radians(fovDeg), aspect, 0.1f, farPlane);
    proj[1][1] *= -1.0f; // flip Y for Vulkan's clip space
    return proj;
}

} // namespace mc
