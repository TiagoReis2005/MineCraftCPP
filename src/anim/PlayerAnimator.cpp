#include "anim/PlayerAnimator.h"

#include "anim/AnimChannel.h"
#include "anim/Json.h"
#include "anim/Pose.h"
#include "anim/Rig.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace mc {
namespace {

// Bedrock bones we animate. root moves everything; waist/body carry the upper body.
enum Bone { B_Root = 0, B_Waist, B_Body, B_Head, B_LeftArm, B_RightArm, B_LeftLeg, B_RightLeg, B_Count };

const char* kBoneNames[B_Count] = {"root", "waist", "body", "head",
                                   "leftarm", "rightarm", "leftleg", "rightleg"};

// Joint pivots in OUR model space (feet at origin, facing -Z, character right = +X).
const glm::vec3 kPivots[B_Count] = {
    {0, 0, 0},  {0, 12, 0}, {0, 24, 0}, {0, 24, 0},
    {-4, 22, 0}, {4, 22, 0}, {-2, 12, 0}, {2, 12, 0},
};

} // namespace

bool PlayerAnimator::load(const std::string& path) {
    try {
        JsonValue rootJson = parseJsonFile(path);
        const JsonValue* anims = rootJson.find("animations");
        if (!anims || !anims->isObject()) throw std::runtime_error("no \"animations\" object");

        for (const auto& [animName, animJson] : anims->object) {
            BedrockAnim anim;
            if (const JsonValue* len = animJson.find("animation_length"); len && len->isNumber()) {
                anim.length = static_cast<float>(len->number);
            }
            if (const JsonValue* bones = animJson.find("bones"); bones && bones->isObject()) {
                for (const auto& [boneName, boneJson] : bones->object) {
                    AnimBone bone;
                    if (const JsonValue* rot = boneJson.find("rotation")) bone.rotation = parseChannel(*rot);
                    if (const JsonValue* pos = boneJson.find("position")) bone.position = parseChannel(*pos);
                    if (bone.rotation.present() || bone.position.present()) {
                        anim.bones[toLowerName(boneName)] = std::move(bone);
                    }
                }
            }
            anims_[animName] = std::move(anim);
        }
        loaded_ = true;
        std::printf("[Anim] loaded animations from %s (%zu total)\n", path.c_str(), anims_.size());
        return true;
    } catch (const std::exception& e) {
        std::printf("[Anim] FAILED to load %s: %s\n", path.c_str(), e.what());
        return false;
    }
}

MolangContext PlayerAnimator::buildContext(const Input& in) const {
    MolangContext ctx;
    // Bedrock's attack_time rests at 0 (the first-person item anims rely on that);
    // our swing progress rests at 1, so map idle back to 0.
    bool swinging = in.attackTime < 1.0f;
    float attackT = swinging ? glm::clamp(in.attackTime, 0.0f, 1.0f) : 0.0f;
    float dist = in.distanceMoved * 4.0f; // Java-style limbSwing units (~2.4m stride)
    ctx.vars["query.modified_distance_moved"] = dist;
    ctx.vars["query.modified_move_speed"] = in.moveAmount;
    ctx.vars["query.walk_distance"] = in.distanceMoved; // raw meters
    ctx.vars["query.life_time"] = in.lifeTime;
    ctx.vars["query.is_sneaking"] = in.sneakAmount > 0.5f ? 1.0f : 0.0f;
    ctx.vars["query.target_x_rotation"] = 0.0f; // head look is applied outside the animator
    ctx.vars["query.target_y_rotation"] = 0.0f;
    // vanilla: tcos0 = cos(dist * 38.17deg) * move_speed * 57.3 (degrees of limb swing)
    ctx.vars["variable.tcos0"] = std::cos(glm::radians(dist * 38.17f)) * in.moveAmount * 57.3f;
    ctx.vars["variable.attack_time"] = attackT;
    // vanilla body twist while swinging (Java: sin(sqrt(p)*2pi) * 0.2 rad)
    ctx.vars["variable.attack_body_rot_y"] =
        std::sin(glm::radians(std::sqrt(attackT) * 360.0f)) * 11.5f;
    ctx.vars["variable.swim_amount"] = 0.0f;
    ctx.vars["variable.is_holding_left"] = 0.0f;
    ctx.vars["variable.is_holding_right"] = 0.0f;
    // First-person extras (vanilla defines these in player.entity.json scripts).
    // Fades over the swing: keeps the fp attack position terms bounded (a constant 1
    // drives the "-factor*attack_time*15" term to -15px and the arm off screen).
    ctx.vars["variable.first_person_item_rotation_factor"] = swinging ? 1.0f - attackT : 0.0f;
    ctx.vars["variable.hand_bob"] = 0.1f * in.moveAmount; // vanilla clamps per-tick speed to 0.1
    ctx.vars["variable.short_arm_offset_right"] = in.slim ? 0.5f : 0.0f;
    ctx.vars["variable.short_arm_offset_left"] = in.slim ? 0.5f : 0.0f;
    ctx.vars["variable.is_using_vr"] = 0.0f;
    ctx.vars["variable.bob_animation"] = 1.0f;
    ctx.vars["variable.map_angle"] = 0.0f;
    ctx.vars["variable.player_arm_height"] = 1.0f;
    return ctx;
}

