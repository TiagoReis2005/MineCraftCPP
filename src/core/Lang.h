#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace mc {

// Localization: vanilla-format language files (assets/lang/<code>.json, flat
// key->string objects like "menu.singleplayer": "Singleplayer"). UI code asks
// tr(key, fallback) with the English string as the built-in fallback, so the game is
// fully playable with zero lang files and translates as soon as one is dropped in.
// list() feeds the future language-selection screen.
class Lang {
public:
    struct Info {
        std::string code; // file stem, e.g. "en_us", "pt_pt"
        std::string name; // "language.name" (+ region) from the file, else the code
    };

    // Every language file under langDir, sorted by code.
    static std::vector<Info> list(const std::string& langDir);

    // Load <langDir>/<code>.json; missing file leaves the table empty (fallbacks show).
    void load(const std::string& langDir, const std::string& code);

    const std::string& code() const { return code_; }
    // Display name of the loaded language ("language.name" key), else its code.
    std::string displayName() const;

    // Translate a key; the fallback is the source-English string.
    const std::string& tr(const std::string& key, const std::string& fallback) const {
        auto it = map_.find(key);
        return it != map_.end() ? it->second : fallback;
    }

private:
    std::unordered_map<std::string, std::string> map_;
    std::string code_ = "en_us";
};

} // namespace mc
