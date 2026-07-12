#include "core/Settings.h"

#include <algorithm>
#include <cstdio>

namespace mc {

void Settings::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f) {
        char line[192];
        while (std::fgets(line, sizeof(line), f)) {
            float v = 0.0f;
            char str[128];
            if (std::sscanf(line, "fov:%f", &v) == 1) fovDeg = v;
            else if (std::sscanf(line, "sensitivity:%f", &v) == 1) sensitivity = v;
            else if (std::sscanf(line, "renderDistance:%f", &v) == 1) renderDistance = static_cast<int>(v);
            else if (std::sscanf(line, "lodDistance:%f", &v) == 1) lodDistance = static_cast<int>(v);
            else if (std::sscanf(line, "name:%127[^\n]", str) == 1) playerName = str;
            else if (std::sscanf(line, "skin:%127[^\n]", str) == 1) skin = str;
            else if (std::sscanf(line, "language:%127[^\n]", str) == 1) language = str;
            else if (std::sscanf(line, "window:%dx%d", &windowW, &windowH) == 2) {}
            else if (std::sscanf(line, "fullscreen:%f", &v) == 1) fullscreen = v != 0.0f;
            else if (std::sscanf(line, "opt.%127[^:]:%f", str, &v) == 2) opt[str] = v;
        }
        std::fclose(f);
    }
    fovDeg = std::clamp(fovDeg, 30.0f, 110.0f);
    sensitivity = std::clamp(sensitivity, 0.008f, 0.24f);
    renderDistance = std::clamp(renderDistance, 2, 64);
    lodDistance = std::clamp(lodDistance, 0, 64);
    windowW = std::clamp(windowW, 640, 7680);
    windowH = std::clamp(windowH, 360, 4320);
    // Migration: language used to store a display string ("English (US)"); it is a
    // lang CODE now (en_us). Anything that can't be a file stem resets.
    if (language.find(' ') != std::string::npos || language.empty()) language = "en_us";
}

void Settings::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "fov:%.0f\nsensitivity:%.4f\nrenderDistance:%d\nlodDistance:%d\n",
                 fovDeg, sensitivity, renderDistance, lodDistance);
    std::fprintf(f, "name:%s\nskin:%s\nlanguage:%s\n",
                 playerName.c_str(), skin.c_str(), language.c_str());
    std::fprintf(f, "window:%dx%d\nfullscreen:%d\n", windowW, windowH, fullscreen ? 1 : 0);
    for (const auto& [key, value] : opt) {
        std::fprintf(f, "opt.%s:%.4f\n", key.c_str(), value);
    }
    std::fclose(f);
}

} // namespace mc
