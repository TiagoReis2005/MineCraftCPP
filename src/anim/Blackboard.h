#pragma once

#include "anim/Molang.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mc {

// Typed parameter store: gameplay writes world state here, the animation graph reads it
// (as Molang v.<name>). This is the P1 contract — the runtime never queries the world, it
// only reads the blackboard (docs/animation.md §6). Combat/parkour/mounts just add params.
//
// Triggers are one-shot bools that stay true until clearTriggers() (call once per tick,
// after the graph has read them): input code fires "jump_pressed" and forgets it.
class Blackboard {
public:
    void setFloat(const std::string& name, float v) { values_[name] = v; }
    void setBool(const std::string& name, bool v) { values_[name] = v ? 1.0f : 0.0f; }
    void setInt(const std::string& name, int v) { values_[name] = static_cast<float>(v); }
    void fire(const std::string& name) { triggers_.insert(name); }

    float getFloat(const std::string& name, float def = 0.0f) const {
        auto it = values_.find(name);
        return it == values_.end() ? def : it->second;
    }
    bool getBool(const std::string& name) const {
        return getFloat(name) != 0.0f || triggers_.count(name) != 0;
    }

    // Copy every value + active trigger into a Molang context as variable.<name>, so
    // expressions (blend weights, transition conditions) read them via v.<name>.
    void toMolang(MolangContext& ctx) const {
        for (const auto& [name, v] : values_) ctx.vars["variable." + name] = v;
        for (const auto& name : triggers_) ctx.vars["variable." + name] = 1.0f;
    }

    void clearTriggers() { triggers_.clear(); }

private:
    std::unordered_map<std::string, float> values_;
    std::unordered_set<std::string>        triggers_;
};

} // namespace mc
