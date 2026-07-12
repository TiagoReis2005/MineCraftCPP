#include "world/BlockRegistry.h"

#include "world/Blocks.h"

#include <unordered_set>

namespace mc {

void BlockRegistry::addImpl(std::unique_ptr<Block> block) {
    block->id = static_cast<BlockId>(blocks_.size());
    for (const std::string& t : block->props().tags) tags_[t].push_back(block->id);
    blocks_.push_back(std::move(block));
}

void BlockRegistry::registerDefaults() {
    blocks_.clear();
    slabCombos_.clear();
    tags_.clear();

    using P = Block::Properties;

    // ---- Register blocks here, one statement each. That's the only place. ----
    add(std::make_unique<Block>("air", P{}.strengthIs(0).notSolid().transparent().canReplace()));

    grass = add(std::make_unique<Block>("grass", P{}.strengthIs(0.6f).prefers(ToolType::Shovel)
                                                     .soundIs("grass").inTab(kTabNature)))
                .faces("grass_top", "dirt", "grass_side").id;
    dirt = add(std::make_unique<Block>("dirt", P{}.strengthIs(0.5f).prefers(ToolType::Shovel)
                                                   .soundIs("gravel").inTab(kTabNature)))
               .allFaces("dirt").id;
    stone = add(std::make_unique<Block>("stone", P{}.strengthIs(1.5f).needs(ToolType::Pickaxe)))
                .allFaces("stone").id;
    add(std::make_unique<Block>("cobblestone", P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)))
        .allFaces("cobblestone");
    sand = add(std::make_unique<Block>("sand", P{}.strengthIs(0.5f).prefers(ToolType::Shovel)
                                                   .soundIs("sand").inTab(kTabNature)))
               .allFaces("sand").id;
    add(std::make_unique<PillarBlock>("oak_log", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                     .soundIs("wood").inTab(kTabNature)
                                                     .tag("logs").burns(15.0f)))
        .column("oak_log_top", "oak_log");
    add(std::make_unique<LeavesBlock>("leaves", P{}.strengthIs(0.2f).prefers(ToolType::Shears)
                                                    .transparent().soundIs("grass")
                                                    .inTab(kTabNature),
                                      "oak_sapling"))
        .allFaces("oak_leaves");
    add(std::make_unique<GlassBlock>("glass", P{}.strengthIs(0.3f).transparent().cullsSelf()
                                                  .soundIs("glass")))
        .allFaces("glass");

