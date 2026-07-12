#include "anim/Rig.h"

#include "anim/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace mc {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

glm::vec3 readVec3(const JsonValue* v, glm::vec3 def) {
    if (!v || !v->isArray() || v->array.size() < 3) return def;
    return {static_cast<float>(v->array[0].number), static_cast<float>(v->array[1].number),
            static_cast<float>(v->array[2].number)};
}

} // namespace

bool Rig::loadFromGeometry(const std::string& path, const std::string& identifier) {
    bones_.clear();
    locators_.clear();

    JsonValue root;
    try {
        root = parseJsonFile(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Rig] %s unreadable: %s\n", path.c_str(), e.what());
        return false;
    }
    const JsonValue* geos = root.find("minecraft:geometry");
    if (!geos || !geos->isArray()) return false;

    const JsonValue* geo = nullptr;
    for (const JsonValue& g : geos->array) {
        const JsonValue* desc = g.find("description");
        const JsonValue* id = desc ? desc->find("identifier") : nullptr;
        if (id && id->isString() && id->string == identifier) {
            geo = &g;
            break;
        }
    }
    if (!geo) {
        std::fprintf(stderr, "[Rig] identifier %s not found in %s\n", identifier.c_str(),
                     path.c_str());
        return false;
    }
    const JsonValue* bonesJson = geo->find("bones");
    if (!bonesJson || !bonesJson->isArray()) return false;

    // Raw bones in file order; Bedrock allows a child to be listed before its parent
    // (player.json lists "body" before "waist"), so parent links are resolved by name
    // after collecting everything, then topologically sorted below.
    struct Raw {
        std::string name;
        std::string parent;
        glm::vec3   pivot;
    };
    std::vector<Raw> raw;
    // Locators captured with their owning bone NAME, remapped to an index post-sort.
    struct RawLoc {
        std::string name;
        std::string bone;
        glm::vec3   pos;
    };
    std::vector<RawLoc> rawLocs;

    for (const JsonValue& b : bonesJson->array) {
        const JsonValue* nm = b.find("name");
        if (!nm || !nm->isString()) continue;
        Raw r;
        r.name = nm->string;
        if (const JsonValue* p = b.find("parent"); p && p->isString()) r.parent = p->string;
        glm::vec3 piv = readVec3(b.find("pivot"), glm::vec3(0.0f));
        r.pivot = {-piv.x, piv.y, piv.z}; // mirror X into our space
        raw.push_back(std::move(r));

        if (const JsonValue* locs = b.find("locators"); locs && locs->isObject()) {
            for (const auto& [lname, lval] : locs->object) {
                glm::vec3 lp = readVec3(&lval, glm::vec3(0.0f)); // array form [x,y,z]
                rawLocs.push_back({lname, nm->string, {-lp.x, lp.y, lp.z}});
            }
        }
    }
    if (raw.empty()) return false;

    // Topological sort: emit a bone once its parent is already emitted (or it is a root).
    // Stable within that constraint so sibling order follows the file.
    std::vector<int> order;
    order.reserve(raw.size());
    std::vector<bool> placed(raw.size(), false);
    auto indexOf = [&](const std::string& name) -> int {
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i].name == name) return static_cast<int>(i);
        return -1;
    };
    bool progress = true;
    while (order.size() < raw.size() && progress) {
        progress = false;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (placed[i]) continue;
            int p = raw[i].parent.empty() ? -1 : indexOf(raw[i].parent);
            // Ready when root, parent missing entirely, or parent already placed.
            if (p < 0 || placed[p]) {
                order.push_back(static_cast<int>(i));
                placed[i] = true;
                progress = true;
            }
        }
    }
    if (order.size() < raw.size()) { // a parent cycle: emit the rest in file order
        for (size_t i = 0; i < raw.size(); ++i)
            if (!placed[i]) order.push_back(static_cast<int>(i));
    }

    // rawIndex -> final sorted index, so parents (emitted earlier) resolve to a lower index.
    std::vector<int> finalOf(raw.size(), -1);
    for (size_t f = 0; f < order.size(); ++f) finalOf[order[f]] = static_cast<int>(f);

    bones_.reserve(order.size());
    for (int rawIdx : order) {
        RigBone rb;
        rb.name = raw[rawIdx].name;
        rb.pivot = raw[rawIdx].pivot;
        int p = raw[rawIdx].parent.empty() ? -1 : indexOf(raw[rawIdx].parent);
        rb.parent = p < 0 ? -1 : finalOf[p];
        bones_.push_back(std::move(rb));
    }
    for (const RawLoc& rl : rawLocs) {
        locators_.push_back({rl.name, findBone(rl.bone), rl.pos});
    }
    return true;
}

int Rig::findBone(const std::string& name) const {
    for (size_t i = 0; i < bones_.size(); ++i)
        if (bones_[i].name == name) return static_cast<int>(i);
    return -1;
}

int Rig::findBoneCI(const std::string& name) const {
    std::string q = lower(name);
    for (size_t i = 0; i < bones_.size(); ++i)
        if (lower(bones_[i].name) == q) return static_cast<int>(i);
    return -1;
}

int Rig::findLocator(const std::string& name) const {
    for (size_t i = 0; i < locators_.size(); ++i)
        if (locators_[i].name == name) return static_cast<int>(i);
    return -1;
}

} // namespace mc
