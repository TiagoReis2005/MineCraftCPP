#include "anim/AnimClip.h"

#include "anim/Json.h"
#include "anim/Pose.h"
#include "anim/Rig.h"
#include "core/Paths.h"

#include <glm/glm.hpp>

#include <cstdio>

namespace mc {
namespace {

// Parse a "bones" object ({ "boneName": { "rotation": .., "position": .. } }) into `out`,
// overriding any existing entry. Shared by native clips and Bedrock import.
void parseBones(const JsonValue& bones, std::map<std::string, AnimBone>& out) {
    for (const auto& [name, bj] : bones.object) {
        AnimBone bone;
        if (const JsonValue* r = bj.find("rotation")) bone.rotation = parseChannel(*r);
        if (const JsonValue* p = bj.find("position")) bone.position = parseChannel(*p);
        if (bone.rotation.present() || bone.position.present())
            out[toLowerName(name)] = std::move(bone);
    }
}

// Largest keyframe time across all channels — the fallback duration when unspecified.
float maxKeyTime(const std::map<std::string, AnimBone>& bones) {
    float m = 0.0f;
    auto scan = [&](const AnimChannel& ch) {
        for (const AnimKey& k : ch.keys) m = k.time > m ? k.time : m;
    };
    for (const auto& [name, b] : bones) { scan(b.rotation); scan(b.position); }
    return m;
}

} // namespace

bool AnimClip::importBedrock(const std::string& file, const std::string& animation) {
    JsonValue root;
    try {
        root = parseJsonFile(resolve("assets/" + file));
    } catch (const std::exception& e) {
        std::printf("[Clip] import_bedrock %s unreadable: %s\n", file.c_str(), e.what());
        return false;
    }
    const JsonValue* anims = root.find("animations");
    const JsonValue* anim = anims && anims->isObject() ? anims->find(animation) : nullptr;
    if (!anim || !anim->isObject()) {
        std::printf("[Clip] import_bedrock: '%s' not found in %s\n", animation.c_str(), file.c_str());
        return false;
    }
    if (const JsonValue* len = anim->find("animation_length"); len && len->isNumber())
        duration_ = static_cast<float>(len->number);
    if (const JsonValue* loop = anim->find("loop"))
        loop_ = loop->type == JsonValue::Type::Bool && loop->boolean;
    if (const JsonValue* bones = anim->find("bones"); bones && bones->isObject())
        parseBones(*bones, bones_);
    return true;
}

bool AnimClip::load(const std::string& path) {
    JsonValue root;
    try {
        root = parseJsonFile(path);
    } catch (const std::exception& e) {
        std::printf("[Clip] %s unreadable: %s\n", path.c_str(), e.what());
        return false;
    }

    // Optional Bedrock import first (sets bones/duration/loop), then native overrides.
    if (const JsonValue* imp = root.find("import_bedrock"); imp && imp->isObject()) {
        const JsonValue* f = imp->find("file");
        const JsonValue* a = imp->find("animation");
        if (f && f->isString() && a && a->isString()) importBedrock(f->string, a->string);
    }

    if (const JsonValue* bones = root.find("bones"); bones && bones->isObject())
        parseBones(*bones, bones_);

    if (const JsonValue* d = root.find("duration"); d && d->isNumber())
        duration_ = static_cast<float>(d->number);
    if (const JsonValue* l = root.find("loop"))
        loop_ = l->type == JsonValue::Type::Bool && l->boolean;

    if (const JsonValue* evs = root.find("events"); evs && evs->isArray()) {
        for (const JsonValue& e : evs->array) {
            if (!e.isObject()) continue;
            ClipEvent ev;
            if (const JsonValue* t = e.find("time"); t && t->isNumber()) ev.time = static_cast<float>(t->number);
            if (const JsonValue* n = e.find("name"); n && n->isString()) ev.name = n->string;
            if (const JsonValue* b = e.find("bone"); b && b->isString()) ev.bone = b->string;
            if (const JsonValue* a = e.find("arg"); a && a->isString()) ev.arg = a->string;
            events_.push_back(std::move(ev));
        }
    }

    if (const JsonValue* cs = root.find("curves"); cs && cs->isObject()) {
        for (const auto& [name, keysJson] : cs->object) {
            if (!keysJson.isArray()) continue;
            std::vector<ScalarKey>& keys = curves_[name];
            for (const JsonValue& k : keysJson.array) {
                if (!k.isObject()) continue;
                ScalarKey sk;
                if (const JsonValue* t = k.find("time"); t && t->isNumber()) sk.time = static_cast<float>(t->number);
                if (const JsonValue* v = k.find("value"); v && v->isNumber()) sk.value = static_cast<float>(v->number);
                keys.push_back(sk);
            }
        }
    }

    // FUTURE(smooth-keys): catmull-rom interpolation. A2 is linear + step only.
    if (duration_ <= 0.0f) duration_ = maxKeyTime(bones_);
    loaded_ = true;
    std::printf("[Clip] loaded %s (%.2fs, %s, %zu bones)\n", path.c_str(), duration_,
                loop_ ? "loop" : "once", bones_.size());
    return true;
}

void AnimClip::sample(float time, const Rig& rig, Pose& pose, MolangContext& ctx) const {
    for (const auto& [name, bone] : bones_) {
        int idx = rig.findBoneCI(name);
        if (idx < 0) continue;
        if (bone.rotation.present())
            pose.rot[idx] = boneRotation(sampleAnimChannel(bone.rotation, time, ctx, glm::vec3(0.0f)));
        if (bone.position.present()) {
            glm::vec3 p = sampleAnimChannel(bone.position, time, ctx, glm::vec3(0.0f));
            pose.pos[idx] = {-p.x, p.y, p.z};
        }
    }
}

float AnimClip::curve(const std::string& name, float time) const {
    auto it = curves_.find(name);
    if (it == curves_.end() || it->second.empty()) return 0.0f;
    const std::vector<ScalarKey>& keys = it->second;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            float span = keys[i].time - keys[i - 1].time;
            float f = span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
            return glm::mix(keys[i - 1].value, keys[i].value, f);
        }
    }
    return keys.back().value;
}

void AnimClip::collectEvents(float fromTime, float toTime,
                             std::vector<const ClipEvent*>& out) const {
    for (const ClipEvent& e : events_)
        if (e.time > fromTime && e.time <= toTime) out.push_back(&e);
}

} // namespace mc
