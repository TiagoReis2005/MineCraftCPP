#include "ui/DebugOverlay.h"

#include "gfx/UIRenderer.h"
#include "ui/Font.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace mc {
namespace {

const char* facingFromYaw(float yaw) {
    float yawR = yaw * 3.14159265f / 180.0f;
    float fx = std::cos(yawR);
    float fz = std::sin(yawR);
    if (std::fabs(fx) > std::fabs(fz)) return fx > 0 ? "east (+X)" : "west (-X)";
    return fz > 0 ? "south (+Z)" : "north (-Z)";
}

const char* faceFromNormal(const glm::ivec3& n) {
    if (n.y > 0) return "up";
    if (n.y < 0) return "down";
    if (n.x > 0) return "east";
    if (n.x < 0) return "west";
    if (n.z > 0) return "south";
    if (n.z < 0) return "north";
    return "-";
}

double toMiB(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

} // namespace

void DebugOverlay::build(UIRenderer& ui, const Font& font, uint32_t screenW, uint32_t /*screenH*/,
                         const DebugInfo& info) const {
    const float scale = 2.0f;
    const float lineH = font.lineHeight(scale) + 2.0f;
    const glm::vec4 white(1.0f);
    char buf[160];

    // ---- Left column ----
    std::vector<std::string> left;
    left.emplace_back(info.version);
    std::snprintf(buf, sizeof(buf), "%.0f fps", info.fps); left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "XYZ: %.2f / %.2f / %.2f", info.pos.x, info.pos.y, info.pos.z);
    left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "Block: %d %d %d", (int)std::floor(info.pos.x),
                  (int)std::floor(info.pos.y), (int)std::floor(info.pos.z));
    left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "Chunk: %d %d %d", (int)std::floor(info.pos.x / 32.0f),
                  (int)std::floor(info.pos.y / 32.0f), (int)std::floor(info.pos.z / 32.0f));
    left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "Facing: %s", facingFromYaw(info.yaw)); left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "Chunks: %d loaded, %d rendered", info.chunksLoaded,
                  info.chunksRendered);
    left.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "LOD tiles: %d", info.lodTiles); left.emplace_back(buf);
    if (!info.timeText.empty()) left.emplace_back(info.timeText);

    float y = 4.0f;
    for (const std::string& s : left) {
        font.drawText(ui, s, 4.0f, y, scale, white);
        y += lineH;
    }

    // ---- Right column (right-aligned) ----
    std::vector<std::string> right;
    right.push_back(info.gpuName);
    std::snprintf(buf, sizeof(buf), "Display: %ux%u", info.displayW, info.displayH);
    right.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "GPU mem: %.1f MiB", toMiB(info.gpuMemBytes)); right.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "Alloc rate: %.2f MiB/s", info.allocRateMiBs); right.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "RAM: %.0f MiB", toMiB(info.ramBytes)); right.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "CPU: %.0f%%", info.cpuPercent); right.emplace_back(buf);

    y = 4.0f;
    for (const std::string& s : right) {
        float w = font.textWidth(s, scale);
        font.drawText(ui, s, static_cast<float>(screenW) - w - 4.0f, y, scale, white);
        y += lineH;
    }

    // ---- Targeted block panel (right side, below the system stats) ----
    if (info.hasTarget) {
        y += lineH; // gap

        std::snprintf(buf, sizeof(buf), "Targeted Block: %d, %d, %d", info.targetBlock.x,
                      info.targetBlock.y, info.targetBlock.z);
        std::string header = buf;
        float hw = font.textWidth(header, scale);
        float hx = static_cast<float>(screenW) - hw - 4.0f;
        font.drawText(ui, header, hx, y, scale, white);
        ui.quad(hx, y + font.lineHeight(scale) + 1.0f, hw, scale, white); // underline
        y += lineH;

        auto drawRight = [&](const std::string& s) {
            float w = font.textWidth(s, scale);
            font.drawText(ui, s, static_cast<float>(screenW) - w - 4.0f, y, scale, white);
            y += lineH;
        };
        drawRight(info.targetName);
        std::snprintf(buf, sizeof(buf), "face: %s", faceFromNormal(info.targetNormal));
        drawRight(buf);
        if (!info.targetTags.empty()) drawRight("[" + info.targetTags + "]");
    }
}

} // namespace mc
