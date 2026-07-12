#include "core/Lang.h"

#include "anim/Json.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace mc {
namespace fs = std::filesystem;

std::vector<Lang::Info> Lang::list(const std::string& langDir) {
    std::vector<Info> out;
    std::error_code ec;
    if (fs::exists(langDir, ec)) {
        for (const auto& e : fs::directory_iterator(langDir, ec)) {
            if (e.path().extension() != ".json") continue;
            Info info;
            info.code = e.path().stem().string();
            info.name = info.code;
            try {
                JsonValue j = parseJsonFile(e.path().string());
                const JsonValue* name = j.find("language.name");
                const JsonValue* region = j.find("language.region");
                if (name && name->type == JsonValue::Type::String) {
                    info.name = name->string;
                    if (region && region->type == JsonValue::Type::String) {
                        info.name += " (" + region->string + ")";
                    }
                }
            } catch (const std::exception&) {}
            out.push_back(std::move(info));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Info& a, const Info& b) { return a.code < b.code; });
    return out;
}

void Lang::load(const std::string& langDir, const std::string& code) {
    map_.clear();
    code_ = code;
    std::string path = langDir + "/" + code + ".json";
    try {
        JsonValue j = parseJsonFile(path);
        if (j.type == JsonValue::Type::Object) {
            for (const auto& [key, value] : j.object) {
                if (value.type == JsonValue::Type::String) map_[key] = value.string;
            }
        }
        std::fprintf(stderr, "[Lang] %s: %zu strings\n", code.c_str(), map_.size());
    } catch (const std::exception&) {
        // No file: every tr() shows its built-in English fallback.
        std::fprintf(stderr, "[Lang] no %s.json (assets/lang) - built-in English\n",
                     code.c_str());
    }
}

std::string Lang::displayName() const {
    auto it = map_.find("language.name");
    if (it == map_.end()) return code_;
    auto region = map_.find("language.region");
    return region != map_.end() ? it->second + " (" + region->second + ")" : it->second;
}

} // namespace mc
