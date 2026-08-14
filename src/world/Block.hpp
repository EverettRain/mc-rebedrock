#pragma once

#include "core/Identifier.hpp"
#include "world/StateSchema.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::world {

// Blocks and items share one identifier type and one namespace.
using core::Identifier;
using core::kNamespace;
using core::kVanillaNamespace;

enum class Block : std::uint8_t {
    Air,
    Grass,
    Dirt,
    Stone,
    Cobblestone,
    OakPlanks,
    OakLog,
    Bricks,
    Bedrock,
    Sand,
    Glass,
    CoalOre,
    IronOre,
    GoldOre,
    DiamondOre,
    GrassPlant,
    Dandelion,
    OakSapling,
    OakLeaves,
    Water,
    Gravel,
    SprucePlanks,
    BirchPlanks,
    SpruceLog,
    BirchLog,
    Bookshelf,
    CraftingTable,
    Furnace,
    Obsidian,
    Clay,
    SnowBlock,
    Netherrack,
    Glowstone,
    WhiteWool,
    RedWool,
    BlackWool,
    StoneBricks,
    MossyCobblestone,
    Sandstone,
    Pumpkin,
    Melon,
    Tnt,
    Granite,
    Diorite,
    Andesite,
    CoarseDirt,
    Podzol,
    RedSand,
    Lava,
    Torch,
    WallTorch,
    Chest,
    LapisOre,
    RedstoneOre,
    EmeraldOre,
    MossyStoneBricks,
    ChiseledStoneBricks,
    QuartzBlock,
    JungleLog,
    JunglePlanks,
    AcaciaLog,
    AcaciaPlanks,
    DarkOakLog,
    DarkOakPlanks,
    SpruceLeaves,
    BirchLeaves,
    JungleLeaves,
    AcaciaLeaves,
    DarkOakLeaves,
    SpruceSapling,
    BirchSapling,
    JungleSapling,
    AcaciaSapling,
    DarkOakSapling,
    // FarmlandBlock: the tilled soil a hoe produces. A single block carries a
    // MOISTURE property 0-7 (vanilla's FarmlandBlock.MOISTURE), which the top
    // face texture and the crop growth rate both read. Not soil():
    // grass cannot spread into it and saplings will not grow on it, exactly like
    // the Java block that only accepts crops.
    Farmland,
    // CropBlock: wheat, carrots and potatoes, one block per species. Each carries
    // an AGE property 0-7 (vanilla's CropBlock.AGE). The mesher picks the stage
    // texture from the age, and the random tick advances it.
    WheatCrops,
    Carrots,
    Potatoes,
    // The decorative stone variants added to round out the stone family: each
    // polished stone is the 2x2-crafted version of its parent, and smooth stone
    // is smelted from stone in the furnace (no crafting recipe, like 1.16.1).
    PolishedGranite,
    PolishedDiorite,
    PolishedAndesite,
    SmoothStone,
    Count,
};

// The direction of a block's authored "front" (furnaces/chests), or the
// direction of the end grain for pillar blocks such as logs.
enum class BlockOrientation : std::uint8_t {
    North,
    East,
    South,
    West,
    Up,
    Down,
};

enum class BlockRenderLayer : std::uint8_t {
    Opaque,
    Cutout,
    Translucent,
};

enum class BlockModel : std::uint8_t {
    Cube,
    Cross,
    // A crop: the crossed plant quads of a Cross model, but the texture is read
    // from the block's age rather than a single fixed layer (wheat stages 0-7,
    // carrots/potatoes map their eight ages onto four stage textures).
    Crop,
    Torch,
    Chest,
};

// What a block needs underneath or beside it in order to stay in the world.
enum class BlockSupport : std::uint8_t {
    None,
    // A sturdy upward face below (torches).
    Ground,
    // A sturdy face on the wall the block hangs off (wall torches).
    Wall,
    // Dirt-like soil below (grass, flowers, saplings).
    Soil,
    // Farmland below (crops, the way BushBlock#mayPlaceOn accepts FarmlandBlock).
    Farmland,
};

// Vanilla's AbstractBlock.OffsetType: whether the model is drawn with a
// deterministic per-position jitter (AbstractBlock#getModelOffset). XZ shifts
// the plant a few pixels off its block centre in both horizontal axes, the way
// flowers and grass sit in vanilla; XYZ adds a small downward drop as well.
enum class BlockOffsetType : std::uint8_t {
    None,
    XZ,
    XYZ,
};

// The container screen right-clicking a block opens (vanilla's BlockBehaviour
// onBlockUse), read off the block's registry entry so the interaction system
// never compares against a specific block.
enum class ContainerType : std::uint8_t {
    None,
    CraftingTable,
    Furnace,
    Chest,
};

// The resolved atlas layer for each face of a block, filled by the renderer's
// name-driven atlas build (see textureLayers/setBlockTextureLayers below).
struct BlockTextureLayers final {
    float top = 0.0F;
    float side = 0.0F;
    float bottom = 0.0F;
};

// The block's textures by vanilla file name ("granite", "grass_block_top",
// "dirt"), mirroring how 1.16.1 blocks reference sprites by ResourceLocation.
// The renderer resolves the names into atlas layer indices once at startup and
// writes the per-block layers into kBlockTextureLayers; the mesher and the GUI
// read those precomputed floats, so the hot paths never touch a string.
struct BlockTextureNames final {
    const char* top = nullptr;
    const char* side = nullptr;
    const char* bottom = nullptr;
};

