#include "game/Recipes.h"

#include "world/BlockRegistry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mc {

bool RecipeBook::resolve(const BlockRegistry& reg, const char* name, Ingredient& out) {
    if (name[0] == '#') {
        out.anyOf = reg.withTag(name + 1);
        return !out.anyOf.empty();
    }
    out.id = reg.byName(name);
    return out.id != BLOCK_AIR;
}

void RecipeBook::addShaped(const BlockRegistry& reg, std::initializer_list<const char*> rows,
                           const char* result, int count,
                           std::initializer_list<std::pair<char, const char*>> key) {
    Recipe r;
    r.result = reg.byName(result);
    if (r.result == BLOCK_AIR) return; // result block not registered yet: dormant
    r.count = count;
    r.h = static_cast<int>(rows.size());
    r.w = 0;
    for (const char* row : rows) r.w = std::max(r.w, static_cast<int>(std::strlen(row)));
    if (r.w > 3 || r.h > 3) return;

    int y = 0;
    for (const char* row : rows) {
        for (int x = 0; x < r.w; ++x) {
            char c = x < static_cast<int>(std::strlen(row)) ? row[x] : ' ';
            Ingredient ing;
            if (c != ' ') {
                const char* name = nullptr;
                for (const auto& [k, n] : key) {
                    if (k == c) {
                        name = n;
                        break;
                    }
                }
                if (!name || !resolve(reg, name, ing)) return; // dormant
            }
            r.cells[static_cast<size_t>(y * r.w + x)] = std::move(ing);
        }
        ++y;
    }
    recipes_.push_back(std::move(r));
}

void RecipeBook::addShapeless(const BlockRegistry& reg,
                              std::initializer_list<const char*> ingredients,
                              const char* result, int count) {
    Recipe r;
    r.shapeless = true;
    r.result = reg.byName(result);
    if (r.result == BLOCK_AIR) return;
    r.count = count;
    for (const char* name : ingredients) {
        Ingredient ing;
        if (!resolve(reg, name, ing)) return; // dormant until the block/tag exists
        r.loose.push_back(std::move(ing));
    }
    recipes_.push_back(std::move(r));
}

ItemStack RecipeBook::match(const BlockId* grid, int gw, int gh) const {
    // Bounding box of the filled cells (shapeless just collects them).
    int minX = gw, minY = gh, maxX = -1, maxY = -1;
    std::vector<BlockId> filled;
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            if (grid[y * gw + x] == BLOCK_AIR) continue;
            filled.push_back(grid[y * gw + x]);
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (filled.empty()) return {};
    int bw = maxX - minX + 1, bh = maxY - minY + 1;

    for (const Recipe& r : recipes_) {
        if (r.shapeless) {
            if (filled.size() != r.loose.size()) continue;
            // Two-pass greedy binding: exact-id ingredients claim their items first so
            // a tag ingredient never steals an item another slot needs exactly.
            std::vector<bool> used(r.loose.size(), false);
            std::vector<BlockId> pending;
            for (BlockId g : filled) {
                bool bound = false;
                for (size_t i = 0; i < r.loose.size(); ++i) {
                    if (!used[i] && r.loose[i].anyOf.empty() && r.loose[i].id == g) {
                        used[i] = true;
                        bound = true;
                        break;
                    }
                }
                if (!bound) pending.push_back(g);
            }
            bool ok = true;
            for (BlockId g : pending) {
                bool bound = false;
                for (size_t i = 0; i < r.loose.size(); ++i) {
                    if (!used[i] && r.loose[i].matches(g)) {
                        used[i] = true;
                        bound = true;
                        break;
                    }
                }
                if (!bound) {
                    ok = false;
                    break;
                }
            }
            if (ok) return {r.result, r.count}; // equal sizes: everything bound
            continue;
        }
        if (bw != r.w || bh != r.h || r.w > gw || r.h > gh) continue;
        // Direct and horizontally-mirrored comparison against the bounding box.
        bool direct = true, mirrored = true;
        for (int y = 0; y < r.h && (direct || mirrored); ++y) {
            for (int x = 0; x < r.w; ++x) {
                BlockId g = grid[(minY + y) * gw + (minX + x)];
                if (!r.cells[static_cast<size_t>(y * r.w + x)].matches(g)) direct = false;
                if (!r.cells[static_cast<size_t>(y * r.w + (r.w - 1 - x))].matches(g)) {
                    mirrored = false;
                }
            }
        }
        if (direct || mirrored) return {r.result, r.count};
    }
    return {};
}