void PlayerAnimator::evaluatePose(const Input& in, const Rig& rig, Pose& pose) const {
    pose.reset(rig);
    if (!loaded_) return;

    MolangContext ctx = buildContext(in);

    // --- Layer stack (mirrors the vanilla player render controller for on-foot) ---
    struct Layer { const char* name; float weight; };
    const Layer layers[] = {
        {"animation.player.bob", 1.0f},
        {"animation.player.move.arms", 1.0f},
        {"animation.player.move.legs", 1.0f},
        {"animation.player.attack.rotations", 1.0f},
        {"animation.player.sneaking", in.sneakAmount},
    };

    // Accumulate per-bone rotation (deg) + position (px) in Bedrock space.
    glm::vec3 rot[B_Count]{};
    glm::vec3 pos[B_Count]{};
    for (const Layer& layer : layers) {
        if (layer.weight <= 0.0f) continue;
        auto it = anims_.find(layer.name);
        if (it == anims_.end()) continue;
        const BedrockAnim& anim = it->second;
        for (int b = 0; b < B_Count; ++b) {
            auto bit = anim.bones.find(kBoneNames[b]);
            if (bit == anim.bones.end()) continue;
            const AnimBone& bone = bit->second;
            if (bone.rotation.present()) {
                rot[b] += layer.weight * sampleAnimChannel(bone.rotation, in.lifeTime, ctx, rot[b]);
            }
            if (bone.position.present()) {
                pos[b] += layer.weight * sampleAnimChannel(bone.position, in.lifeTime, ctx, pos[b]);
            }
        }
    }

    // Write each animated logical bone as a LOCAL transform on the matching rig bone.
    // The rig carries the parent hierarchy + true pivots, so Pose::compose reproduces
    // the old baked hierarchy (root>waist>body>head/arms, root>legs); untouched bones
    // (forearms/shins/overlays) inherit their parent via their identity local.
    for (int b = 0; b < B_Count; ++b) {
        int idx = rig.findBoneCI(kBoneNames[b]);
        if (idx < 0) continue;
        pose.rot[idx] = boneRotation(rot[b]);
        pose.pos[idx] = {-pos[b].x, pos[b].y, pos[b].z};
    }
}

void PlayerAnimator::evaluateFirstPerson(const Input& in, glm::mat4& armDelta,
                                         glm::mat4& itemDelta) const {
    armDelta = glm::mat4(1.0f);
    itemDelta = glm::mat4(1.0f);
    if (!loaded_) return;

    MolangContext ctx = buildContext(in);

    // Motion layers only — the base pose (first_person.empty_hand) is authored for
    // Bedrock's internal camera rig, so the camera anchor stays hand-tuned in main and
    // these deltas (all zero at idle) animate on top of it. The item flick's position
    // amplitude (±10px) is meant for Bedrock's attachable rig; damped here so the block
    // stays gripped in the hand instead of flying away.
    // itemToArm: breathing_bob is authored on the item bone, but we want the whole hand
    // to breathe — routing it to the arm moves arm + held block together.
    struct Layer { const char* name; float posScale; bool itemToArm; };
    const Layer layers[] = {
        {"animation.player.first_person.walk", 1.0f, false},
        {"animation.player.first_person.attack_rotation", 1.0f, false},
        {"animation.player.first_person.attack_rotation_item", 0.2f, false},
        {"animation.player.first_person.breathing_bob", 1.0f, true},
    };

    glm::vec3 armRot(0.0f), armPos(0.0f), itemRot(0.0f), itemPos(0.0f);
    for (const Layer& layer : layers) {
        auto it = anims_.find(layer.name);
        if (it == anims_.end()) continue;
        for (const auto& [bone, anim] : it->second.bones) {
            glm::vec3* r = nullptr;
            glm::vec3* p = nullptr;
            if (bone == "rightarm" || (bone == "rightitem" && layer.itemToArm)) {
                r = &armRot;
                p = &armPos;
            } else if (bone == "rightitem") {
                r = &itemRot;
                p = &itemPos;
            } else {
                continue;
            }
            if (anim.rotation.present()) *r += sampleAnimChannel(anim.rotation, in.lifeTime, ctx, *r);
            if (anim.position.present()) {
                *p += layer.posScale * sampleAnimChannel(anim.position, in.lifeTime, ctx, *p);
            }
        }
    }

    armDelta = boneTransform(armRot, armPos, kPivots[B_RightArm]);
    itemDelta = boneTransform(itemRot, itemPos, glm::vec3(0.0f)); // about the item's attach point
}

} // namespace mc
