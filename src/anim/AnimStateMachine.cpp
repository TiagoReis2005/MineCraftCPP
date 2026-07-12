#include "anim/AnimStateMachine.h"

#include "anim/Blackboard.h"
#include "anim/Json.h"
#include "anim/Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc {
namespace {

const std::string kNoState = "";

float numberOr(const JsonValue* v, float def) {
    return v && v->isNumber() ? static_cast<float>(v->number) : def;
}

} // namespace

int AnimStateMachine::findState(const std::string& name) const {
    for (size_t i = 0; i < states_.size(); ++i)
        if (states_[i].name == name) return static_cast<int>(i);
    return -1;
}

bool AnimStateMachine::build(const JsonValue& container,
                            const std::map<std::string, AnimClip>& clips) {
    states_.clear();
    auto findClip = [&](const std::string& name) -> const AnimClip* {
        auto it = clips.find(name);
        return it == clips.end() ? nullptr : &it->second;
    };

    const JsonValue* statesJson = container.find("states");
    if (!statesJson || !statesJson->isObject()) {
        std::printf("[Graph] layer has no states\n");
        return false;
    }
    const JsonValue& root = container; // "initial" is read from the container below

    // Parse states, keeping target names in a parallel structure; resolve to indices only
    // after every state exists, then sort transitions by priority.
    std::vector<std::vector<std::string>> transTargets; // [state][transition] -> target name
    std::vector<std::string>              onFinishedNames;

    for (const auto& [name, sj] : statesJson->object) {
        AnimStateDef s;
        s.name = name;
        if (const JsonValue* os = sj.find("one_shot"))
            s.oneShot = os->type == JsonValue::Type::Bool && os->boolean;
        s.interruptibleAfter = numberOr(sj.find("interruptible_after"), 0.0f);

        if (const JsonValue* n = sj.find("node"); n && n->isObject()) {
            const JsonValue* type = n->find("type");
            std::string kind = type && type->isString() ? type->string : "clip";
            if (kind == "blend1d") {
                s.node.kind = AnimNode::Kind::Blend1D;
                if (const JsonValue* p = n->find("parameter"); p && p->isString())
                    s.node.param = p->string;
                if (const JsonValue* pts = n->find("points"); pts && pts->isArray())
                    for (const JsonValue& pt : pts->array) {
                        const JsonValue* c = pt.find("clip");
                        if (c && c->isString())
                            if (const AnimClip* clip = findClip(c->string))
                                s.node.blend.add(numberOr(pt.find("value"), 0.0f), clip);
                    }
            } else if (kind == "clip") {
                s.node.kind = AnimNode::Kind::Clip;
                if (const JsonValue* c = n->find("clip"); c && c->isString())
                    s.node.clip = findClip(c->string);
            } else {
                // FUTURE(parkour) blend2d / FUTURE(combat) additive: parsed, not run yet (P6).
                std::printf("[Graph] state '%s': node type '%s' not implemented yet "
                            "(see FUTURE in docs/animation.md); treating as empty\n",
                            name.c_str(), kind.c_str());
            }
        }

        std::vector<std::string> targets;
        if (const JsonValue* trs = sj.find("transitions"); trs && trs->isArray())
            for (const JsonValue& tj : trs->array) {
                if (!tj.isObject()) continue;
                AnimTransition t;
                t.duration = numberOr(tj.find("duration"), 0.1f);
                t.priority = static_cast<int>(numberOr(tj.find("priority"), 0.0f));
                if (const JsonValue* f = tj.find("force"))
                    t.force = f->type == JsonValue::Type::Bool && f->boolean;
                if (const JsonValue* c = tj.find("condition"); c && c->isString()) {
                    try {
                        t.condition = MolangExpr::parse(c->string);
                    } catch (const std::exception& e) {
                        std::printf("[Graph] state '%s': bad condition '%s': %s\n", name.c_str(),
                                    c->string.c_str(), e.what());
                    }
                }
                const JsonValue* to = tj.find("to");
                targets.push_back(to && to->isString() ? to->string : "");
                s.transitions.push_back(std::move(t));
            }

        const JsonValue* of = sj.find("on_finished");
        onFinishedNames.push_back(of && of->isString() ? of->string : "");
        transTargets.push_back(std::move(targets));
        states_.push_back(std::move(s));
    }

    // Resolve target/on_finished names to indices, then sort each state's edges by priority.
    for (size_t i = 0; i < states_.size(); ++i) {
        states_[i].onFinished = findState(onFinishedNames[i]);
        for (size_t j = 0; j < states_[i].transitions.size(); ++j)
            states_[i].transitions[j].target = findState(transTargets[i][j]);
        std::stable_sort(states_[i].transitions.begin(), states_[i].transitions.end(),
                         [](const AnimTransition& a, const AnimTransition& b) {
                             return a.priority > b.priority;
                         });
    }

    current_ = 0;
    if (const JsonValue* init = root.find("initial"); init && init->isString())
        current_ = std::max(0, findState(init->string));
    stateTime_ = 0.0f;
    crossActive_ = false;
    haveLast_ = false;

    loaded_ = !states_.empty();
    return loaded_;
}

