#pragma once

#include "anim/Rig.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace mc {

struct ModelVertex {
    glm::vec3 pos;   // model space, in "pixels" (1/16 block); feet at y=0
    glm::vec2 uv;    // normalized skin UV
    float     shade; // per-face shading
};

struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t>    indices;
};

// Toggleable skin overlay layers (the options menu's Skin Customization page). Bits
// match the overlay bone names in the Bedrock geometry.
enum SkinLayer : uint32_t {
    SL_Hat         = 1u << 0,
    SL_Jacket      = 1u << 1,
    SL_LeftSleeve  = 1u << 2,
    SL_RightSleeve = 1u << 3,
    SL_LeftPants   = 1u << 4,
    SL_RightPants  = 1u << 5,
    SL_All         = 0x3F,
};

// One mesh per rig bone, indexed to match `Rig` bone indices (empty for non-mesh bones
// like root/waist/items). Each mesh is authored in the shared feet-at-origin pixel
// space; the bone's world transform (from Pose::compose) places it. Meshing a bone into
// its own entry — rather than folding limb halves into a parent — is what lets the
// elbow/knee child bones (leftForearm, leftShin, ...) bend independently.
struct PlayerRigMeshes {
    ModelData bones[kMaxModelBones];
    int       count = 0;
};

// Builds the per-bone player meshes for a 64x64 skin, DATA-DRIVEN from the Bedrock
// geometry in assets/models/entity/player.json — the SAME geometry `rig` was loaded
// from (steve or alex; pass matching `slim`). `layerMask` selects which overlay bones
// (hat/jacket/sleeves/pants) are meshed. Supports box UV and per-face UV cubes, and
// omits faces the geometry disables (joint seams). Empty meshes if the file is unusable.
PlayerRigMeshes buildPlayerRigMeshes(const Rig& rig, bool slim, uint32_t layerMask = SL_All);

} // namespace mc
