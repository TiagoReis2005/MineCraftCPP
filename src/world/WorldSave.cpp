#include "world/WorldSave.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace mc {
namespace fs = std::filesystem;
namespace {

constexpr uint32_t kChunkMagic = 0x4843434Du; // "MCCH"
constexpr uint32_t kChunkVersion = 1;

} // namespace

WorldSave::WorldSave(std::string dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / "chunks", ec);
}

std::string WorldSave::chunkPath(int cx, int cy, int cz) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/chunks/c_%d_%d_%d.bin", cx, cy, cz);
    return dir_ + buf;
}

bool WorldSave::loadChunk(int cx, int cy, int cz, uint32_t* out, size_t count) const {
    std::FILE* f = std::fopen(chunkPath(cx, cy, cz).c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0, version = 0;
    bool ok = std::fread(&magic, 4, 1, f) == 1 && std::fread(&version, 4, 1, f) == 1 &&
              magic == kChunkMagic && version == kChunkVersion;
    size_t filled = 0;
    while (ok && filled < count) {
        uint32_t raw = 0, run = 0;
        if (std::fread(&raw, 4, 1, f) != 1 || std::fread(&run, 4, 1, f) != 1) {
            ok = false;
            break;
        }
        if (run == 0 || filled + run > count) {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < run; ++i) out[filled++] = raw;
    }
    std::fclose(f);
    return ok && filled == count;
}

bool WorldSave::hasChunk(int cx, int cy, int cz) const {
    std::error_code ec;
    return fs::exists(chunkPath(cx, cy, cz), ec);
}

std::vector<glm::ivec3> WorldSave::listChunks() const {
    std::vector<glm::ivec3> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_ + "/chunks", ec)) {
        int x = 0, y = 0, z = 0;
        std::string name = e.path().filename().string();
        if (std::sscanf(name.c_str(), "c_%d_%d_%d.bin", &x, &y, &z) == 3) {
            out.push_back({x, y, z});
        }
    }
    return out;
}

void WorldSave::saveChunk(int cx, int cy, int cz, const uint32_t* data, size_t count) const {
    std::FILE* f = std::fopen(chunkPath(cx, cy, cz).c_str(), "wb");
    if (!f) return;
    std::fwrite(&kChunkMagic, 4, 1, f);
    std::fwrite(&kChunkVersion, 4, 1, f);
    size_t i = 0;
    while (i < count) {
        uint32_t raw = data[i];
        uint32_t run = 1;
        while (i + run < count && data[i + run] == raw) ++run;
        std::fwrite(&raw, 4, 1, f);
        std::fwrite(&run, 4, 1, f);
        i += run;
    }
    std::fclose(f);
}

bool WorldSave::loadLevel(Level& out) const {
    std::FILE* f = std::fopen((dir_ + "/level.txt").c_str(), "r");
    if (!f) return false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char str[192];
        float a = 0, b = 0, c = 0;
        int i = 0, n = 0;
        double d = 0;
        unsigned long long g = 0;
        if (std::sscanf(line, "name:%191[^\n]", str) == 1) out.name = str;
        else if (std::sscanf(line, "seed:%u", &out.seed) == 1) {}
        else if (std::sscanf(line, "time:%lf", &d) == 1) out.timeSeconds = d;
        else if (std::sscanf(line, "gametime:%llu", &g) == 1) out.gametime = g;
        else if (std::sscanf(line, "weather:%d,%f,%f", &i, &a, &b) == 3) {
            out.raining = i != 0;
            out.rainTimer = a;
            out.rainIntensity = b;
        }
        else if (std::sscanf(line, "mode:%d", &out.mode) == 1) {}
        else if (std::sscanf(line, "flying:%d", &i) == 1) out.flying = i != 0;
        else if (std::sscanf(line, "player:%f,%f,%f", &a, &b, &c) == 3) {
            out.playerPos = {a, b, c};
            out.hasPlayer = true;
        } else if (std::sscanf(line, "look:%f,%f", &a, &b) == 2) {
            out.yaw = a;
            out.pitch = b;
        } else if (std::sscanf(line, "selected:%d", &out.selected) == 1) {
        } else if (std::sscanf(line, "hotbar%d:%191[^,],%d", &i, str, &n) == 3) {
            if (i >= 0 && i < 9) out.hotbar[i] = {str, n};
        } else if (std::sscanf(line, "main%d:%191[^,],%d", &i, str, &n) == 3) {
            if (i >= 0 && i < 36) out.main[i] = {str, n};
        } else {
            int x = 0, y = 0, z = 0, dat = 0;
            float prog = 0;
            float px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0, age = 0;
            if (std::sscanf(line, "crack:%d,%d,%d,%191[^,],%d,%f", &x, &y, &z, str, &dat, &prog) == 6) {
                out.cracks.push_back({{x, y, z}, str, static_cast<uint16_t>(dat), prog});
            } else if (std::sscanf(line, "drop:%191[^,],%d,%f,%f,%f,%f,%f,%f,%f", str, &n, &px,
                                   &py, &pz, &vx, &vy, &vz, &age) == 9) {
                out.drops.push_back({{px, py, pz}, {vx, vy, vz}, str, n, age});
            }
        }
    }
    std::fclose(f);
    return true;
}

