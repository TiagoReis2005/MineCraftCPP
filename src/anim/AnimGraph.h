#pragma once

#include "anim/AnimClip.h"
#include "anim/AnimStateMachine.h"

#include <map>
#include <string>
#include <vector>

namespace mc {

class Blackboard;
class Rig;
struct Pose;
struct JsonValue;

// Which bones a layer writes. Empty = the whole body (the base layer). Built from a list of
// bone names, optionally including their descendants (recursive), minus an exclude list.
struct BoneMask {
    std::vector<char> in; // per bone index; empty vector means "all bones"

    bool all() const { return in.empty(); }
    bool has(int bone) const {
        return in.empty() || (bone >= 0 && bone < static_cast<int>(in.size()) && in[bone]);
    }
    void build(const Rig& rig, const std::vector<std::string>& bones, bool recursive,
               const std::vector<std::string>& exclude);
};

// The whole player animation graph: a shared clip library plus one or more layers, each a
// masked state machine, composited bottom-up. Layer 0 is the full-body base (locomotion +
// jumps); higher layers override their masked bones by a weight that fades in/out with the
// layer's activity — e.g. a right-arm "wave" over running legs (docs/animation.md §7, A5).
class AnimGraph {
public:
    // `rig` is used to resolve bone-mask names to indices at load (steve/alex share bone
    // names, so a mask stays valid across a skin swap).
    bool load(const std::string& path, const Rig& rig);
    bool loaded() const { return loaded_; }

    void update(float dt, const Blackboard& bb, float stridePhase, const Rig& rig, Pose& out);

    int layerCount() const { return static_cast<int>(layers_.size()); }
    const std::string& currentState(int layer = 0) const;

    // Events the clips crossed during the last update() (footsteps, hit windows, oar
    // strokes). Gameplay drains these each frame and fans them out to consumers — sound,
    // FUTURE(decals) footprints, FUTURE(snow) deformation, FUTURE(boat) thrust windows.
    const std::vector<AnimFiredEvent>& events() const { return events_; }

    // Rolling per-update introspection (~10s), the source for the FUTURE(anim-debugger)
    // timeline tool (docs/animation.md §10). latestSnapshot() is the current tick.
    struct LayerSnapshot {
        std::string state;
        float       weight = 1.0f;
    };
    struct GraphSnapshot {
        float                      time = 0.0f;   // accumulated seconds
        float                      speed = 0.0f;  // v.speed at this tick
        std::vector<LayerSnapshot> layers;
        int                        eventCount = 0;
    };
    const GraphSnapshot& latestSnapshot() const { return snapshot_; }

    // The snapshot ring, oldest -> newest, for the timeline tool (anim_studio).
    void history(std::vector<GraphSnapshot>& out) const;

    int layerNames(std::vector<std::string>& out) const; // layer names in order; returns count

private:
    struct Layer {
        std::string       name;
        BoneMask          mask;               // empty = full body
        bool              additive = false;   // FUTURE(combat): additive blending (stub)
        AnimStateMachine  machine;
        std::string       emptyState;         // state where the layer is inactive (weight->0)
        float             weight = 0.0f;       // smoothed 0..1 override weight
    };

    std::map<std::string, AnimClip> clips_;   // shared library (stable addresses)
    std::vector<Layer>              layers_;
    bool                            loaded_ = false;

    std::vector<AnimFiredEvent>     events_;   // fired during the last update()
    GraphSnapshot                   snapshot_; // latest introspection frame
    std::vector<GraphSnapshot>      ring_;     // ~10s history for the timeline tool
    size_t                          ringHead_ = 0;
    float                           clock_ = 0.0f;
};

} // namespace mc
