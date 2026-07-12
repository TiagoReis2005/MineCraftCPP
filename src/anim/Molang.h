#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace mc {

// Variable/query values for evaluation. Keys are normalized long names
// ("variable.attack_time", "query.life_time"). Unknown names evaluate to 0.
struct MolangContext {
    std::unordered_map<std::string, float> vars;
    float thisValue = 0.0f; // Molang `this`: the channel's accumulated value so far
};

// A parsed Molang expression covering the vanilla player-animation subset:
// arithmetic, comparisons, && || ?: ??, math.sin/cos (degrees), pow, sqrt,
// clamp, lerp, abs, mod, floor, ceil, min, max, variable./query./this reads.
// Unknown functions and variables evaluate to 0 instead of failing.
class MolangExpr {
public:
    MolangExpr() = default; // constant 0

    static MolangExpr constant(float v);
    static MolangExpr parse(const std::string& src); // throws std::runtime_error

    float eval(const MolangContext& ctx) const;

    struct Node; // internal

private:
    std::shared_ptr<const Node> root_;
};

} // namespace mc
