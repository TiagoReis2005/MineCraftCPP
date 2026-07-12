#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace mc {

class UIRenderer;
class Font;

// Everything the F3 overlay displays; populated each frame by main.
struct DebugInfo {
    const char* version = "MineCraftCPP 0.1";
    float       fps = 0.0f;
    glm::vec3   pos{0.0f};
    float       yaw = 0.0f;
    float       pitch = 0.0f;
    int         chunksLoaded = 0;
    int         chunksRendered = 0;
    int         lodTiles = 0; // far distant-horizon tiles loaded
    std::string timeText; // in-game clock + day (from GameTime)

    uint64_t    gpuMemBytes = 0;
    uint64_t    ramBytes = 0;
    float       cpuPercent = 0.0f;
    float       allocRateMiBs = 0.0f;
    uint32_t    displayW = 0;
    uint32_t    displayH = 0;
    std::string gpuName;

    bool        hasTarget = false;
    glm::ivec3  targetBlock{0};
    glm::ivec3  targetNormal{0};
    std::string targetName;
    std::string targetTags;
};

// Draws the F3-style overlay: left column (world/position), right column (system), and
// the targeted-block panel.
class DebugOverlay {
public:
    void build(UIRenderer& ui, const Font& font, uint32_t screenW, uint32_t screenH,
               const DebugInfo& info) const;
};

} // namespace mc
