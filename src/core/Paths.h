#pragma once

#include <string>

namespace mc {

// Absolute directory containing the running executable (with trailing separator).
const std::string& exeDir();

// Resolve a path relative to the executable directory, e.g.
// resolve("shaders/triangle.vert.spv") or resolve("assets/textures/blocks").
std::string resolve(const std::string& relative);

} // namespace mc
