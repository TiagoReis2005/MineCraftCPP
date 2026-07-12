#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

// One save folder under saves/<folder>/:
//   level.txt    - world meta + player state, text key:value (block/item ids stored by
//                  NAME so saves survive registry changes)
//   chunks/*.bin - RLE-compressed raw BlockStates, one file per edited 32^3 chunk
// Only player-edited chunks are stored; everything else regenerates from the seed.
// Chunk IO uses independent file handles per call, so worker threads can overlay saved
// chunks right after generating them.
class WorldSave {
public:
    struct ItemLine {
        std::string name; // block name ("" = empty slot)
        int count = 0;
    };
    struct CrackLine {
        glm::ivec3 cell{0};
        std::string block;
        uint16_t data = 0;
        float progress = 0.0f;
    };
    struct DropLine { // an item lying in the world
        glm::vec3 pos{0.0f};
        glm::vec3 vel{0.0f};
        std::string block;
        int count = 0;
        float age = 0.0f; // keeps the despawn clock honest across loads
    };

    struct Level {
        std::string name = "New World";
        uint32_t seed = 1337u;
        double timeSeconds = -1.0; // <0 = fresh world (mid-morning default)
        uint64_t gametime = 0;     // total ticks the world has run (drives clouds/redstone)
        bool raining = false;      // weather state (persisted so reload resumes it)
        float rainTimer = 0.0f;
        float rainIntensity = 0.0f;
        int mode = 0;              // GameMode as int
        bool flying = false;
        bool hasPlayer = false;    // false = fresh world: run the ground-spawn scan
        glm::vec3 playerPos{0.5f, 100.0f, 0.5f};
        float yaw = -90.0f, pitch = -20.0f;
        int selected = 0;
        ItemLine hotbar[9];
        ItemLine main[36];
        std::vector<CrackLine> cracks; // partially-broken blocks (break progress)
        std::vector<DropLine> drops;   // items lying on the ground
    };

    explicit WorldSave(std::string dir); // creates <dir>/chunks/
    const std::string& dir() const { return dir_; }

    bool loadLevel(Level& out) const; // false if level.txt is missing
    void saveLevel(const Level& l) const;

    // Chunk block data (count = Chunk::kSize^3 raw states). load returns false when the
    // chunk has no file (never edited); out is left untouched then.
    bool loadChunk(int cx, int cy, int cz, uint32_t* out, size_t count) const;
    void saveChunk(int cx, int cy, int cz, const uint32_t* data, size_t count) const;
    // True if this chunk has a saved (player-edited) file — a cheap existence check the
    // far LOD uses to decide whether a column needs real block data or pure worldgen.
    bool hasChunk(int cx, int cy, int cz) const;
    // Coordinates of every saved chunk file (scanned once at world load; the world uses
    // this to know which guaranteed-air sky cubes still need creating for saved edits).
    std::vector<glm::ivec3> listChunks() const;

    // Valid saves under root (folders containing level.txt), most recently played first.
    struct Info {
        std::string folder;
        std::string name;
        int day = 1;
        int mode = 0;
    };
    static std::vector<Info> list(const std::string& root);
    static void remove(const std::string& root, const std::string& folder);
    // Filesystem-safe unique folder for a new world name ("My World" -> "My_World"...).
    static std::string makeFolder(const std::string& root, const std::string& name);

private:
    std::string dir_;
    std::string chunkPath(int cx, int cy, int cz) const;
};

} // namespace mc
