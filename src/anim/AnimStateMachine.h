#pragma once

#include "anim/AnimClip.h"
#include "anim/Blend1D.h"
#include "anim/Molang.h"
#include "anim/Pose.h"

#include <map>
#include <string>
#include <vector>

namespace mc {

class Blackboard;
class Rig;
struct JsonValue;

// What a state animates: either a single clip or a 1D blend space. FUTURE(parkour):
// Blend2D; FUTURE(combat): Additive — both are loud stubs at the loader for now (P6).
struct AnimNode {
    enum class Kind { Clip, Blend1D };
    Kind            kind = Kind::Clip;
    const AnimClip* clip = nullptr; // Kind::Clip
    Blend1D         blend;          // Kind::Blend1D
    std::string     param;          // Kind::Blend1D: blackboard float driving the blend
};

// A guarded, cross-faded edge to another state. Conditions are Molang over the blackboard
// (v.<name>); the first true transition in priority order wins.
struct AnimTransition {
    int        target = -1;   // resolved state index
    MolangExpr condition;     // evaluated each tick; nonzero = take it
    float      duration = 0.1f; // cross-fade seconds
    int        priority = 0;
    bool       force = false; // may interrupt before interruptibleAfter (FUTURE(combat): hurt/death)
};

struct AnimStateDef {
    std::string                 name;
    AnimNode                    node;
    bool                        oneShot = false;         // plays once, then on_finished
    int                         onFinished = -1;         // state index when a one-shot ends
    float                       interruptibleAfter = 0.0f; // one-shots ignore non-force edges before this
    std::vector<AnimTransition> transitions;             // sorted by priority desc
};

// An event a clip crossed this tick (foot_down, hit windows, oar strokes). `bone` is the
// payload source bone; gameplay resolves its world transform from the composed pose.
struct AnimFiredEvent {
    std::string name;
    std::string bone;
};

// A single-layer animation state machine (docs/animation.md §7). States reference clips /
// blend nodes; transitions cross-fade by freezing the outgoing pose and blending toward the
// incoming one. One machine drives one layer; AnimGraph owns the clip library and composites
// layers (A5).
class AnimStateMachine {
public:
    // Builds states from a container JSON (its "states" object + optional "initial"),
    // resolving clip references against the shared `clips` library. `emptyState` (a state
    // name, may be empty) is where the machine counts as inactive for layer weighting.
    bool build(const JsonValue& container, const std::map<std::string, AnimClip>& clips);
    bool loaded() const { return loaded_; }

    // Advances the machine by dt: evaluates transitions against `bb`, updates the active
    // cross-fade, and samples the current state (blended with the frozen source during a
    // transition) into `outPose`. `stridePhase` in [0,1) drives looping locomotion blends
    // (distance-based, supplied by gameplay so feet track ground travel). If `events` is
    // non-null, events the active/dominant clip crossed this tick are appended to it.
    void update(float dt, const Blackboard& bb, float stridePhase, const Rig& rig, Pose& outPose,
                std::vector<AnimFiredEvent>* events = nullptr);

    const std::string& currentState() const;

private:
    std::vector<AnimStateDef>       states_;
    bool  loaded_ = false;
    int   current_ = 0;
    float stateTime_ = 0.0f;   // seconds in the current state

    // Cross-fade: the outgoing pose is frozen at the transition, blended out over crossDur_.
    Pose  fromPose_;           // frozen source, valid while crossActive_
    Pose  lastOut_;            // last frame's output (the pose frozen on the next transition)
    bool  crossActive_ = false;
    bool  haveLast_ = false;
    float crossT_ = 0.0f;
    float crossDur_ = 0.0f;
    float prevStridePhase_ = 0.0f; // last tick's stride phase (event-crossing detection)

    int   findState(const std::string& name) const;
    void  sampleState(int idx, const Blackboard& bb, float stridePhase, const Rig& rig,
                      Pose& out) const;
    float nodeDuration(const AnimStateDef& s, const Blackboard& bb) const;
    void  fireEvents(const AnimStateDef& s, const Blackboard& bb, float prevStateTime,
                     float stridePhase, std::vector<AnimFiredEvent>* events) const;
};

} // namespace mc