void RecipeBook::registerDefaults(const BlockRegistry& reg) {
    // ---- Wood families, generated from tags: every "#logs" member gets its
    // log -> planks recipe, every "#planks" member its slab/stairs/door recipes
    // (results derived from the block's name). Tag a new wood and it just works.
    for (BlockId logId : reg.withTag("logs")) {
        const std::string& n = reg.block(BlockState(logId)).name(); // e.g. "oak_log"
        size_t pos = n.rfind("_log");
        if (pos == std::string::npos || pos + 4 != n.size()) continue;
        std::string planks = n.substr(0, pos) + "_planks";
        addShapeless(reg, {n.c_str()}, planks.c_str(), 4);
    }
    for (BlockId planksId : reg.withTag("planks")) {
        const std::string& n = reg.block(BlockState(planksId)).name(); // "oak_planks"
        size_t pos = n.rfind("_planks");
        if (pos == std::string::npos || pos + 7 != n.size()) continue;
        std::string base = n.substr(0, pos);
        std::string slab = base + "_slab";
        std::string stairs = base + "_stairs";
        std::string door = base + "_door";
        addShaped(reg, {"###"}, slab.c_str(), 6, {{'#', n.c_str()}});
        addShaped(reg, {"#  ", "## ", "###"}, stairs.c_str(), 4, {{'#', n.c_str()}});
        addShaped(reg, {"##", "##", "##"}, door.c_str(), 3, {{'#', n.c_str()}});
    }
    // Any planks (mixed woods allowed), straight off the tag — vanilla behavior.
    addShaped(reg, {"##", "##"}, "crafting_table", 1, {{'#', "#planks"}});
    addShaped(reg, {"###", "# #", "###"}, "chest", 1, {{'#', "#planks"}}); // dormant
    addShaped(reg, {"###"}, "bookshelf", 1, {{'#', "#planks"}}); // no books yet: simplified

    // ---- Stone families: slabs and stairs from the full block (irregular name pairs,
    // so this stays a data table; unknown blocks skip).
    const char* stones[][2] = {
        {"stone", "stone"},
        {"cobblestone", "cobblestone"},
        {"smooth_stone", "smooth_stone"},
        {"bricks", "brick"},
        {"stone_bricks", "stone_brick"},
        {"sandstone", "sandstone"},
        {"red_sandstone", "red_sandstone"},
        {"granite", "granite"},
        {"diorite", "diorite"},
        {"andesite", "andesite"},
        {"nether_bricks", "nether_brick"},
    };
    for (const auto& s : stones) {
        std::string slab = std::string(s[1]) + "_slab";
        std::string stairs = std::string(s[1]) + "_stairs";
        addShaped(reg, {"###"}, slab.c_str(), 6, {{'#', s[0]}});
        addShaped(reg, {"#  ", "## ", "###"}, stairs.c_str(), 4, {{'#', s[0]}});
    }

    // ---- 2x2 conversions.
    addShaped(reg, {"##", "##"}, "stone_bricks", 4, {{'#', "stone"}});
    addShaped(reg, {"##", "##"}, "polished_granite", 4, {{'#', "granite"}});
    addShaped(reg, {"##", "##"}, "polished_diorite", 4, {{'#', "diorite"}});
    addShaped(reg, {"##", "##"}, "polished_andesite", 4, {{'#', "andesite"}});
    addShaped(reg, {"##", "##"}, "sandstone", 1, {{'#', "sand"}});
    addShaped(reg, {"##", "##"}, "red_sandstone", 1, {{'#', "red_sand"}});

    // ---- Functional blocks (furnace stays dormant until the block is registered).
    addShaped(reg, {"###", "# #", "###"}, "furnace", 1, {{'#', "cobblestone"}});

    std::fprintf(stderr, "[Recipes] %zu recipes active (%zu planks, %zu logs, %zu fuels)\n",
                 recipes_.size(), reg.withTag("planks").size(), reg.withTag("logs").size(),
                 reg.withTag("fuel").size());
}

} // namespace mc
