#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mc {

// Analytic two-bone IK (hip -> knee -> foot), solved in one space (our model-pixel space).
//   hip     : the upper bone's joint position (fixed).
//   target  : where the foot should land.
//   L1, L2  : upper (hip->knee) and lower (knee->foot) bone lengths.
//   poleHint: biases the knee-bend plane (e.g. model -Z so the knee points forward).
// Outputs the WORLD-space rotation for each bone that takes its rest down-axis (0,-1,0) onto
// the solved bone direction. The caller converts these to the pose's local rotations.
// If the target is out of reach the leg is left straight toward it (clamped).
//
// Foot-lock IK (docs/animation.md FUTURE(ik)): physics still moves the body; when a foot is
// planted we pin its world position and solve this each frame so it doesn't slide, using the
// split shin/leg bones and the foot_down/foot_up events.
void solveTwoBoneIK(const glm::vec3& hip, const glm::vec3& target, float L1, float L2,
                    const glm::vec3& poleHint, glm::quat& upperWorld, glm::quat& lowerWorld);

} // namespace mc
