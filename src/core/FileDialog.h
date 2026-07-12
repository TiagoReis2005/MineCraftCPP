#pragma once

#include <string>

namespace mc {

// Native "open file" dialog for PNG images (skin upload). Blocks until the user picks
// a file or cancels; returns the absolute path, or "" on cancel/unsupported platform.
std::string openPngFileDialog();

} // namespace mc