    // Slabs (one item each; top/bottom is placement state, doubles are hidden combos).
    BlockId stoneSlab =
        add(std::make_unique<SlabBlock>("stone_slab", P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)
                                                          .transparent()))
            .allFaces("stone").id;
    BlockId cobbleSlab =
        add(std::make_unique<SlabBlock>("cobblestone_slab",
                                        P{}.strengthIs(2.0f).needs(ToolType::Pickaxe).transparent()))
            .allFaces("cobblestone").id;

    // Stairs.
    add(std::make_unique<StairBlock>("stone_stairs", P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)
                                                         .transparent()))
        .allFaces("stone");
    add(std::make_unique<StairBlock>("cobblestone_stairs",
                                     P{}.strengthIs(2.0f).needs(ToolType::Pickaxe).transparent()))
        .allFaces("cobblestone");
    add(std::make_unique<StairBlock>("oak_stairs", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                       .transparent().soundIs("wood")))
        .allFaces("oak_planks");

    // Fence family + door + plate (wood set).
    add(std::make_unique<FenceBlock>("oak_fence", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                      .transparent().soundIs("wood")
                                                      .inTab(kTabFunctional)))
        .allFaces("oak_planks");
    add(std::make_unique<FenceGateBlock>("oak_fence_gate",
                                         P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                             .transparent().soundIs("wood")
                                             .inTab(kTabFunctional)))
        .allFaces("oak_planks");
    add(std::make_unique<DoorBlock>("oak_door", P{}.strengthIs(3.0f).prefers(ToolType::Axe)
                                                    .transparent().soundIs("wood")
                                                    .inTab(kTabFunctional)
                                                    .needsSupport(BlockSide::Bottom),
                                    "oak_door_bottom", "oak_door_top"));
    add(std::make_unique<PressurePlateBlock>("stone_pressure_plate",
                                             P{}.strengthIs(0.5f).needs(ToolType::Pickaxe)
                                                 .notSolid().transparent()
                                                 .inTab(kTabFunctional)
                                                 .needsSupport(BlockSide::Bottom)))
        .allFaces("stone");

    // Snow layers.
    add(std::make_unique<LayerBlock>("snow_layer", P{}.strengthIs(0.1f).prefers(ToolType::Shovel)
                                                       .transparent().soundIs("snow")
                                                       .inTab(kTabNature)
                                                       .needsSupport(BlockSide::Bottom)))
        .allFaces("snow");

    // ---- Bulk families: small helpers keep it to one line per block. ----
    // Pickaxe-required cube (stone/ore/mineral family).
    auto hardCube = [&](const char* n, float s, const char* tx, uint8_t tab) -> Block& {
        return add(std::make_unique<Block>(n, P{}.strengthIs(s).needs(ToolType::Pickaxe).inTab(tab)))
            .allFaces(tx);
    };
    // Tool-preferred soft cube (dirt/sand/wood family; never withholds drops).
    auto softCube = [&](const char* n, float s, ToolType t, const char* snd, const char* tx,
                        uint8_t tab) -> Block& {
        return add(std::make_unique<Block>(n, P{}.strengthIs(s).prefers(t).soundIs(snd).inTab(tab)))
            .allFaces(tx);
    };
    // Planks: tagged for recipe families ("#planks" ingredients, slab/stairs/door
    // derivation) and as furnace fuel.
    auto planksCube = [&](const char* n, const char* tx) {
        add(std::make_unique<Block>(n, P{}.strengthIs(2.0f).prefers(ToolType::Axe).soundIs("wood")
                                          .inTab(kTabBuilding).tag("planks").burns(15.0f)))
            .allFaces(tx);
    };
    auto woodSlab = [&](const char* n, const char* tx) -> BlockId {
        return add(std::make_unique<SlabBlock>(n, P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                      .transparent().soundIs("wood")
                                                      .burns(7.5f)))
            .allFaces(tx).id;
    };
    auto stoneSlabF = [&](const char* n, const char* tx) -> BlockId {
        return add(std::make_unique<SlabBlock>(n, P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)
                                                      .transparent()))
            .allFaces(tx).id;
    };
    auto woodStairs = [&](const char* n, const char* tx) {
        add(std::make_unique<StairBlock>(n, P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                .transparent().soundIs("wood").burns(15.0f)))
            .allFaces(tx);
    };
    auto stoneStairs = [&](const char* n, const char* tx) {
        add(std::make_unique<StairBlock>(n, P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)
                                                .transparent()))
            .allFaces(tx);
    };
    auto woodSet = [&](const char* wood) { // fence + gate + door for a plank texture
        std::string w = wood;
        add(std::make_unique<FenceBlock>(w + "_fence",
                                         P{}.strengthIs(2.0f).prefers(ToolType::Axe).transparent()
                                             .soundIs("wood").inTab(kTabFunctional)
                                             .burns(15.0f)))
            .allFaces(w + "_planks");
        add(std::make_unique<FenceGateBlock>(w + "_fence_gate",
                                             P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                 .transparent().soundIs("wood")
                                                 .inTab(kTabFunctional).burns(15.0f)))
            .allFaces(w + "_planks");
        add(std::make_unique<DoorBlock>(w + "_door",
                                        P{}.strengthIs(3.0f).prefers(ToolType::Axe).transparent()
                                            .soundIs("wood").inTab(kTabFunctional)
                                            .needsSupport(BlockSide::Bottom),
                                        w + "_door_bottom", w + "_door_top"));
    };

    // Building blocks.
    hardCube("bricks", 2.0f, "bricks", kTabBuilding);
    hardCube("stone_bricks", 1.5f, "stone_bricks", kTabBuilding);
    hardCube("mossy_stone_bricks", 1.5f, "mossy_stone_bricks", kTabBuilding);
    hardCube("cracked_stone_bricks", 1.5f, "cracked_stone_bricks", kTabBuilding);
    hardCube("chiseled_stone_bricks", 1.5f, "chiseled_stone_bricks", kTabBuilding);
    hardCube("smooth_stone", 2.0f, "smooth_stone", kTabBuilding);
    hardCube("mossy_cobblestone", 2.0f, "mossy_cobblestone", kTabBuilding);
    add(std::make_unique<Block>("sandstone", P{}.strengthIs(0.8f).needs(ToolType::Pickaxe)
                                                 .inTab(kTabBuilding)))
        .faces("sandstone_top", "sandstone_bottom", "sandstone");
    hardCube("smooth_sandstone", 2.0f, "sandstone_top", kTabBuilding);
    add(std::make_unique<PillarBlock>("quartz_block", P{}.strengthIs(0.8f).needs(ToolType::Pickaxe)
                                                          .inTab(kTabBuilding)))
        .column("quartz_block_top", "quartz_block_side");
    hardCube("coal_block", 5.0f, "coal_block", kTabBuilding);
    hardCube("iron_block", 5.0f, "iron_block", kTabBuilding);
    hardCube("gold_block", 3.0f, "gold_block", kTabBuilding);
    hardCube("diamond_block", 5.0f, "diamond_block", kTabBuilding);
    hardCube("emerald_block", 5.0f, "emerald_block", kTabBuilding);
    hardCube("redstone_block", 5.0f, "redstone_block", kTabBuilding);
    hardCube("lapis_block", 3.0f, "lapis_block", kTabBuilding);
    hardCube("obsidian", 50.0f, "obsidian", kTabBuilding);
    add(std::make_unique<Block>("bedrock", P{}.strengthIs(-1.0f).inTab(kTabBuilding)))
        .allFaces("bedrock");
    planksCube("oak_planks", "oak_planks");
    planksCube("spruce_planks", "spruce_planks");
    planksCube("birch_planks", "birch_planks");
    add(std::make_unique<Block>("bookshelf", P{}.strengthIs(1.5f).prefers(ToolType::Axe)
                                                 .soundIs("wood").inTab(kTabBuilding)
                                                 .burns(15.0f)))
        .column("oak_planks", "bookshelf");
    softCube("white_wool", 0.8f, ToolType::Shears, "wool", "white_wool", kTabBuilding);
    softCube("black_wool", 0.8f, ToolType::Shears, "wool", "black_wool", kTabBuilding);
    softCube("red_wool", 0.8f, ToolType::Shears, "wool", "red_wool", kTabBuilding);
    softCube("blue_wool", 0.8f, ToolType::Shears, "wool", "blue_wool", kTabBuilding);
    softCube("green_wool", 0.8f, ToolType::Shears, "wool", "green_wool", kTabBuilding);
    softCube("yellow_wool", 0.8f, ToolType::Shears, "wool", "yellow_wool", kTabBuilding);
    softCube("orange_wool", 0.8f, ToolType::Shears, "wool", "orange_wool", kTabBuilding);
    softCube("purple_wool", 0.8f, ToolType::Shears, "wool", "purple_wool", kTabBuilding);

    // Nature.
    hardCube("coal_ore", 3.0f, "coal_ore", kTabNature);
    hardCube("iron_ore", 3.0f, "iron_ore", kTabNature);
    hardCube("copper_ore", 3.0f, "copper_ore", kTabNature);
    hardCube("gold_ore", 3.0f, "gold_ore", kTabNature);
    hardCube("redstone_ore", 3.0f, "redstone_ore", kTabNature);
    hardCube("lapis_ore", 3.0f, "lapis_ore", kTabNature);
    hardCube("diamond_ore", 3.0f, "diamond_ore", kTabNature);
    hardCube("emerald_ore", 3.0f, "emerald_ore", kTabNature);
    softCube("gravel", 0.6f, ToolType::Shovel, "gravel", "gravel", kTabNature);
    softCube("clay", 0.6f, ToolType::Shovel, "gravel", "clay", kTabNature);
    add(std::make_unique<PillarBlock>("spruce_log", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                        .soundIs("wood").inTab(kTabNature)
                                                        .tag("logs").burns(15.0f)))
        .column("spruce_log_top", "spruce_log");
    add(std::make_unique<PillarBlock>("birch_log", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                                       .soundIs("wood").inTab(kTabNature)
                                                       .tag("logs").burns(15.0f)))
        .column("birch_log_top", "birch_log");
    add(std::make_unique<PillarBlock>("pumpkin", P{}.strengthIs(1.0f).prefers(ToolType::Axe)
                                                     .soundIs("wood").inTab(kTabNature)))
        .column("pumpkin_top", "pumpkin_side");
    add(std::make_unique<PillarBlock>("melon", P{}.strengthIs(1.0f).prefers(ToolType::Axe)
                                                   .soundIs("wood").inTab(kTabNature)))
        .column("melon_top", "melon_side");
    softCube("snow_block", 0.2f, ToolType::Shovel, "snow", "snow", kTabNature);
    hardCube("ice", 0.5f, "ice", kTabNature);
    hardCube("packed_ice", 0.5f, "packed_ice", kTabNature);
    softCube("sponge", 0.6f, ToolType::None, "grass", "sponge", kTabNature);
    hardCube("netherrack", 0.4f, "netherrack", kTabNature);
    softCube("soul_sand", 0.5f, ToolType::Shovel, "sand", "soul_sand", kTabNature);
    hardCube("end_stone", 3.0f, "end_stone", kTabNature);

    // Functional.
    add(std::make_unique<Block>("crafting_table", P{}.strengthIs(2.5f).prefers(ToolType::Axe)
                                                      .soundIs("wood").inTab(kTabFunctional)
                                                      .opens(kContainerCrafting).burns(15.0f)))
        .faces("crafting_table_top", "oak_planks", "crafting_table_side");
    add(std::make_unique<Block>("glowstone", P{}.strengthIs(0.3f).light(15).inTab(kTabFunctional)))
        .allFaces("glowstone");
    add(std::make_unique<Block>("tnt", P{}.strengthIs(0.0f).inTab(kTabFunctional)))
        .faces("tnt_top", "tnt_bottom", "tnt_side");
    woodSet("spruce");
    woodSet("birch");
    add(std::make_unique<DoorBlock>("iron_door", P{}.strengthIs(5.0f).needs(ToolType::Pickaxe)
                                                     .transparent().inTab(kTabFunctional)
                                                     .needsSupport(BlockSide::Bottom),
                                    "iron_door_bottom", "iron_door_top"));
    add(std::make_unique<PressurePlateBlock>("oak_pressure_plate",
                                             P{}.strengthIs(0.5f).prefers(ToolType::Axe)
                                                 .notSolid().transparent().soundIs("wood")
                                                 .inTab(kTabFunctional)
                                                 .needsSupport(BlockSide::Bottom)))
        .allFaces("oak_planks");

    // Batch 2: stone variety.
    hardCube("granite", 1.5f, "granite", kTabNature);
    hardCube("polished_granite", 1.5f, "polished_granite", kTabBuilding);
    hardCube("diorite", 1.5f, "diorite", kTabNature);
    hardCube("polished_diorite", 1.5f, "polished_diorite", kTabBuilding);
    hardCube("andesite", 1.5f, "andesite", kTabNature);
    hardCube("polished_andesite", 1.5f, "polished_andesite", kTabBuilding);
    add(std::make_unique<PillarBlock>("deepslate", P{}.strengthIs(3.0f).needs(ToolType::Pickaxe)
                                                       .inTab(kTabNature)))
        .column("deepslate_top", "deepslate");
    hardCube("cobbled_deepslate", 3.5f, "cobbled_deepslate", kTabBuilding);
    hardCube("deepslate_bricks", 3.5f, "deepslate_bricks", kTabBuilding);
    hardCube("deepslate_tiles", 3.5f, "deepslate_tiles", kTabBuilding);
    hardCube("polished_deepslate", 3.5f, "polished_deepslate", kTabBuilding);
    hardCube("tuff", 1.5f, "tuff", kTabNature);
    hardCube("calcite", 0.8f, "calcite", kTabNature);
    hardCube("dripstone_block", 1.5f, "dripstone_block", kTabNature);
    hardCube("amethyst_block", 1.5f, "amethyst_block", kTabNature);
    softCube("mud", 0.5f, ToolType::Shovel, "gravel", "mud", kTabNature);
    hardCube("packed_mud", 1.0f, "packed_mud", kTabBuilding);
    hardCube("mud_bricks", 1.5f, "mud_bricks", kTabBuilding);
    hardCube("nether_bricks", 2.0f, "nether_bricks", kTabBuilding);
    hardCube("red_nether_bricks", 2.0f, "red_nether_bricks", kTabBuilding);
    hardCube("quartz_bricks", 0.8f, "quartz_bricks", kTabBuilding);
    hardCube("end_stone_bricks", 3.0f, "end_stone_bricks", kTabBuilding);
    hardCube("purpur_block", 1.5f, "purpur_block", kTabBuilding);
    add(std::make_unique<PillarBlock>("purpur_pillar", P{}.strengthIs(1.5f).needs(ToolType::Pickaxe)
                                                           .inTab(kTabBuilding)))
        .column("purpur_pillar_top", "purpur_pillar");
    add(std::make_unique<PillarBlock>("basalt", P{}.strengthIs(1.25f).needs(ToolType::Pickaxe)
                                                    .inTab(kTabNature)))
        .column("basalt_top", "basalt_side");
    add(std::make_unique<PillarBlock>("polished_basalt", P{}.strengthIs(1.25f)
                                                             .needs(ToolType::Pickaxe)
                                                             .inTab(kTabBuilding)))
        .column("polished_basalt_top", "polished_basalt_side");
    add(std::make_unique<PillarBlock>("blackstone", P{}.strengthIs(1.5f).needs(ToolType::Pickaxe)
                                                        .inTab(kTabNature)))
        .column("blackstone_top", "blackstone");
    hardCube("polished_blackstone", 2.0f, "polished_blackstone", kTabBuilding);
    hardCube("polished_blackstone_bricks", 1.5f, "polished_blackstone_bricks", kTabBuilding);
    softCube("red_sand", 0.5f, ToolType::Shovel, "sand", "red_sand", kTabNature);
    add(std::make_unique<Block>("red_sandstone", P{}.strengthIs(0.8f).needs(ToolType::Pickaxe)
                                                     .inTab(kTabBuilding)))
        .faces("red_sandstone_top", "red_sandstone_bottom", "red_sandstone");
    hardCube("smooth_red_sandstone", 2.0f, "red_sandstone_top", kTabBuilding);
    hardCube("copper_block", 3.0f, "copper_block", kTabBuilding);
    hardCube("cut_copper", 3.0f, "cut_copper", kTabBuilding);
    hardCube("raw_iron_block", 5.0f, "raw_iron_block", kTabBuilding);
    hardCube("raw_copper_block", 5.0f, "raw_copper_block", kTabBuilding);
    hardCube("raw_gold_block", 5.0f, "raw_gold_block", kTabBuilding);

    // Batch 2: terracotta + concrete + the remaining wools.
    hardCube("terracotta", 1.25f, "terracotta", kTabBuilding);
    for (const char* c : {"white", "red", "orange", "yellow", "brown"}) {
        hardCube((std::string(c) + "_terracotta").c_str(), 1.25f,
                 (std::string(c) + "_terracotta").c_str(), kTabBuilding);
    }
    for (const char* c : {"white", "red", "blue", "green", "yellow", "black", "orange", "purple"}) {
        hardCube((std::string(c) + "_concrete").c_str(), 1.8f,
                 (std::string(c) + "_concrete").c_str(), kTabBuilding);
    }
    for (const char* c : {"light_blue", "lime", "pink", "gray", "light_gray", "cyan", "magenta",
                          "brown"}) {
        softCube((std::string(c) + "_wool").c_str(), 0.8f, ToolType::Shears, "wool",
                 (std::string(c) + "_wool").c_str(), kTabBuilding);
    }

    // Batch 2: wood + misc.
    planksCube("dark_oak_planks", "dark_oak_planks");
    planksCube("acacia_planks", "acacia_planks");
    planksCube("jungle_planks", "jungle_planks");
    planksCube("cherry_planks", "cherry_planks");
    for (const char* w : {"dark_oak", "acacia", "jungle"}) {
        add(std::make_unique<PillarBlock>(std::string(w) + "_log",
                                          P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                              .soundIs("wood").inTab(kTabNature)))
            .column(std::string(w) + "_log_top", std::string(w) + "_log");
    }
    woodSet("dark_oak");
    woodSet("acacia");
    woodSet("jungle");
    add(std::make_unique<PillarBlock>("hay_block", P{}.strengthIs(0.5f).soundIs("grass")
                                                       .inTab(kTabNature)))
        .column("hay_block_top", "hay_block_side");
    softCube("honeycomb_block", 0.6f, ToolType::None, "grass", "honeycomb_block", kTabNature);
    add(std::make_unique<Block>("note_block", P{}.strengthIs(0.8f).prefers(ToolType::Axe)
                                                  .soundIs("wood").inTab(kTabFunctional)))
        .allFaces("note_block");
    add(std::make_unique<Block>("jukebox", P{}.strengthIs(2.0f).prefers(ToolType::Axe)
                                               .soundIs("wood").inTab(kTabFunctional)))
        .column("jukebox_top", "jukebox_side");
    add(std::make_unique<Block>("target", P{}.strengthIs(0.5f).soundIs("grass")
                                              .inTab(kTabFunctional)))
        .column("target_top", "target_side");

    // More slabs and stairs (Building).
    BlockId oakSlab = woodSlab("oak_slab", "oak_planks");
    BlockId spruceSlab = woodSlab("spruce_slab", "spruce_planks");
    BlockId birchSlab = woodSlab("birch_slab", "birch_planks");
    BlockId brickSlab = stoneSlabF("brick_slab", "bricks");
    BlockId stoneBrickSlab = stoneSlabF("stone_brick_slab", "stone_bricks");
    BlockId smoothStoneSlab = stoneSlabF("smooth_stone_slab", "smooth_stone");
    BlockId sandstoneSlab = stoneSlabF("sandstone_slab", "sandstone_top");
    BlockId graniteSlab = stoneSlabF("granite_slab", "granite");
    BlockId dioriteSlab = stoneSlabF("diorite_slab", "diorite");
    BlockId andesiteSlab = stoneSlabF("andesite_slab", "andesite");
    BlockId netherBrickSlab = stoneSlabF("nether_brick_slab", "nether_bricks");
    BlockId redSandstoneSlab = stoneSlabF("red_sandstone_slab", "red_sandstone_top");
    BlockId darkOakSlab = woodSlab("dark_oak_slab", "dark_oak_planks");
    BlockId acaciaSlab = woodSlab("acacia_slab", "acacia_planks");
    BlockId jungleSlab = woodSlab("jungle_slab", "jungle_planks");
    stoneStairs("brick_stairs", "bricks");
    stoneStairs("stone_brick_stairs", "stone_bricks");
    stoneStairs("sandstone_stairs", "sandstone_top");
    woodStairs("spruce_stairs", "spruce_planks");
    woodStairs("birch_stairs", "birch_planks");
    stoneStairs("granite_stairs", "granite");
    stoneStairs("diorite_stairs", "diorite");
    stoneStairs("andesite_stairs", "andesite");
    stoneStairs("nether_brick_stairs", "nether_bricks");
    stoneStairs("red_sandstone_stairs", "red_sandstone_top");
    woodStairs("dark_oak_stairs", "dark_oak_planks");
    woodStairs("acacia_stairs", "acacia_planks");
    woodStairs("jungle_stairs", "jungle_planks");

    // Hidden double-slab combos for every ordered pair of slab materials (mixed OK).
    std::vector<BlockId> slabs{stoneSlab,       cobbleSlab,       oakSlab,
                               spruceSlab,      birchSlab,        brickSlab,
                               stoneBrickSlab,  smoothStoneSlab,  sandstoneSlab,
                               graniteSlab,     dioriteSlab,      andesiteSlab,
                               netherBrickSlab, redSandstoneSlab, darkOakSlab,
                               acaciaSlab,      jungleSlab};
    for (BlockId b : slabs) {
        for (BlockId t : slabs) {
            std::string name = block(b).name() + "+" + block(t).name();
            BlockId comboId =
                add(std::make_unique<DoubleSlabBlock>(std::move(name),
                                                      P{}.strengthIs(2.0f).needs(ToolType::Pickaxe)
                                                          .hidden(),
                                                      b, t))
                    .id;
            slabCombos_[comboKey(b, t)] = comboId;
        }
    }
}

std::vector<std::string> BlockRegistry::textureNames() const {
    std::unordered_set<std::string> unique;
    std::vector<std::string> names;
    for (const auto& b : blocks_) {
        names.clear();
        b->gatherTextureNames(names);
        for (auto& n : names) unique.insert(std::move(n));
    }
    return std::vector<std::string>(unique.begin(), unique.end());
}

void BlockRegistry::resolveTextures(const TextureArray& tex) {
    for (const auto& b : blocks_) b->resolveTextures(tex);
}

} // namespace mc