void WorldSave::saveLevel(const Level& l) const {
    std::FILE* f = std::fopen((dir_ + "/level.txt").c_str(), "w");
    if (!f) return;
    std::fprintf(f, "name:%s\nseed:%u\ntime:%.2f\nmode:%d\nflying:%d\n",
                 l.name.c_str(), l.seed, l.timeSeconds, l.mode, l.flying ? 1 : 0);
    std::fprintf(f, "gametime:%llu\nweather:%d,%.2f,%.3f\n",
                 static_cast<unsigned long long>(l.gametime), l.raining ? 1 : 0, l.rainTimer,
                 l.rainIntensity);
    if (l.hasPlayer) {
        std::fprintf(f, "player:%.3f,%.3f,%.3f\nlook:%.2f,%.2f\n",
                     l.playerPos.x, l.playerPos.y, l.playerPos.z, l.yaw, l.pitch);
    }
    std::fprintf(f, "selected:%d\n", l.selected);
    for (int i = 0; i < 9; ++i) {
        if (!l.hotbar[i].name.empty() && l.hotbar[i].count > 0) {
            std::fprintf(f, "hotbar%d:%s,%d\n", i, l.hotbar[i].name.c_str(), l.hotbar[i].count);
        }
    }
    for (int i = 0; i < 36; ++i) {
        if (!l.main[i].name.empty() && l.main[i].count > 0) {
            std::fprintf(f, "main%d:%s,%d\n", i, l.main[i].name.c_str(), l.main[i].count);
        }
    }
    for (const CrackLine& c : l.cracks) {
        std::fprintf(f, "crack:%d,%d,%d,%s,%d,%.3f\n", c.cell.x, c.cell.y, c.cell.z,
                     c.block.c_str(), c.data, c.progress);
    }
    for (const DropLine& d : l.drops) {
        std::fprintf(f, "drop:%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f\n", d.block.c_str(),
                     d.count, d.pos.x, d.pos.y, d.pos.z, d.vel.x, d.vel.y, d.vel.z, d.age);
    }
    std::fclose(f);
}

std::vector<WorldSave::Info> WorldSave::list(const std::string& root) {
    struct Entry {
        Info info;
        fs::file_time_type mtime;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    if (fs::exists(root, ec)) {
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory()) continue;
            fs::path levelPath = e.path() / "level.txt";
            if (!fs::exists(levelPath, ec)) continue;
            WorldSave save(e.path().string());
            Level l;
            if (!save.loadLevel(l)) continue;
            Info info;
            info.folder = e.path().filename().string();
            info.name = l.name;
            info.day = l.timeSeconds >= 0 ? static_cast<int>(l.timeSeconds / 1200.0) + 1 : 1;
            info.mode = l.mode;
            entries.push_back({info, fs::last_write_time(levelPath, ec)});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
    std::vector<Info> out;
    out.reserve(entries.size());
    for (Entry& e : entries) out.push_back(std::move(e.info));
    return out;
}

void WorldSave::remove(const std::string& root, const std::string& folder) {
    if (folder.empty()) return; // never let an empty name nuke the saves root
    std::error_code ec;
    fs::remove_all(fs::path(root) / folder, ec);
}

std::string WorldSave::makeFolder(const std::string& root, const std::string& name) {
    std::string base;
    for (char ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') base += ch;
        else if (ch == ' ') base += '_';
    }
    if (base.empty()) base = "World";
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string folder = base;
    for (int i = 2; fs::exists(fs::path(root) / folder, ec); ++i) {
        folder = base + "_" + std::to_string(i);
    }
    return folder;
}

} // namespace mc
