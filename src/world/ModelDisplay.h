#pragma once

#include <glm/glm.hpp>

#include <string>

namespace mc {

// One entry of a Java block-model "display" table: how the item sits in a given context
// (gui slot, hand). Vanilla conventions: rotation in degrees applied X then Y then Z,
// translation in 16ths of a block, scale in blocks.
struct ItemTransform {
    glm::vec3 rotationDeg{0.0f};
    glm::vec3 translation{0.0f};
    glm::vec3 scale{1.0f};
};

struct DisplaySet {
    ItemTransform gui;         // inventory slot icon
    ItemTransform firstPerson; // firstperson_righthand
    ItemTransform thirdPerson; // thirdperson_righthand
};

// Reads <modelsDir>/<modelName>.json and walks its "parent" chain; the nearest model
// defining each display entry wins (vanilla inheritance, e.g. thin_block lifts plates
// in hand, fence_inventory turns the gui icon). Entries no model defines fall back to
// the vanilla block/block defaults, so a missing file still looks right.
DisplaySet loadModelDisplay(const std::string& modelsDir, const std::string& modelName);

} // namespace mc