const std::string& AnimStateMachine::currentState() const {
    if (!loaded_ || current_ < 0 || current_ >= static_cast<int>(states_.size())) return kNoState;
    return states_[current_].name;
}

float AnimStateMachine::nodeDuration(const AnimStateDef& s, const Blackboard& bb) const {
    if (s.node.kind == AnimNode::Kind::Clip) return s.node.clip ? s.node.clip->duration() : 0.0f;
    return s.node.blend.duration(bb.getFloat(s.node.param));
}

void AnimStateMachine::sampleState(int idx, const Blackboard& bb, float stridePhase,
                                   const Rig& rig, Pose& out) const {
    out.reset(rig);
    if (idx < 0 || idx >= static_cast<int>(states_.size())) return;
    const AnimStateDef& s = states_[idx];
    MolangContext ctx;
    bb.toMolang(ctx);
    if (s.node.kind == AnimNode::Kind::Clip) {
        const AnimClip* c = s.node.clip;
        if (!c) return;
        float t;
        if (s.oneShot)
            t = std::min(stateTime_, c->duration()); // play once, hold the final frame
        else
            t = c->looping() && c->duration() > 0.0f ? std::fmod(stateTime_, c->duration())
                                                     : stateTime_;
        c->sample(t, rig, out, ctx);
    } else {
        s.node.blend.sample(bb.getFloat(s.node.param), stridePhase, rig, out, ctx);
    }
}

void AnimStateMachine::update(float dt, const Blackboard& bb, float stridePhase, const Rig& rig,
                              Pose& outPose, std::vector<AnimFiredEvent>* events) {
    if (!loaded_) { outPose.reset(rig); return; }
    float prevStateTime = stateTime_;
    stateTime_ += dt;

    MolangContext ctx;
    bb.toMolang(ctx);

    // Pick a transition: first true in priority order; one-shots ignore non-force edges until
    // interruptibleAfter. If none and a one-shot has played out, take its on_finished.
    const AnimStateDef& s = states_[current_];
    int fired = -1;
    float firedDur = 0.1f;
    for (const AnimTransition& t : s.transitions) {
        if (t.target < 0 || t.target == current_) continue;
        if (s.oneShot && !t.force && stateTime_ < s.interruptibleAfter) continue;
        if (t.condition.eval(ctx) != 0.0f) {
            fired = t.target;
            firedDur = t.duration;
            break;
        }
    }
    if (fired < 0 && s.oneShot && s.onFinished >= 0 && stateTime_ >= nodeDuration(s, bb)) {
        fired = s.onFinished;
        firedDur = 0.1f;
    }

    if (fired >= 0) {
        if (haveLast_) { // freeze the pose we last showed, cross-fade toward the new state
            fromPose_ = lastOut_;
            crossActive_ = true;
            crossT_ = 0.0f;
            crossDur_ = firedDur;
        }
        current_ = fired;
        stateTime_ = 0.0f;
        prevStateTime = 0.0f; // events in the new state fire from t=0
    }

    Pose live(rig);
    sampleState(current_, bb, stridePhase, rig, live);

    if (crossActive_) {
        crossT_ += dt;
        float a = crossDur_ > 0.0f ? std::min(crossT_ / crossDur_, 1.0f) : 1.0f;
        outPose.blend(fromPose_, live, a);
        if (a >= 1.0f) crossActive_ = false;
    } else {
        outPose = live;
    }

    if (events) fireEvents(states_[current_], bb, prevStateTime, stridePhase, events);
    prevStridePhase_ = stridePhase;

    lastOut_ = outPose;
    haveLast_ = true;
}

void AnimStateMachine::fireEvents(const AnimStateDef& s, const Blackboard& bb, float prevStateTime,
                                  float stridePhase, std::vector<AnimFiredEvent>* events) const {
    // Find the clip whose playhead advanced this tick and the (prev, cur) times it covered.
    const AnimClip* clip = nullptr;
    float pT = 0.0f, cT = 0.0f, dur = 0.0f;
    bool loop = false;
    if (s.node.kind == AnimNode::Kind::Clip) {
        clip = s.node.clip;
        if (clip) {
            dur = clip->duration();
            loop = clip->looping();
            if (s.oneShot) {
                pT = std::min(prevStateTime, dur);
                cT = std::min(stateTime_, dur);
            } else {
                pT = loop && dur > 0.0f ? std::fmod(prevStateTime, dur) : prevStateTime;
                cT = loop && dur > 0.0f ? std::fmod(stateTime_, dur) : stateTime_;
            }
        }
    } else {
        // Blend: only the dominant clip fires, so a blended step emits one event, not two.
        clip = s.node.blend.dominantClip(bb.getFloat(s.node.param));
        if (clip) {
            dur = clip->duration();
            loop = clip->looping();
            pT = prevStridePhase_ * dur;
            cT = stridePhase * dur;
        }
    }
    if (!clip || dur <= 0.0f) return;

    std::vector<const ClipEvent*> hit;
    if (loop && cT < pT) { // wrapped past the loop point this tick
        clip->collectEvents(pT, dur, hit);
        clip->collectEvents(-1e-6f, cT, hit);
    } else {
        clip->collectEvents(pT, cT, hit);
    }
    for (const ClipEvent* e : hit) events->push_back({e->name, e->bone});
}

} // namespace mc
