#include "world/ModelDisplay.h"

#include "anim/Json.h"

#include <exception>

namespace mc {
namespace {

// Vanilla block/block display defaults, used for entries nothing in the chain defines.
const ItemTransform kDefaultGui{{30.0f, 225.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                {0.625f, 0.625f, 0.625f}};
const ItemTransform kDefaultFirstPerson{{0.0f, 45.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                        {0.40f, 0.40f, 0.40f}};
const ItemTransform kDefaultThirdPerson{{75.0f, 45.0f, 0.0f}, {0.0f, 2.5f, 0.0f},
                                        {0.375f, 0.375f, 0.375f}};

glm::vec3 readVec3(const JsonValue* v, glm::vec3 out) {
    if (v && v->isArray() && v->array.size() >= 3) {
        for (size_t i = 0; i < 3; ++i) {
            if (v->array[i].isNumber()) out[static_cast<int>(i)] = static_cast<float>(v->array[i].number);
        }
    }
    return out;
}

bool readEntry(const JsonValue& display, const char* key, ItemTransform* out) {
    const JsonValue* e = display.find(key);
    if (!e || !e->isObject()) return false;
    out->rotationDeg = readVec3(e->find("rotation"), glm::vec3(0.0f));
    out->translation = readVec3(e->find("translation"), glm::vec3(0.0f));
    out->scale = readVec3(e->find("scale"), glm::vec3(1.0f));
    return true;
}

// "minecraft:block/cube_all" -> "cube_all" (the staged models live flat in one folder).
std::string modelStem(const std::string& ref) {
    size_t slash = ref.find_last_of('/');
    return slash == std::string::npos ? ref : ref.substr(slash + 1);
}

} // namespace

DisplaySet loadModelDisplay(const std::string& modelsDir, const std::string& modelName) {
    DisplaySet set;
    bool haveGui = false, haveFp = false, haveTp = false;
    std::string name = modelStem(modelName);
    for (int depth = 0; depth < 16 && !name.empty(); ++depth) {
        JsonValue root;
        try {
            root = parseJsonFile(modelsDir + "/" + name + ".json");
        } catch (const std::exception&) {
            break; // model missing: keep what was found, defaults cover the rest
        }
        if (const JsonValue* display = root.find("display")) {
            if (!haveGui) haveGui = readEntry(*display, "gui", &set.gui);
            if (!haveFp) haveFp = readEntry(*display, "firstperson_righthand", &set.firstPerson);
            if (!haveTp) haveTp = readEntry(*display, "thirdperson_righthand", &set.thirdPerson);
        }
        if (haveGui && haveFp && haveTp) break;
        const JsonValue* parent = root.find("parent");
        if (!parent || !parent->isString()) break;
        name = modelStem(parent->string);
    }
    if (!haveGui) set.gui = kDefaultGui;
    if (!haveFp) set.firstPerson = kDefaultFirstPerson;
    if (!haveTp) set.thirdPerson = kDefaultThirdPerson;
    return set;
}

} // namespace mc