// Everything the engine knows about one block. Instances are produced by
// BlockProperties (below) and live in the registry table, never built by hand.
struct BlockDefinition final {
    Block block = Block::Air;
    // The registry key, always in this project's namespace.
    Identifier identifier{};
    // The vanilla block this one mirrors, empty for original content. Drives
    // translation keys and 1.16.1 asset lookups; several block states may share
    // one vanilla name, the way the four wall torches do.
    Identifier vanilla{};
    // Fallback English name. Localized text comes from the translation key the
    // identifiers below produce; this is what shows when a key is missing.
    const char* displayName = "";
    BlockTextureNames textures{};
    float hardness = 0.0F;
    float blastResistance = 0.0F;
    std::uint8_t maximumStackSize = 64U;
    BlockRenderLayer renderLayer = BlockRenderLayer::Opaque;
    bool collision = true;
    BlockModel model = BlockModel::Cube;
    // The height of the block's solid box, in [0, 1]. Full cubes are 1.0; a
    // truncated block (farmland is 15/16 in vanilla) shrinks its top face and
    // the tops of its side faces in the mesh, the selection box and collision.
    float modelHeight = 1.0F;
    bool replaceable = false;
    bool dropsItem = true;
    // Light the block emits, and how much sky light it swallows when it is not
    // a full opaque cube (leaves and water dim the column by one).
    std::uint8_t light = 0U;
    std::uint8_t lightFilter = 0U;
    BlockSupport support = BlockSupport::None;
    // AbstractBlock.OffsetType: None for a model pinned to its block centre,
    // XZ/XYZ for the deterministic per-position jitter described above.
    BlockOffsetType offsetType = BlockOffsetType::None;
    bool soil = false;
    bool gravity = false;
    // Takes the axis of the face it was placed against (logs).
    bool pillar = false;
    // Reads a horizontal FACING property (furnaces, chests).
    bool horizontalFacing = false;
    // Carries a LIT property, the way AbstractFurnaceBlock does: the same block
    // whether or not it is burning, with the lit front face and the light level
    // coming from the state rather than from a second block. `litLight` is what
    // it emits while lit; unlit it emits `light` like anything else.
    bool lit = false;
    std::uint8_t litLight = 0U;
    // The container screen this block opens on right-click, None otherwise.
    ContainerType container = ContainerType::None;
    bool torch = false;
    // A LeavesBlock: decays when no log is left within six blocks of it, unless
    // a player placed it. See BlockState::persistent().
    bool leaves = false;
    // Java's `createBlockStateDefinition`: which properties this block's states
    // are the cartesian product of. Declared through the builder below — the
    // flags above that imply a property (pillar, horizontalFacing, lit, leaves)
    // add it themselves, so a block cannot own a facing flag and forget the
    // axis that stores the facing.
    StateSchema states{};
};

// Java's Block.Properties: one chained expression declares a block completely,
// and the result converts straight into the registry table entry.
class BlockProperties final {
  public:
    // Registers a block that mirrors vanilla: `path` names both the
    // `rebedrock:` registry key and the `minecraft:` alias behind it.
    [[nodiscard]] static constexpr BlockProperties of(Block block, std::string_view path,
                                                      const char* displayName) {
        BlockProperties properties;
        properties.definition_.block = block;
        properties.definition_.identifier = {kNamespace, path};
        properties.definition_.vanilla = {kVanillaNamespace, path};
        properties.definition_.displayName = displayName;
        return properties;
    }

    // Original content: no vanilla alias, so it falls back to its own name for
    // translation and keeps whatever display name it was given.
    [[nodiscard]] constexpr BlockProperties custom() const {
        BlockProperties copy = *this;
        copy.definition_.vanilla = {};
        return copy;
    }

    // A block state whose vanilla counterpart is a different, shared name — the
    // four wall torches are all `minecraft:wall_torch`.
    [[nodiscard]] constexpr BlockProperties vanillaAlias(std::string_view path) const {
        BlockProperties copy = *this;
        copy.definition_.vanilla = {kVanillaNamespace, path};
        return copy;
    }

    [[nodiscard]] constexpr BlockProperties texture(const char* all) const {
        return texture(all, all, all);
    }
    [[nodiscard]] constexpr BlockProperties texture(const char* top, const char* side,
                                                    const char* bottom) const {
        BlockProperties copy = *this;
        copy.definition_.textures = {top, side, bottom};
        return copy;
    }

    // Hardness alone mirrors Java's strength(x) shorthand, where the blast
    // resistance matches the hardness.
    [[nodiscard]] constexpr BlockProperties strength(float hardness) const {
        return strength(hardness, hardness);
    }
    [[nodiscard]] constexpr BlockProperties strength(float hardness, float blastResistance) const {
        BlockProperties copy = *this;
        copy.definition_.hardness = hardness;
        copy.definition_.blastResistance = blastResistance;
        return copy;
    }
    // Hardness 0: the block gives way on the tick the swing starts.
    [[nodiscard]] constexpr BlockProperties instantBreak(float blastResistance = 0.0F) const {
        return strength(0.0F, blastResistance);
    }
    [[nodiscard]] constexpr BlockProperties unbreakable(float blastResistance) const {
        return strength(-1.0F, blastResistance);
    }

