#include "game/PlayerSkin.h"

#include <cstdio>
#include <filesystem>
#include <random>
#include <vector>

namespace mc {
namespace fs = std::filesystem;

namespace {
void scanFolder(const fs::path& dir, bool slim, std::vector<PlayerSkin>& out) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".png") {
            out.push_back({entry.path().string(), slim, entry.path().stem().string()});
        }
    }
}
} // namespace

PlayerSkin pickPlayerSkin(const std::string& playerDir, const std::string& preferred) {
    // The remembered skin from options.txt wins if its file still exists.
    if (!preferred.empty()) {
        std::vector<PlayerSkin> all;
        scanFolder(fs::path(playerDir) / "custom", false, all);
        for (PlayerSkin& s : all) {
            s.slim = s.name.find("slim") != std::string::npos ||
                     s.name.find("alex") != std::string::npos;
        }
        scanFolder(fs::path(playerDir) / "slim", true, all);
        scanFolder(fs::path(playerDir) / "wide", false, all);
        for (const PlayerSkin& s : all) {
            if (s.name == preferred) {
                std::fprintf(stderr, "[Skin] remembered skin '%s' (%s)\n", s.name.c_str(),
                             s.slim ? "slim/Alex" : "wide/Steve");
                return s;
            }
        }
    }

    // A custom skin overrides the random default.
    std::vector<PlayerSkin> custom;
    scanFolder(fs::path(playerDir) / "custom", false, custom);
    for (PlayerSkin& s : custom) {
        s.slim = s.name.find("slim") != std::string::npos || s.name.find("alex") != std::string::npos;
    }
    if (!custom.empty()) {
        const PlayerSkin& s = custom.front();
        std::fprintf(stderr, "[Skin] custom skin '%s' (%s)\n", s.name.c_str(), s.slim ? "slim/Alex" : "wide/Steve");
        return s;
    }

    std::vector<PlayerSkin> options;
    scanFolder(fs::path(playerDir) / "slim", true, options);
    scanFolder(fs::path(playerDir) / "wide", false, options);
    if (options.empty()) {
        std::fprintf(stderr, "[Skin] no skins found under %s\n", playerDir.c_str());
        return {};
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, options.size() - 1);
    PlayerSkin picked = options[dist(rng)];
    std::fprintf(stderr, "[Skin] picked '%s' (%s)\n", picked.name.c_str(),
                 picked.slim ? "slim/Alex" : "wide/Steve");
    return picked;
}

void patchSkinJointCaps(unsigned char* rgba, int w, int h) {
    if (!rgba || w != 64 || h != 64) return; // legacy/fallback textures: leave untouched

    // Each limb's seam ring: the two texel rows adjacent to the elbow/knee split,
    // across the full width of the limb's 4 side cells. Spanning the full 16-column
    // strip and skipping transparent texels makes the same ring work for wide AND
    // slim skins (slim arms leave their unused columns transparent).
    struct Ring {
        int u0, u1; // [u0, u1) columns
        int v0, v1; // the two rows: seam-1, seam
        int dstX;   // reserved dead pixel (dstX, 0) the cap faces sample
    };
    static constexpr Ring kRings[] = {
        {40, 56, 25, 26, 0}, // rightArm  (elbow)
        {32, 48, 57, 58, 1}, // leftArm   (elbow)
        {0, 16, 25, 26, 2},  // rightLeg  (knee)
        {16, 32, 57, 58, 3}, // leftLeg   (knee)
    };
    for (const Ring& r : kRings) {
        // Byte-average in gamma space — same as the LOD tile color averaging; plenty
        // for a 4x4-pixel cap that only shows when a joint bends.
        unsigned sum[3] = {0, 0, 0};
        unsigned n = 0;
        for (int v : {r.v0, r.v1}) {
            for (int u = r.u0; u < r.u1; ++u) {
                const unsigned char* p = rgba + (v * w + u) * 4;
                if (p[3] == 0) continue; // slim dead columns / bad texels
                sum[0] += p[0];
                sum[1] += p[1];
                sum[2] += p[2];
                ++n;
            }
        }
        unsigned char* dst = rgba + r.dstX * 4; // row 0: unused corner of the 64x64 layout
        if (n == 0) {
            dst[0] = dst[1] = dst[2] = 96; // fully transparent ring: neutral gray cap
        } else {
            dst[0] = static_cast<unsigned char>(sum[0] / n);
            dst[1] = static_cast<unsigned char>(sum[1] / n);
            dst[2] = static_cast<unsigned char>(sum[2] / n);
        }
        dst[3] = 255;
    }
}

} // namespace mc
