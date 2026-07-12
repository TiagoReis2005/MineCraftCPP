#pragma once

#include "anim/AnimChannel.h"
#include "anim/Molang.h"

#include <glm/glm.hpp>

#include <map>
#include <string>

namespace mc {
class Rig;
struct Pose;
}

namespace mc {

struct BedrockAnim {
    float length = 0.0f;
    std::map<std::string, AnimBone> bones; // bone names lowercased
};

// Loads a Bedrock player.animation.json and evaluates the vanilla layer stack
// (bob, move.arms, move.legs, attack.rotations, sneaking) into per-part local
// transforms for our 6-part rig (feet-at-origin pixel space, model facing -Z).
class PlayerAnimator {
public:
    struct Input {
        float distanceMoved = 0.0f; // meters walked (drives stride phase)
        float moveAmount = 0.0f;    // 0..1 walk amplitude
        float lifeTime = 0.0f;      // seconds since spawn (idle bob)
        float attackTime = 1.0f;    // swing progress 0->1 (1 = idle)
        float sneakAmount = 0.0f;   // 0..1 crouch blend
        bool  slim = false;         // Alex arms (short_arm_offset)
    };

    // Additive: call once per animation file. False + console note if missing/broken.
    bool load(const std::string& path);
    bool loaded() const { return loaded_; }

    // Third person: writes the vanilla layer stack (bob + move.arms + move.legs +
    // attack + sneaking) as per-bone LOCAL transforms into `pose`, keyed to the rig's
    // bones by name (root/waist/body/head/leftArm/rightArm/leftLeg/rightLeg). Bones the
    // animator doesn't touch — forearms/shins/overlays — keep their identity local and
    // so inherit their parent limb through Pose::compose. No-op when not loaded.
    // FUTURE(combat): additive upper-body layers write here too; the AnimGraph replaces
    // this fixed stack entirely at A4 (docs/animation.md).
    void evaluatePose(const Input& in, const Rig& rig, Pose& pose) const;

    // First person: motion DELTAS (zero at idle) from the first_person.* animations —
    // armDelta wraps the right arm about its shoulder pivot, itemDelta wraps the held
    // item about its own attach point. Both identity when not loaded.
    void evaluateFirstPerson(const Input& in, glm::mat4& armDelta, glm::mat4& itemDelta) const;

private:
    std::map<std::string, BedrockAnim> anims_;
    bool loaded_ = false;

    MolangContext buildContext(const Input& in) const;
};

} // namespace mc