    [[nodiscard]] constexpr BlockProperties renderLayer(BlockRenderLayer layer) const {
        BlockProperties copy = *this;
        copy.definition_.renderLayer = layer;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties model(BlockModel shape) const {
        BlockProperties copy = *this;
        copy.definition_.model = shape;
        return copy;
    }
    // Shrinks the block's solid box to the given height (vanilla farmland is a
    // 15/16-high cube).
    [[nodiscard]] constexpr BlockProperties height(float value) const {
        BlockProperties copy = *this;
        copy.definition_.modelHeight = value;
        return copy;
    }
    // AbstractBlock.OffsetType. Cross plants opt into the per-position jitter
    // here; a block whose model is pinned to its block centre stays None.
    [[nodiscard]] constexpr BlockProperties offsetType(BlockOffsetType type) const {
        BlockProperties copy = *this;
        copy.definition_.offsetType = type;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties noCollision() const {
        BlockProperties copy = *this;
        copy.definition_.collision = false;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties replaceable() const {
        BlockProperties copy = *this;
        copy.definition_.replaceable = true;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties noDrops() const {
        BlockProperties copy = *this;
        copy.definition_.dropsItem = false;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties stackSize(std::uint8_t size) const {
        BlockProperties copy = *this;
        copy.definition_.maximumStackSize = size;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties light(std::uint8_t level) const {
        BlockProperties copy = *this;
        copy.definition_.light = level;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties lightFilter(std::uint8_t level) const {
        BlockProperties copy = *this;
        copy.definition_.lightFilter = level;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties support(BlockSupport requirement) const {
        BlockProperties copy = *this;
        copy.definition_.support = requirement;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties soil() const {
        BlockProperties copy = *this;
        copy.definition_.soil = true;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties gravity() const {
        BlockProperties copy = *this;
        copy.definition_.gravity = true;
        return copy;
    }
    // Java's `Block#createBlockStateDefinition`: declare one property of this
    // block's state. The states are the cartesian product of everything
    // declared here, so a block with facing(4) and lit(2) has eight.
    //
    // Most blocks never call this directly — the flag methods below declare
    // their own property, exactly as vanilla's block subclasses do. Reach for
    // it when a property has no flag of its own (a crop's age, water's level).
    [[nodiscard]] constexpr BlockProperties state(StateProperty property,
                                                  std::uint8_t valueCount) const {
        BlockProperties copy = *this;
        copy.definition_.states.add(property, valueCount);
        return copy;
    }
    // RotatedPillarBlock: takes the axis of the face it was placed against, so
    // its facing axis is the full six directions rather than the horizontal four.
    [[nodiscard]] constexpr BlockProperties pillar() const {
        BlockProperties copy = *this;
        copy.definition_.pillar = true;
        return copy.state(StateProperty::Facing, 6U);
    }
    [[nodiscard]] constexpr BlockProperties horizontalFacing() const {
        BlockProperties copy = *this;
        copy.definition_.horizontalFacing = true;
        return copy.state(StateProperty::Facing, 4U);
    }
    // AbstractFurnaceBlock's LIT: one block, two states, and the light level
    // the burning one emits.
    [[nodiscard]] constexpr BlockProperties lit(std::uint8_t litLightLevel) const {
        BlockProperties copy = *this;
        copy.definition_.lit = true;
        copy.definition_.litLight = litLightLevel;
        return copy.state(StateProperty::Lit, 2U);
    }
    // Right-clicking opens this container screen (BlockBehaviour#onBlockUse).
    [[nodiscard]] constexpr BlockProperties container(ContainerType type) const {
        BlockProperties copy = *this;
        copy.definition_.container = type;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties torch() const {
        BlockProperties copy = *this;
        copy.definition_.torch = true;
        return copy;
    }
    // A LeavesBlock: cutout, one level of light filtering, subject to decay, and
    // carrying the PERSISTENT flag that exempts player-placed leaves from it.
    [[nodiscard]] constexpr BlockProperties leaves() const {
        BlockProperties copy = *this;
        copy.definition_.leaves = true;
        return copy.strength(0.2F)
            .renderLayer(BlockRenderLayer::Cutout)
            .lightFilter(1U)
            .state(StateProperty::Persistent, 2U);
    }

    // The shorthands vanilla blocks reach for again and again.
    [[nodiscard]] constexpr BlockProperties cross() const {
        return renderLayer(BlockRenderLayer::Cutout).model(BlockModel::Cross).noCollision();
    }

    constexpr operator BlockDefinition() const { return definition_; }

  private:
    constexpr BlockProperties() = default;

    BlockDefinition definition_{};
};

// The block registry. One entry per Block value, in enum order, each written as
// a single chained statement. Adding a block is: add the enum value, add the
// line here, done — every table that used to need a parallel switch reads the
// properties below instead.
inline constexpr std::array<BlockDefinition, static_cast<std::size_t>(Block::Count)> kBlockRegistry{
    BlockProperties::of(Block::Air, "air", "Air")
        .stackSize(0U)
        .renderLayer(BlockRenderLayer::Translucent)
        .noCollision()
        .replaceable()
        .noDrops(),
    BlockProperties::of(Block::Grass, "grass_block", "Grass Block")
        .texture("grass_block_top", "grass_block_side", "dirt")
        .strength(0.6F)
        .soil(),
    BlockProperties::of(Block::Dirt, "dirt", "Dirt").texture("dirt").strength(0.5F).soil(),
    BlockProperties::of(Block::Stone, "stone", "Stone").texture("stone").strength(1.5F, 6.0F),
    BlockProperties::of(Block::Cobblestone, "cobblestone", "Cobblestone")
        .texture("cobblestone")
        .strength(2.0F, 6.0F),
    BlockProperties::of(Block::OakPlanks, "oak_planks", "Oak Planks")
        .texture("oak_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::OakLog, "oak_log", "Oak Log")
        .texture("oak_log_top", "oak_log", "oak_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::Bricks, "bricks", "Bricks").texture("bricks").strength(2.0F, 6.0F),
    BlockProperties::of(Block::Bedrock, "bedrock", "Bedrock")
        .texture("bedrock")
        .unbreakable(3'600'000.0F),
    BlockProperties::of(Block::Sand, "sand", "Sand").texture("sand").strength(0.5F).gravity(),
    BlockProperties::of(Block::Glass, "glass", "Glass")
        .texture("glass")
        .strength(0.3F)
        .renderLayer(BlockRenderLayer::Translucent),
    BlockProperties::of(Block::CoalOre, "coal_ore", "Coal Ore").texture("coal_ore").strength(3.0F),
    BlockProperties::of(Block::IronOre, "iron_ore", "Iron Ore").texture("iron_ore").strength(3.0F),
    BlockProperties::of(Block::GoldOre, "gold_ore", "Gold Ore").texture("gold_ore").strength(3.0F),
    BlockProperties::of(Block::DiamondOre, "diamond_ore", "Diamond Ore")
        .texture("diamond_ore")
        .strength(3.0F),
    // Material.REPLACEABLE_PLANT: placing a block inside tall grass replaces it.
    BlockProperties::of(Block::GrassPlant, "short_grass", "Short Grass")
        .texture("short_grass")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::Dandelion, "dandelion", "Dandelion")
        .texture("dandelion")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::OakSapling, "oak_sapling", "Oak Sapling")
        .texture("oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::OakLeaves, "oak_leaves", "Oak Leaves")
        .texture("oak_leaves")
        .leaves(),
    BlockProperties::of(Block::Water, "water", "Water")
        .texture("water_still", "water_flow", "water_flow")
        .strength(100.0F)
        .renderLayer(BlockRenderLayer::Translucent)
        .noCollision()
        .replaceable()
        .noDrops()
        .lightFilter(1U)
        // LiquidBlock.LEVEL: 0 is a source, 1-7 are flowing depths and 8 is
        // falling water.
        .state(StateProperty::FluidLevel, 9U),
    BlockProperties::of(Block::Gravel, "gravel", "Gravel")
        .texture("gravel")
        .strength(0.6F)
        .gravity(),
    BlockProperties::of(Block::SprucePlanks, "spruce_planks", "Spruce Planks")
        .texture("spruce_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::BirchPlanks, "birch_planks", "Birch Planks")
        .texture("birch_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::SpruceLog, "spruce_log", "Spruce Log")
        .texture("spruce_log_top", "spruce_log", "spruce_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::BirchLog, "birch_log", "Birch Log")
        .texture("birch_log_top", "birch_log", "birch_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::Bookshelf, "bookshelf", "Bookshelf")
        .texture("oak_planks", "bookshelf", "oak_planks")
        .strength(1.5F),
    BlockProperties::of(Block::CraftingTable, "crafting_table", "Crafting Table")
        .texture("crafting_table_top", "crafting_table_side", "oak_planks")
        .strength(2.5F)
        .container(ContainerType::CraftingTable),
    // One furnace, lit or not: AbstractFurnaceBlock's LIT is a state, so a
    // burning furnace is the same block and keeps its block entity (and its
    // smelt) across the swap. The lit front is picked by the mesher
    // (kFurnaceFrontOnLayer); light 13 is what the burning state emits.
    BlockProperties::of(Block::Furnace, "furnace", "Furnace")
        .texture("furnace_top", "furnace_side", "furnace_top")
        .strength(3.5F)
        .horizontalFacing()
        .lit(13U)
        .container(ContainerType::Furnace),
    BlockProperties::of(Block::Obsidian, "obsidian", "Obsidian")
        .texture("obsidian")
        .strength(50.0F, 1'200.0F),
    BlockProperties::of(Block::Clay, "clay", "Clay").texture("clay").strength(0.6F),
    BlockProperties::of(Block::SnowBlock, "snow_block", "Snow Block")
        .texture("snow")
        .strength(0.2F),
    BlockProperties::of(Block::Netherrack, "netherrack", "Netherrack")
        .texture("netherrack")
        .strength(0.4F),
    BlockProperties::of(Block::Glowstone, "glowstone", "Glowstone")
        .texture("glowstone")
        .strength(0.3F)
        .light(15U),
    BlockProperties::of(Block::WhiteWool, "white_wool", "White Wool")
        .texture("white_wool")
        .strength(0.8F),
    BlockProperties::of(Block::RedWool, "red_wool", "Red Wool").texture("red_wool").strength(0.8F),
    BlockProperties::of(Block::BlackWool, "black_wool", "Black Wool")
        .texture("black_wool")
        .strength(0.8F),
    BlockProperties::of(Block::StoneBricks, "stone_bricks", "Stone Bricks")
        .texture("stone_bricks")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::MossyCobblestone, "mossy_cobblestone", "Mossy Cobblestone")
        .texture("mossy_cobblestone")
        .strength(2.0F, 6.0F),
    BlockProperties::of(Block::Sandstone, "sandstone", "Sandstone")
        .texture("sandstone_top", "sandstone", "sandstone_bottom")
        .strength(0.8F),
    BlockProperties::of(Block::Pumpkin, "pumpkin", "Pumpkin")
        .texture("pumpkin_top", "pumpkin_side", "pumpkin_top")
        .strength(1.0F),
    BlockProperties::of(Block::Melon, "melon", "Melon")
        .texture("melon_top", "melon_side", "melon_top")
        .strength(1.0F),
    BlockProperties::of(Block::Tnt, "tnt", "TNT")
        .texture("tnt_top", "tnt_side", "tnt_bottom")
        .instantBreak(),
    BlockProperties::of(Block::Granite, "granite", "Granite")
        .texture("granite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::Diorite, "diorite", "Diorite")
        .texture("diorite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::Andesite, "andesite", "Andesite")
        .texture("andesite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::CoarseDirt, "coarse_dirt", "Coarse Dirt")
        .texture("coarse_dirt")
        .strength(0.5F)
        .soil(),
    BlockProperties::of(Block::Podzol, "podzol", "Podzol")
        .texture("podzol_top", "podzol_side", "dirt")
        .strength(0.5F)
        .soil(),
    BlockProperties::of(Block::RedSand, "red_sand", "Red Sand")
        .texture("red_sand")
        .strength(0.5F)
        .gravity(),
    // Carved cells below y=11 become lava (CaveCarver#carveAtPoint). Rendered
    // as a solid self-lit cube for now; it carries no fluid simulation. The
    // top face uses the animated still strip and the sides the animated flow
    // strip. Their bases/counts come from BlockAtlasLayout.hpp so resource-pack
    // animation data cannot drift away from shader literals.
    BlockProperties::of(Block::Lava, "lava", "Lava")
        .texture("lava_still", "lava_flow", "lava_flow")
        .strength(100.0F)
        .noDrops()
        .light(15U)
        .lightFilter(1U),
    BlockProperties::of(Block::Torch, "torch", "Torch")
        .texture("torch")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .light(14U)
        .support(BlockSupport::Ground)
        .torch(),
    // WallTorchBlock's FACING is a state, not four blocks.
    BlockProperties::of(Block::WallTorch, "wall_torch", "Wall Torch")
        .vanillaAlias("wall_torch")
        .texture("torch")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .light(14U)
        .support(BlockSupport::Wall)
        .horizontalFacing()
        .torch(),
    BlockProperties::of(Block::Chest, "chest", "Chest")
        .texture("chest", "chest", "chest")
        .strength(2.5F)
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Chest)
        .horizontalFacing()
        .container(ContainerType::Chest),
    BlockProperties::of(Block::LapisOre, "lapis_ore", "Lapis Lazuli Ore")
        .texture("lapis_ore")
        .strength(3.0F),
    BlockProperties::of(Block::RedstoneOre, "redstone_ore", "Redstone Ore")
        .texture("redstone_ore")
        .strength(3.0F),
    BlockProperties::of(Block::EmeraldOre, "emerald_ore", "Emerald Ore")
        .texture("emerald_ore")
        .strength(3.0F),
    BlockProperties::of(Block::MossyStoneBricks, "mossy_stone_bricks", "Mossy Stone Bricks")
        .texture("mossy_stone_bricks")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::ChiseledStoneBricks, "chiseled_stone_bricks",
                        "Chiseled Stone Bricks")
        .texture("chiseled_stone_bricks")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::QuartzBlock, "quartz_block", "Block of Quartz")
        .texture("quartz_block_top", "quartz_block_side", "quartz_block_top")
        .strength(0.8F),
    BlockProperties::of(Block::JungleLog, "jungle_log", "Jungle Log")
        .texture("jungle_log_top", "jungle_log", "jungle_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::JunglePlanks, "jungle_planks", "Jungle Planks")
        .texture("jungle_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::AcaciaLog, "acacia_log", "Acacia Log")
        .texture("acacia_log_top", "acacia_log", "acacia_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::AcaciaPlanks, "acacia_planks", "Acacia Planks")
        .texture("acacia_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::DarkOakLog, "dark_oak_log", "Dark Oak Log")
        .texture("dark_oak_log_top", "dark_oak_log", "dark_oak_log_top")
        .strength(2.0F)
        .pillar(),
    BlockProperties::of(Block::DarkOakPlanks, "dark_oak_planks", "Dark Oak Planks")
        .texture("dark_oak_planks")
        .strength(2.0F, 3.0F),
    BlockProperties::of(Block::SpruceLeaves, "spruce_leaves", "Spruce Leaves")
        .texture("spruce_leaves")
        .leaves(),
    BlockProperties::of(Block::BirchLeaves, "birch_leaves", "Birch Leaves")
        .texture("birch_leaves")
        .leaves(),
    BlockProperties::of(Block::JungleLeaves, "jungle_leaves", "Jungle Leaves")
        .texture("jungle_leaves")
        .leaves(),
    BlockProperties::of(Block::AcaciaLeaves, "acacia_leaves", "Acacia Leaves")
        .texture("acacia_leaves")
        .leaves(),
    BlockProperties::of(Block::DarkOakLeaves, "dark_oak_leaves", "Dark Oak Leaves")
        .texture("dark_oak_leaves")
        .leaves(),
    BlockProperties::of(Block::SpruceSapling, "spruce_sapling", "Spruce Sapling")
        .texture("spruce_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::BirchSapling, "birch_sapling", "Birch Sapling")
        .texture("birch_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::JungleSapling, "jungle_sapling", "Jungle Sapling")
        .texture("jungle_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::AcaciaSapling, "acacia_sapling", "Acacia Sapling")
        .texture("acacia_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    BlockProperties::of(Block::DarkOakSapling, "dark_oak_sapling", "Dark Oak Sapling")
        .texture("dark_oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil),
    // FarmlandBlock: the tilled soil a hoe makes. The top face swaps between
    // the dry and moist textures once the orientation's moisture passes 0;
    // the sides are plain dirt, matching the vanilla model. Breaking it
    // yields dirt (see minedDrops), never farmland itself. Its solid box is
    // 15/16 tall, the vanilla FarmlandBlock.SHAPE.
    BlockProperties::of(Block::Farmland, "farmland", "Farmland")
        .texture("farmland", "dirt", "dirt")
        .strength(0.6F)
        .height(15.0F / 16.0F)
        .state(StateProperty::Moisture, 8U),
    // CropBlock: wheat/carrot/potato share the crossed-plant render, with the
    // stage texture driven by their AGE property. They need farmland below,
    // have no collision of their own, and never drop themselves — minedDrops
    // rolls the species' loot table from the age.
    BlockProperties::of(Block::WheatCrops, "wheat", "Wheat")
        .texture("wheat_stage0")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Crop)
        .noCollision()
        .noDrops()
        .support(BlockSupport::Farmland)
        .state(StateProperty::Age, 8U),
    BlockProperties::of(Block::Carrots, "carrots", "Carrots")
        .texture("carrots_stage0")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Crop)
        .noCollision()
        .noDrops()
        .support(BlockSupport::Farmland)
        .state(StateProperty::Age, 8U),
    BlockProperties::of(Block::Potatoes, "potatoes", "Potatoes")
        .texture("potatoes_stage0")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Crop)
        .noCollision()
        .noDrops()
        .support(BlockSupport::Farmland)
        .state(StateProperty::Age, 8U),
    // Decorative stone variants. Each polished stone matches its parent's
    // hardness; smooth stone is the furnace product of stone. The texture
    // layers 242-245 occupy four of newContentTextures' placeholder slots.
    BlockProperties::of(Block::PolishedGranite, "polished_granite", "Polished Granite")
        .texture("polished_granite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::PolishedDiorite, "polished_diorite", "Polished Diorite")
        .texture("polished_diorite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::PolishedAndesite, "polished_andesite", "Polished Andesite")
        .texture("polished_andesite")
        .strength(1.5F, 6.0F),
    BlockProperties::of(Block::SmoothStone, "smooth_stone", "Smooth Stone")
        .texture("smooth_stone")
        .strength(2.0F, 6.0F),
};

[[nodiscard]] constexpr bool isValidBlock(Block block) {
    return static_cast<std::uint8_t>(block) < static_cast<std::uint8_t>(Block::Count);
}

[[nodiscard]] constexpr const BlockDefinition& blockDefinition(Block block) {
    return kBlockRegistry[isValidBlock(block) ? static_cast<std::size_t>(block) : 0U];
}

// The table is indexed by the enum value, so a misplaced line would silently
// hand out another block's properties. Both invariants are checked here rather
// than left for a bug report.
constexpr bool blockRegistryIsWellFormed() {
    for (std::size_t index = 0; index < kBlockRegistry.size(); ++index) {
        const auto& definition = kBlockRegistry[index];
        if (static_cast<std::size_t>(definition.block) != index)
            return false;
        if (definition.identifier.space != kNamespace || definition.identifier.path.empty()) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (kBlockRegistry[other].identifier == definition.identifier)
                return false;
        }
    }
    return true;
}
static_assert(blockRegistryIsWellFormed(),
              "kBlockRegistry must list every Block once, in enum order, with unique identifiers");

// Resolves a registry key to its block. Accepts `rebedrock:stone`, the vanilla
// alias `minecraft:stone`, and the bare `stone`.
[[nodiscard]] constexpr std::optional<Block> blockFromIdentifier(std::string_view text) {
    for (const auto& definition : kBlockRegistry) {
        if (definition.identifier.matches(text))
            return definition.block;
    }
    for (const auto& definition : kBlockRegistry) {
        if (definition.vanilla.matches(text))
            return definition.block;
    }
    return std::nullopt;
}

// The identifier a block's localized name is looked up under: the vanilla one
// when the block mirrors vanilla content, its own otherwise. Original blocks
// therefore need a `rebedrock.*` entry in the language files, and fall back to
// their display name until one exists.
[[nodiscard]] constexpr const Identifier& translationIdentifier(Block block) {
    const auto& definition = blockDefinition(block);
    return definition.vanilla.empty() ? definition.identifier : definition.vanilla;
}

[[nodiscard]] constexpr bool isRenderable(Block block) {
    return block != Block::Air && block != Block::Count;
}

[[nodiscard]] constexpr bool isOpaque(Block block) {
    return isRenderable(block) && blockDefinition(block).renderLayer == BlockRenderLayer::Opaque;
}

[[nodiscard]] constexpr std::uint8_t skyLightOpacity(Block block) {
    if (isOpaque(block))
        return 15U;
    return blockDefinition(block).lightFilter;
}

// Java's AbstractBlockState#getOpacity, the amount a block "shields" the cell
// above it in eyes of the spreadable-block checks. This is *not* the light
// filter used for propagation: opaque blocks report 15, water and lava (whose
// material is neither opaque nor transparent) report 3, and everything else 0.
// A water cell sitting above a grass block therefore makes SpreadableBlock's
// `canSpread` fail (3 > 2) and the grass revert to dirt on its next random
// tick — the vanilla 1.16.1 behaviour this project mirrors.
[[nodiscard]] constexpr int opacity(Block block) {
    if (isOpaque(block))
        return 15;
    if (block == Block::Water || block == Block::Lava)
        return 3;
    return 0;
}

[[nodiscard]] constexpr bool hasCollision(Block block) { return blockDefinition(block).collision; }

// The light a block emits in its *default* state. Blocks whose emission depends
// on a state — a furnace only glows while it burns — must be asked through
// BlockState::emittedLight() instead; this returns their unlit level.
[[nodiscard]] constexpr std::uint8_t emittedLight(Block block) {
    return blockDefinition(block).light;
}

[[nodiscard]] constexpr bool isTorch(Block block) { return blockDefinition(block).torch; }

// Wall torches sit flush against their wall, the way 1.16.1's WallTorchBlock
// AABB runs all the way to the block face (a north-facing torch spans z 11..16
// of 16). This is the inset of the model's root from the cell centre toward the
// wall; the mesh and the selection box share it so clicking matches the look.
inline constexpr float kWallTorchInset = 0.5F;

[[nodiscard]] constexpr bool isLog(Block block) { return blockDefinition(block).pillar; }

[[nodiscard]] constexpr bool isLeaves(Block block) { return blockDefinition(block).leaves; }

// Java's LeavesBlock.PERSISTENT is a declared property of every leaves block
// (see the registry entries): leaves a player placed stay put, leaves that grew
// with a tree decay once the trunk that fed them is gone. Read it through
// `BlockState::persistent()`; it used to be a magic value in the orientation
// byte, which is the overloading the state schema exists to end.

// LeavesBlock.DISTANCE only reaches 7, so leaves further than six steps through
// other leaves from any log are the ones that decay.
inline constexpr int kMaximumLeafSupportDistance = 6;

// The sapling a wood set's leaves roll on their loot table.
[[nodiscard]] constexpr Block saplingForLeaves(Block leaves) {
    switch (leaves) {
    case Block::SpruceLeaves:
        return Block::SpruceSapling;
    case Block::BirchLeaves:
        return Block::BirchSapling;
    case Block::JungleLeaves:
        return Block::JungleSapling;
    case Block::AcaciaLeaves:
        return Block::AcaciaSapling;
    case Block::DarkOakLeaves:
        return Block::DarkOakSapling;
    default:
        return Block::OakSapling;
    }
}

[[nodiscard]] constexpr bool isReplaceable(Block block) {
    return blockDefinition(block).replaceable;
}

[[nodiscard]] constexpr bool isFluid(Block block) { return block == Block::Water; }

// Whether the block fills its whole 1x1x1 cell. Cross plants, torches, chests
// and fluids are the "incomplete" blocks: they neither occlude a neighbour face
// nor hand a full face to whatever wants to attach to them.
[[nodiscard]] constexpr bool isFullCube(Block block) {
    return isRenderable(block) && !isFluid(block) &&
           blockDefinition(block).model == BlockModel::Cube;
}

// Java's BlockState#isFaceSturdy: only a full collision cube can carry an
// attached block. Glass qualifies, leaves deliberately do not.
[[nodiscard]] constexpr bool isFaceSturdy(Block block) {
    return isFullCube(block) && blockDefinition(block).collision && !isLeaves(block);
}

// The surface a walking land entity may use as ground. This is deliberately a
// gameplay/navigation property rather than an alias for hasCollision(): leaves
// have a collision shape, but vanilla's MOTION_BLOCKING_NO_LEAVES heightmap and
// land path-node classification do not promote a canopy into ordinary ground.
// Keep partial collision blocks (notably farmland) usable by mobs.
[[nodiscard]] constexpr bool isLandEntitySupport(Block block) {
    return blockDefinition(block).collision && !isLeaves(block);
}

// Whether the block darkens a smooth-lighting AO corner (vanilla 1.16.1
// AbstractBlock#getAmbientOcclusionLightLevel: a full cube whose material is
// opaque returns 0.2, everything else 1.0). isFullCube alone is wrong: leaves,
// glass and glowstone are cube-shaped but their vanilla materials are not
// opaque, so they must not darken corners.
[[nodiscard]] constexpr bool aoOccludes(Block block) {
    return isFullCube(block) && !isLeaves(block) && isOpaque(block) && block != Block::Glowstone;
}

// Flowing water washes away decoration blocks that do not block motion. Crops
// are no-collision too, but vanilla's crops survive water (their material is
// not REPLACEABLE_PLANT), so they are carved out of the fluid-destroyed set.
[[nodiscard]] constexpr bool isCrop(Block block) {
    return block == Block::WheatCrops || block == Block::Carrots || block == Block::Potatoes;
}
[[nodiscard]] constexpr bool isDestroyedByFluid(Block block) {
    return isRenderable(block) && !isFluid(block) && !isCrop(block) &&
           !blockDefinition(block).collision;
}

[[nodiscard]] constexpr BlockSupport blockSupport(Block block) {
    return blockDefinition(block).support;
}

// BushBlock#mayPlaceOn in Java 1.16.1.
[[nodiscard]] constexpr bool isSoil(Block block) { return blockDefinition(block).soil; }

[[nodiscard]] constexpr bool isFarmland(Block block) { return block == Block::Farmland; }

// A crop's age (vanilla's CropBlock.AGE) and farmland's moisture
// (FarmlandBlock.MOISTURE) are declared properties of those blocks, read
// through `BlockState::age()` / `BlockState::moisture()`. Both default to 0, so
// a freshly placed crop and freshly tilled farmland are each their block's
// default state and need no explicit write.
//
// They used to be `cropAge(BlockOrientation)` / `farmlandOrientation(int)`:
// values 0-7 stuffed into a six-value direction enum and masked back out with
// `& 0x7`. Those functions are gone rather than deprecated, so nothing can
// reach the old encoding by accident.

// The stage-texture index for a crop age. Wheat has one texture per age; the
// carrots/potatoes blockstate maps their eight ages onto four stage textures
// (0-1 -> stage 0, 2-3 -> stage 1, 4-6 -> stage 2, 7 -> stage 3).
[[nodiscard]] constexpr int cropStageIndex(Block block, int age) {
    if (block == Block::WheatCrops) {
        return age < 0 ? 0 : (age > 7 ? 7 : age);
    }
    if (age <= 1)
        return 0;
    if (age <= 3)
        return 1;
    if (age <= 6)
        return 2;
    return 3;
}

// The crop's stage-0 texture name the registry stores (wheat_stage0, ...).
[[nodiscard]] constexpr const char* cropStage0Name(Block block) {
    switch (block) {
    case Block::WheatCrops:
        return "wheat_stage0";
    case Block::Carrots:
        return "carrots_stage0";
    case Block::Potatoes:
        return "potatoes_stage0";
    default:
        return nullptr;
    }
}

// The selection-box height of a crop at the given age, from vanilla's
// CropBlock.SHAPES: ages 0-7 grow in 2/16 steps from 2/16 to a full block
// (0.125, 0.25, ... , 1.0).
[[nodiscard]] constexpr float cropSelectionHeight(int age) {
    const int clamped = age < 0 ? 0 : (age > 7 ? 7 : age);
    return static_cast<float>(clamped + 1) * 2.0F / 16.0F;
}

// Vanilla FarmlandBlock.SHAPE: a 15/16-high box.
inline constexpr float kFarmlandModelHeight = 15.0F / 16.0F;

[[nodiscard]] constexpr bool isAffectedByGravity(Block block) {
    return blockDefinition(block).gravity;
}

// Blocks whose model reads a horizontal FACING property (HorizontalDirectionalBlock).
[[nodiscard]] constexpr bool hasHorizontalFacing(Block block) {
    return blockDefinition(block).horizontalFacing;
}

[[nodiscard]] constexpr BlockOrientation defaultOrientation(Block block) {
    return isLog(block) ? BlockOrientation::Up : BlockOrientation::North;
}

[[nodiscard]] constexpr bool isHorizontal(BlockOrientation orientation) {
    return orientation != BlockOrientation::Up && orientation != BlockOrientation::Down;
}

[[nodiscard]] constexpr BlockOrientation oppositeOrientation(BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::North:
        return BlockOrientation::South;
    case BlockOrientation::East:
        return BlockOrientation::West;
    case BlockOrientation::South:
        return BlockOrientation::North;
    case BlockOrientation::West:
        return BlockOrientation::East;
    case BlockOrientation::Up:
        return BlockOrientation::Down;
    case BlockOrientation::Down:
        return BlockOrientation::Up;
    }
    return BlockOrientation::North;
}

// The direction a wall torch leans toward, matching the vanilla wall_torch
// FACING property. Now that the four wall torches are one block, the facing is
// the cell's state rather than its identity, so these take the orientation.
// The wall that carries the torch is on the opposite side.
[[nodiscard]] constexpr BlockOrientation wallTorchSupportSide(BlockOrientation facing) {
    return oppositeOrientation(facing);
}

[[nodiscard]] constexpr bool isSelectable(Block block) {
    // The normal interaction ray ignores fluids, but still needs to hit
    // non-colliding cross models such as grass and flowers.
    return isRenderable(block) && !isFluid(block);
}

// The resolved atlas layer for each face of a block. `kBlockTextureLayers` is
// filled once by the renderer's atlas builder from the block registry's texture
// names; reading it here keeps the mesher's per-vertex path a plain array
// index — the name resolution happens once at startup, not per face. The
// C++17 inline variable shares one instance across translation units without a
// dedicated source file.
inline std::array<BlockTextureLayers, static_cast<std::size_t>(Block::Count)> kBlockTextureLayers{};

[[nodiscard]] inline const BlockTextureLayers& textureLayers(Block block) {
    const auto index = static_cast<std::size_t>(block);
    return kBlockTextureLayers[index < kBlockTextureLayers.size() ? index : 0U];
}

// The renderer registers a block's resolved top/side/bottom atlas layers here.
inline void setBlockTextureLayers(Block block, BlockTextureLayers layers) {
    kBlockTextureLayers[static_cast<std::size_t>(block)] = layers;
}

// The crop's stage textures are laid out contiguously from its stage-0 layer,
// so the mesher reads stage0 + age.
[[nodiscard]] inline float cropTextureLayer(Block block, int age) {
    const int stage = cropStageIndex(block, age);
    return textureLayers(block).top + static_cast<float>(stage);
}

[[nodiscard]] constexpr const char* blockName(Block block) {
    return blockDefinition(block).displayName;
}

} // namespace mc::world
