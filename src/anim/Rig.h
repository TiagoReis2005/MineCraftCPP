#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace mc {

// Upper bound on bones per rig — matches the fixed-size pose/mesh arrays downstream
// (ModelPose::bones, PlayerRigMeshes::bones). The player rig is 25 bones.
inline constexpr int kMaxModelBones = 32;

// One bone of the skeleton, parsed from a Bedrock geometry (assets/models/entity/*.json).
// Everything is in OUR model space: pixels, feet at origin, model faces -Z, character's
// right is +X. Bedrock's +X = character LEFT, so x is mirrored on load (pivot.x negated).
struct RigBone {
    std::string name;    // exact Bedrock bone name ("leftForearm")
    int         parent = -1; // index into Rig::bones(), -1 = root. Always < own index.
    glm::vec3   pivot{0.0f};  // joint this bone rotates about
};

// A named attach point on a bone (Bedrock "locators"): held items, saddle, foot marks.
struct RigLocator {
    std::string name;
    int         bone = -1;   // owning bone index
    glm::vec3   pos{0.0f};   // offset in OUR model space
};

// The bone hierarchy of one geometry. Bones are topologically sorted (every parent
// precedes its children) so a single forward pass composes world transforms.
// FUTURE(mounts)/FUTURE(boat): load a horse/boat geometry the same way; sockets
// (locators) expose saddle/oar attach points. FUTURE(decals)/FUTURE(snow): foot
// locators on the shins feed footprint/snow consumers.
class Rig {
public:
    // Loads the geometry whose description.identifier matches `identifier`
    // (e.g. "geometry.npc.steve"). Returns false (and leaves the rig empty) on any
    // parse error or missing geometry.
    bool loadFromGeometry(const std::string& path, const std::string& identifier);

    bool empty() const { return bones_.empty(); }
    int  boneCount() const { return static_cast<int>(bones_.size()); }
    const std::vector<RigBone>& bones() const { return bones_; }
    const RigBone& bone(int i) const { return bones_[i]; }

    // -1 if absent. `findBone` is exact (mesh routing); `findBoneCI` is case-insensitive
    // (bridging the animator, which lowercases Bedrock bone names).
    int findBone(const std::string& name) const;
    int findBoneCI(const std::string& name) const;

    const std::vector<RigLocator>& locators() const { return locators_; }
    int findLocator(const std::string& name) const;

private:
    std::vector<RigBone>    bones_;
    std::vector<RigLocator> locators_;
};

} // namespace mc
