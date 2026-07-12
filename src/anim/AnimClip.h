#pragma once

#include "anim/AnimChannel.h"
#include "anim/Molang.h"

#include <map>
#include <string>
#include <vector>

namespace mc {

class Rig;
struct Pose;

// A named time marker on a clip. Consumers (footsteps, hit windows, oar strokes) fan out
// from these; the clip itself is agnostic. FUTURE(decals)/FUTURE(combat)/FUTURE(boat).
struct ClipEvent {
    float       time = 0.0f;
    std::string name;
    std::string bone; // payload: source bone (e.g. which foot); may be empty
    std::string arg;  // payload: optional string
};

// One keyframed scalar sample of a named curve.
struct ScalarKey {
    float time = 0.0f;
    float value = 0.0f;
};

// A single animation clip: per-bone keyframe/expression channels, plus named events and
// scalar curves. Authored as assets/anim/clips/*.clip.json (native), optionally importing
// keyframe channels from a Bedrock animation (Blockbench export). See docs/animation.md §5.
//
// This is the A2 building block: a clip samples into a Pose. Blending multiple clips and
// the state machine that selects them arrive at A3/A4.
class AnimClip {
public:
    // Loads a .clip.json. `import_bedrock` (optional) pulls channels from a Bedrock
    // animation first; inline "bones" then add/override. False + console note on error.
    bool load(const std::string& path);
    bool loaded() const { return loaded_; }

    float duration() const { return duration_; }
    bool  looping() const { return loop_; }

    // Samples the clip at `time` seconds, writing per-bone LOCAL transforms into `pose`
    // (bones the clip doesn't mention are left untouched, so it can layer over a base).
    // `ctx` evaluates any Molang in expression channels; a default context suffices for
    // pure-keyframe clips.
    void sample(float time, const Rig& rig, Pose& pose, MolangContext& ctx) const;

    // Value of a named scalar curve at `time` (0 if absent). FUTURE(boat): oar_in_water.
    float curve(const std::string& name, float time) const;

    // Appends events whose time lies in (fromTime, toTime] to `out` (no wrap handling yet;
    // FUTURE: loop-aware firing at A6). FUTURE(decals/combat/boat) consumers drain these.
    void collectEvents(float fromTime, float toTime, std::vector<const ClipEvent*>& out) const;

private:
    bool  loaded_ = false;
    float duration_ = 0.0f;
    bool  loop_ = false;
    std::map<std::string, AnimBone>              bones_;  // bone name (lowercased) -> channels
    std::vector<ClipEvent>                       events_;
    std::map<std::string, std::vector<ScalarKey>> curves_;

    // Pulls the named animation's bone channels + length from a Bedrock animation file.
    bool importBedrock(const std::string& file, const std::string& animation);
};

} // namespace mc
