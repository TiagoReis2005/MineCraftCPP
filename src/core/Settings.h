#pragma once

#include <map>
#include <string>

namespace mc {

// User options (edited in the options menu), persisted as key:value lines in
// options.txt next to the exe. Loaded at startup, saved on Done and on exit.
struct Settings {
    float fovDeg = 70.0f;      // 30..110
    float sensitivity = 0.08f; // mouse look factor (menu shows it as a percentage)
    int renderDistance = 5;    // full-detail chunks, 2..64
    int lodDistance = 16;      // far low-detail terrain radius, chunks, 0(off)..64

    // Profile (persisted with the options; skin remembers the picked file's stem).
    std::string playerName = "Player";
    std::string skin;
    std::string language = "en_us"; // Lang code: assets/lang/<code>.json

    // Window (persists across launches; fullscreen also toggles in the options menu).
    int windowW = 1280, windowH = 720; // windowed client size
    bool fullscreen = false;

    // Every other options-menu value (volumes, graphics toggles, skin parts, chat/
    // accessibility knobs...) lives in one persisted map keyed "opt.<name>", so the menu
    // can grow without touching this struct. Cosmetic ones don't affect the engine yet;
    // they just remember their value. Sliders store 0..1, cycles store the option index.
    std::map<std::string, float> opt;
    float optGet(const std::string& key, float def) const {
        auto it = opt.find(key);
        return it != opt.end() ? it->second : def;
    }
    void optSet(const std::string& key, float v) { opt[key] = v; }

    void load(const std::string& path); // missing file/keys keep current values
    void save(const std::string& path) const;
};

} // namespace mc
