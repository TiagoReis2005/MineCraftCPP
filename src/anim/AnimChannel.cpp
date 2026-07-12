#include "anim/AnimChannel.h"

#include "anim/Json.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mc {
namespace {

MolangExpr parseComponent(const JsonValue& v) {
    if (v.isNumber()) return MolangExpr::constant(static_cast<float>(v.number));
    if (v.isString()) return MolangExpr::parse(v.string);
    return MolangExpr(); // unsupported (nested pre/post objects) -> 0
}

} // namespace

std::string toLowerName(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AnimChannel parseChannel(const JsonValue& v) {
    AnimChannel ch;
    if (v.isArray() && v.array.size() == 3) {
        AnimKey key;
        key.value = {parseComponent(v.array[0]), parseComponent(v.array[1]), parseComponent(v.array[2])};
        ch.keys.push_back(std::move(key));
    } else if (v.isObject()) {
        for (const auto& [time, val] : v.object) {
            if (!val.isArray() || val.array.size() != 3) continue;
            AnimKey key;
            key.time = std::strtof(time.c_str(), nullptr);
            key.value = {parseComponent(val.array[0]), parseComponent(val.array[1]), parseComponent(val.array[2])};
            ch.keys.push_back(std::move(key));
        }
        std::sort(ch.keys.begin(), ch.keys.end(),
                  [](const AnimKey& a, const AnimKey& b) { return a.time < b.time; });
    }
    return ch;
}

glm::vec3 sampleAnimChannel(const AnimChannel& ch, float t, MolangContext& ctx,
                            const glm::vec3& accum) {
    auto evalKey = [&](const AnimVec3& v) {
        glm::vec3 out;
        ctx.thisValue = accum.x; out.x = v.x.eval(ctx);
        ctx.thisValue = accum.y; out.y = v.y.eval(ctx);
        ctx.thisValue = accum.z; out.z = v.z.eval(ctx);
        return out;
    };
    if (ch.keys.empty()) return glm::vec3(0.0f);
    if (ch.keys.size() == 1) return evalKey(ch.keys[0].value);
    // Keyframed: find bracketing keys and lerp.
    if (t <= ch.keys.front().time) return evalKey(ch.keys.front().value);
    if (t >= ch.keys.back().time) return evalKey(ch.keys.back().value);
    for (size_t i = 1; i < ch.keys.size(); ++i) {
        if (t <= ch.keys[i].time) {
            float span = ch.keys[i].time - ch.keys[i - 1].time;
            float f = span > 0.0f ? (t - ch.keys[i - 1].time) / span : 0.0f;
            return glm::mix(evalKey(ch.keys[i - 1].value), evalKey(ch.keys[i].value), f);
        }
    }
    return evalKey(ch.keys.back().value);
}

// Bedrock anim conventions: rotation X negates (180-degree turn about Y between the
// conventions); Z does NOT — Bedrock anim Z-rotations inherit Java's y-down model
// convention (verified: the idle bob must sway the arms OUTWARD; a negated Z clips them
// into the torso). Positions are authored +Z = backward, which is +Z for our -Z model.
// Rotation order Z * Y * X matches the old matrix build (Rz*Ry*Rx).
glm::quat boneRotation(const glm::vec3& rotDeg) {
    glm::vec3 r = glm::radians(glm::vec3(-rotDeg.x, rotDeg.y, rotDeg.z));
    if (r == glm::vec3(0.0f)) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::angleAxis(r.z, glm::vec3(0, 0, 1)) * glm::angleAxis(r.y, glm::vec3(0, 1, 0)) *
           glm::angleAxis(r.x, glm::vec3(1, 0, 0));
}

glm::mat4 boneTransform(const glm::vec3& rotDeg, const glm::vec3& posPx, const glm::vec3& pivot) {
    glm::vec3 p(-posPx.x, posPx.y, posPx.z);
    if (rotDeg == glm::vec3(0.0f) && p == glm::vec3(0.0f)) return glm::mat4(1.0f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), p + pivot);
    m = m * glm::mat4_cast(boneRotation(rotDeg));
    return glm::translate(m, -pivot);
}

} // namespace mc
