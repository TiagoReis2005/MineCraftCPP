#include "anim/AnimGraph.h"

#include "anim/Blackboard.h"
#include "anim/Json.h"
#include "anim/Pose.h"
#include "anim/Rig.h"
#include "core/Paths.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdio>

namespace mc {
namespace {

const std::string kNoState = "";

std::vector<std::string> readStringList(const JsonValue* v) {
    std::vector<std::string> out;
    if (v && v->isArray())
        for (const JsonValue& e : v->array)
            if (e.isString()) out.push_back(e.string);
    return out;
}

} // namespace

void BoneMask::build(const Rig& rig, const std::vector<std::string>& bones, bool recursive,
                     const std::vector<std::string>& exclude) {
    const int n = rig.boneCount();
    in.assign(n, 0);
    for (const std::string& b : bones) {
        int i = rig.findBone(b);
        if (i >= 0) in[i] = 1;
    }
    if (recursive) // topo order: a bone joins the mask if its parent is in it
        for (int i = 0; i < n; ++i) {
            int p = rig.bone(i).parent;
            if (p >= 0 && in[p]) in[i] = 1;
        }
    if (!exclude.empty()) { // remove excluded bones and their subtrees
        std::vector<char> ex(n, 0);
        for (const std::string& b : exclude) {
            int e = rig.findBone(b);
            if (e >= 0) ex[e] = 1;
        }
        for (int i = 0; i < n; ++i) {
            int p = rig.bone(i).parent;
            if (p >= 0 && ex[p]) ex[i] = 1;
        }
        for (int i = 0; i < n; ++i)
            if (ex[i]) in[i] = 0;
    }
}

bool AnimGraph::load(const std::string& path, const Rig& rig) {
    JsonValue root;
    try {
        root = parseJsonFile(path);
    } catch (const std::exception& e) {
        std::printf("[Graph] %s unreadable: %s\n", path.c_str(), e.what());
        return false;
    }

    // Shared clip library: name -> file under assets/anim/clips/.
    if (const JsonValue* clips = root.find("clips"); clips && clips->isObject())
        for (const auto& [name, file] : clips->object) {
            if (!file.isString()) continue;
            AnimClip clip;
            if (clip.load(resolve("assets/anim/clips/" + file.string)))
                clips_[name] = std::move(clip);
        }

    // Layers (A5). Backward-compatible: a graph with a top-level "states" and no "layers"
    // is one full-body layer.
    auto addLayer = [&](const JsonValue& container, const std::string& name, const BoneMask& mask,
                        bool additive, const std::string& emptyState) {
        Layer L;
        L.name = name;
        L.mask = mask;
        L.additive = additive;
        L.emptyState = emptyState;
        if (!L.machine.build(container, clips_)) return;
        if (additive)
            std::printf("[Graph] layer '%s' is additive — not implemented yet "
                        "(see FUTURE(combat) in docs/animation.md); layer skipped at runtime\n",
                        name.c_str());
        layers_.push_back(std::move(L));
    };

    if (const JsonValue* layers = root.find("layers"); layers && layers->isArray()) {
        for (const JsonValue& lj : layers->array) {
            if (!lj.isObject()) continue;
            const JsonValue* nm = lj.find("name");
            std::string name = nm && nm->isString() ? nm->string : "layer";
            BoneMask mask; // empty = full body unless a "mask" object narrows it
            if (const JsonValue* m = lj.find("mask"); m && m->isObject()) {
                const JsonValue* rec = m->find("recursive");
                bool recursive = rec && rec->type == JsonValue::Type::Bool && rec->boolean;
                mask.build(rig, readStringList(m->find("bones")), recursive,
                           readStringList(m->find("exclude")));
            }
            bool additive = false;
            if (const JsonValue* a = lj.find("additive"))
                additive = a->type == JsonValue::Type::Bool && a->boolean;
            const JsonValue* es = lj.find("empty_state");
            addLayer(lj, name, mask, additive, es && es->isString() ? es->string : "");
        }
    } else {
        addLayer(root, "base", BoneMask{}, false, "");
    }

    loaded_ = !layers_.empty() && layers_[0].machine.loaded();
    std::printf("[Graph] loaded %s (%zu layers, %zu clips)\n", path.c_str(), layers_.size(),
                clips_.size());
    return loaded_;
}

const std::string& AnimGraph::currentState(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return kNoState;
    return layers_[layer].machine.currentState();
}

void AnimGraph::history(std::vector<GraphSnapshot>& out) const {
    out.clear();
    out.reserve(ring_.size());
    // Not full: ringHead_ is 0 and ring_ is already oldest->newest. Full: oldest is at ringHead_.
    for (size_t i = 0; i < ring_.size(); ++i) out.push_back(ring_[(ringHead_ + i) % ring_.size()]);
}

int AnimGraph::layerNames(std::vector<std::string>& out) const {
    out.clear();
    for (const Layer& L : layers_) out.push_back(L.name);
    return static_cast<int>(layers_.size());
}

void AnimGraph::update(float dt, const Blackboard& bb, float stridePhase, const Rig& rig,
                       Pose& out) {
    events_.clear();
    if (!loaded_) { out.reset(rig); return; }
    clock_ += dt;

    layers_[0].machine.update(dt, bb, stridePhase, rig, out, &events_); // base, full body

    Pose lp(rig);
    for (size_t i = 1; i < layers_.size(); ++i) {
        Layer& L = layers_[i];
        L.machine.update(dt, bb, stridePhase, rig, lp, &events_);
        if (L.additive) continue; // FUTURE(combat)

        float target = (L.emptyState.empty() || L.machine.currentState() != L.emptyState) ? 1.0f
                                                                                          : 0.0f;
        L.weight += (target - L.weight) * std::min(1.0f, dt * 12.0f);
        if (L.weight <= 0.001f) continue;

        const int n = rig.boneCount();
        for (int b = 0; b < n; ++b) {
            if (!L.mask.has(b)) continue;
            glm::quat qb = lp.rot[b];
            if (glm::dot(out.rot[b], qb) < 0.0f) qb = -qb;
            out.rot[b] = glm::normalize(out.rot[b] * (1.0f - L.weight) + qb * L.weight);
            out.pos[b] = glm::mix(out.pos[b], lp.pos[b], L.weight);
        }
    }

    // Introspection snapshot for the timeline tool (FUTURE(anim-debugger)).
    snapshot_.time = clock_;
    snapshot_.speed = bb.getFloat("speed");
    snapshot_.eventCount = static_cast<int>(events_.size());
    snapshot_.layers.resize(layers_.size());
    for (size_t i = 0; i < layers_.size(); ++i) {
        snapshot_.layers[i].state = layers_[i].machine.currentState();
        snapshot_.layers[i].weight = i == 0 ? 1.0f : layers_[i].weight;
    }
    constexpr size_t kRing = 600; // ~10s at 60fps
    if (ring_.size() < kRing)
        ring_.push_back(snapshot_);
    else {
        ring_[ringHead_] = snapshot_;
        ringHead_ = (ringHead_ + 1) % kRing;
    }
}

} // namespace mc
