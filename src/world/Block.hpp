#pragma once

#include "core/Identifier.hpp"

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
    // The burning furnace: LitFurnaceBlock in 1.16.1, the same block with the
    // lit front face and a 13 light level. Not a separate item — breaking it
    // yields the unlit furnace.
    LitFurnace,
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
    WallTorchNorth,
    WallTorchEast,
    WallTorchSouth,
    WallTorchWest,
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
    // moisture value 0-7 in its orientation state (vanilla's FarmlandBlock.MOISTURE),
    // which the top face texture and the crop growth rate both read. Not soil():
    // grass cannot spread into it and saplings will not grow on it, exactly like
    // the Java block that only accepts crops.
    Farmland,
    // CropBlock: wheat, carrots and potatoes, one block per species. Each stores
    // its age 0-7 in the orientation state (vanilla's CropsBlock.AGE), the same
    // slot leaves use for their persistent flag. The mesher picks the stage
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

// The texture-array layers for the crop and farmland blocks, appended to the end
// of the base block atlas in VulkanRenderer.cpp. The order here is the insertion
// order there; these named constants are what both the block registry and the
// mesher read, so the hand-ordered atlas and the blocks can never disagree.
inline constexpr float kWheatStage0Layer = 274.0F;
inline constexpr float kWheatStage1Layer = 275.0F;
inline constexpr float kWheatStage2Layer = 276.0F;
inline constexpr float kWheatStage3Layer = 277.0F;
inline constexpr float kWheatStage4Layer = 278.0F;
inline constexpr float kWheatStage5Layer = 279.0F;
inline constexpr float kWheatStage6Layer = 280.0F;
inline constexpr float kWheatStage7Layer = 281.0F;
inline constexpr float kCarrotStage0Layer = 282.0F;
inline constexpr float kCarrotStage1Layer = 283.0F;
inline constexpr float kCarrotStage2Layer = 284.0F;
inline constexpr float kCarrotStage3Layer = 285.0F;
inline constexpr float kPotatoStage0Layer = 286.0F;
inline constexpr float kPotatoStage1Layer = 287.0F;
inline constexpr float kPotatoStage2Layer = 288.0F;
inline constexpr float kPotatoStage3Layer = 289.0F;
inline constexpr float kFarmlandLayer = 290.0F;
inline constexpr float kFarmlandMoistLayer = 291.0F;

struct BlockTextureLayers final {
    float top = 0.0F;
    float side = 0.0F;
    float bottom = 0.0F;
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
    BlockTextureLayers textures{};
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
    // The container screen this block opens on right-click, None otherwise.
    ContainerType container = ContainerType::None;
    bool torch = false;
    // A LeavesBlock: decays when no log is left within six blocks of it, unless
    // a player placed it. See leavesArePersistent below.
    bool leaves = false;
};

// Java's Block.Properties: one chained expression declares a block completely,
// and the result converts straight into the registry table entry.
class BlockProperties final {
  public:
    // Registers a block that mirrors vanilla: `path` names both the
    // `rebedrock:` registry key and the `minecraft:` alias behind it.
    [[nodiscard]] static constexpr BlockProperties of(
        Block block, std::string_view path, const char* displayName) {
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

    [[nodiscard]] constexpr BlockProperties texture(float all) const {
        return texture(all, all, all);
    }
    [[nodiscard]] constexpr BlockProperties texture(float top, float side, float bottom) const {
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
    [[nodiscard]] constexpr BlockProperties pillar() const {
        BlockProperties copy = *this;
        copy.definition_.pillar = true;
        return copy;
    }
    [[nodiscard]] constexpr BlockProperties horizontalFacing() const {
        BlockProperties copy = *this;
        copy.definition_.horizontalFacing = true;
        return copy;
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
    // A LeavesBlock: cutout, one level of light filtering, and subject to decay.
    [[nodiscard]] constexpr BlockProperties leaves() const {
        BlockProperties copy = *this;
        copy.definition_.leaves = true;
        return copy.strength(0.2F).renderLayer(BlockRenderLayer::Cutout).lightFilter(1U);
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
inline constexpr std::array<BlockDefinition, static_cast<std::size_t>(Block::Count)>
    kBlockRegistry{
        BlockProperties::of(Block::Air, "air", "Air")
            .stackSize(0U)
            .renderLayer(BlockRenderLayer::Translucent)
            .noCollision()
            .replaceable()
            .noDrops(),
        BlockProperties::of(Block::Grass, "grass_block", "Grass Block")
            .texture(0.0F, 1.0F, 2.0F)
            .strength(0.6F)
            .soil(),
        BlockProperties::of(Block::Dirt, "dirt", "Dirt")
            .texture(2.0F)
            .strength(0.5F)
            .soil(),
        BlockProperties::of(Block::Stone, "stone", "Stone")
            .texture(3.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::Cobblestone, "cobblestone", "Cobblestone")
            .texture(6.0F)
            .strength(2.0F, 6.0F),
        BlockProperties::of(Block::OakPlanks, "oak_planks", "Oak Planks")
            .texture(7.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::OakLog, "oak_log", "Oak Log")
            .texture(9.0F, 8.0F, 9.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::Bricks, "bricks", "Bricks")
            .texture(10.0F)
            .strength(2.0F, 6.0F),
        BlockProperties::of(Block::Bedrock, "bedrock", "Bedrock")
            .texture(4.0F)
            .unbreakable(3'600'000.0F),
        BlockProperties::of(Block::Sand, "sand", "Sand")
            .texture(5.0F)
            .strength(0.5F)
            .gravity(),
        BlockProperties::of(Block::Glass, "glass", "Glass")
            .texture(11.0F)
            .strength(0.3F)
            .renderLayer(BlockRenderLayer::Translucent),
        BlockProperties::of(Block::CoalOre, "coal_ore", "Coal Ore")
            .texture(12.0F)
            .strength(3.0F),
        BlockProperties::of(Block::IronOre, "iron_ore", "Iron Ore")
            .texture(13.0F)
            .strength(3.0F),
        BlockProperties::of(Block::GoldOre, "gold_ore", "Gold Ore")
            .texture(14.0F)
            .strength(3.0F),
        BlockProperties::of(Block::DiamondOre, "diamond_ore", "Diamond Ore")
            .texture(15.0F)
            .strength(3.0F),
        // Material.REPLACEABLE_PLANT: placing a block inside tall grass replaces it.
        BlockProperties::of(Block::GrassPlant, "grass", "Grass")
            .texture(16.0F)
            .instantBreak()
            .cross()
            .offsetType(BlockOffsetType::XZ)
            .replaceable()
            .noDrops()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::Dandelion, "dandelion", "Dandelion")
            .texture(17.0F)
            .instantBreak()
            .cross()
            .offsetType(BlockOffsetType::XZ)
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::OakSapling, "oak_sapling", "Oak Sapling")
            .texture(18.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::OakLeaves, "oak_leaves", "Oak Leaves")
            .texture(19.0F)
            .leaves(),
        BlockProperties::of(Block::Water, "water", "Water")
            .texture(20.0F, 52.0F, 52.0F)
            .strength(100.0F)
            .renderLayer(BlockRenderLayer::Translucent)
            .noCollision()
            .replaceable()
            .noDrops()
            .lightFilter(1U),
        BlockProperties::of(Block::Gravel, "gravel", "Gravel")
            .texture(86.0F)
            .strength(0.6F)
            .gravity(),
        BlockProperties::of(Block::SprucePlanks, "spruce_planks", "Spruce Planks")
            .texture(87.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::BirchPlanks, "birch_planks", "Birch Planks")
            .texture(88.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::SpruceLog, "spruce_log", "Spruce Log")
            .texture(90.0F, 89.0F, 90.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::BirchLog, "birch_log", "Birch Log")
            .texture(92.0F, 91.0F, 92.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::Bookshelf, "bookshelf", "Bookshelf")
            .texture(7.0F, 93.0F, 7.0F)
            .strength(1.5F),
        BlockProperties::of(Block::CraftingTable, "crafting_table", "Crafting Table")
            .texture(94.0F, 95.0F, 7.0F)
            .strength(2.5F)
            .container(ContainerType::CraftingTable),
        BlockProperties::of(Block::Furnace, "furnace", "Furnace")
            .texture(96.0F, 97.0F, 96.0F)
            .strength(3.5F)
            .horizontalFacing()
            .container(ContainerType::Furnace),
        // The lit front is picked by the mesher (kFurnaceFrontOnLayer); the
        // registry entry keeps the same top/side/back faces and the furnace's
        // interaction. Light 13 is LitFurnaceBlock's level.
        BlockProperties::of(Block::LitFurnace, "lit_furnace", "Furnace")
            .vanillaAlias("furnace")
            .texture(96.0F, 97.0F, 96.0F)
            .strength(3.5F)
            .light(13U)
            .horizontalFacing()
            .container(ContainerType::Furnace),
        BlockProperties::of(Block::Obsidian, "obsidian", "Obsidian")
            .texture(98.0F)
            .strength(50.0F, 1'200.0F),
        BlockProperties::of(Block::Clay, "clay", "Clay")
            .texture(99.0F)
            .strength(0.6F),
        BlockProperties::of(Block::SnowBlock, "snow_block", "Snow Block")
            .texture(100.0F)
            .strength(0.2F),
        BlockProperties::of(Block::Netherrack, "netherrack", "Netherrack")
            .texture(101.0F)
            .strength(0.4F),
        BlockProperties::of(Block::Glowstone, "glowstone", "Glowstone")
            .texture(102.0F)
            .strength(0.3F)
            .light(15U),
        BlockProperties::of(Block::WhiteWool, "white_wool", "White Wool")
            .texture(103.0F)
            .strength(0.8F),
        BlockProperties::of(Block::RedWool, "red_wool", "Red Wool")
            .texture(104.0F)
            .strength(0.8F),
        BlockProperties::of(Block::BlackWool, "black_wool", "Black Wool")
            .texture(105.0F)
            .strength(0.8F),
        BlockProperties::of(Block::StoneBricks, "stone_bricks", "Stone Bricks")
            .texture(106.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::MossyCobblestone, "mossy_cobblestone", "Mossy Cobblestone")
            .texture(107.0F)
            .strength(2.0F, 6.0F),
        BlockProperties::of(Block::Sandstone, "sandstone", "Sandstone")
            .texture(108.0F, 109.0F, 110.0F)
            .strength(0.8F),
        BlockProperties::of(Block::Pumpkin, "pumpkin", "Pumpkin")
            .texture(111.0F, 112.0F, 111.0F)
            .strength(1.0F),
        BlockProperties::of(Block::Melon, "melon", "Melon")
            .texture(113.0F, 114.0F, 113.0F)
            .strength(1.0F),
        BlockProperties::of(Block::Tnt, "tnt", "TNT")
            .texture(115.0F, 116.0F, 117.0F)
            .instantBreak(),
        BlockProperties::of(Block::Granite, "granite", "Granite")
            .texture(130.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::Diorite, "diorite", "Diorite")
            .texture(131.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::Andesite, "andesite", "Andesite")
            .texture(132.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::CoarseDirt, "coarse_dirt", "Coarse Dirt")
            .texture(133.0F)
            .strength(0.5F)
            .soil(),
        BlockProperties::of(Block::Podzol, "podzol", "Podzol")
            .texture(134.0F, 135.0F, 2.0F)
            .strength(0.5F)
            .soil(),
        BlockProperties::of(Block::RedSand, "red_sand", "Red Sand")
            .texture(136.0F)
            .strength(0.5F)
            .gravity(),
        // Carved cells below y=11 become lava (CaveCarver#carveAtPoint). Rendered
        // as a solid self-lit cube for now; it carries no fluid simulation. The
        // top face uses the animated still strip (20 frames from layer 344) and
        // the sides the animated flow strip (16 frames from layer 364), laid out
        // at the end of the atlas in VulkanRenderer.cpp.
        BlockProperties::of(Block::Lava, "lava", "Lava")
            .texture(344.0F, 364.0F, 364.0F)
            .strength(100.0F)
            .noDrops()
            .light(15U)
            .lightFilter(1U),
        BlockProperties::of(Block::Torch, "torch", "Torch")
            .texture(138.0F)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Torch)
            .noCollision()
            .light(14U)
            .support(BlockSupport::Ground)
            .torch(),
        BlockProperties::of(Block::WallTorchNorth, "wall_torch_north", "Wall Torch")
            .vanillaAlias("wall_torch")
            .texture(138.0F)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Torch)
            .noCollision()
            .light(14U)
            .support(BlockSupport::Wall)
            .torch(),
        BlockProperties::of(Block::WallTorchEast, "wall_torch_east", "Wall Torch")
            .vanillaAlias("wall_torch")
            .texture(138.0F)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Torch)
            .noCollision()
            .light(14U)
            .support(BlockSupport::Wall)
            .torch(),
        BlockProperties::of(Block::WallTorchSouth, "wall_torch_south", "Wall Torch")
            .vanillaAlias("wall_torch")
            .texture(138.0F)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Torch)
            .noCollision()
            .light(14U)
            .support(BlockSupport::Wall)
            .torch(),
        BlockProperties::of(Block::WallTorchWest, "wall_torch_west", "Wall Torch")
            .vanillaAlias("wall_torch")
            .texture(138.0F)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Torch)
            .noCollision()
            .light(14U)
            .support(BlockSupport::Wall)
            .torch(),
        BlockProperties::of(Block::Chest, "chest", "Chest")
            .texture(220.0F, 222.0F, 222.0F)
            .strength(2.5F)
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Chest)
            .horizontalFacing()
            .container(ContainerType::Chest),
        BlockProperties::of(Block::LapisOre, "lapis_ore", "Lapis Lazuli Ore")
            .texture(204.0F)
            .strength(3.0F),
        BlockProperties::of(Block::RedstoneOre, "redstone_ore", "Redstone Ore")
            .texture(205.0F)
            .strength(3.0F),
        BlockProperties::of(Block::EmeraldOre, "emerald_ore", "Emerald Ore")
            .texture(206.0F)
            .strength(3.0F),
        BlockProperties::of(Block::MossyStoneBricks, "mossy_stone_bricks", "Mossy Stone Bricks")
            .texture(207.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::ChiseledStoneBricks, "chiseled_stone_bricks",
                            "Chiseled Stone Bricks")
            .texture(208.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::QuartzBlock, "quartz_block", "Block of Quartz")
            .texture(209.0F, 210.0F, 209.0F)
            .strength(0.8F),
        BlockProperties::of(Block::JungleLog, "jungle_log", "Jungle Log")
            .texture(234.0F, 233.0F, 234.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::JunglePlanks, "jungle_planks", "Jungle Planks")
            .texture(235.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::AcaciaLog, "acacia_log", "Acacia Log")
            .texture(237.0F, 236.0F, 237.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::AcaciaPlanks, "acacia_planks", "Acacia Planks")
            .texture(238.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::DarkOakLog, "dark_oak_log", "Dark Oak Log")
            .texture(240.0F, 239.0F, 240.0F)
            .strength(2.0F)
            .pillar(),
        BlockProperties::of(Block::DarkOakPlanks, "dark_oak_planks", "Dark Oak Planks")
            .texture(241.0F)
            .strength(2.0F, 3.0F),
        BlockProperties::of(Block::SpruceLeaves, "spruce_leaves", "Spruce Leaves")
            .texture(262.0F)
            .leaves(),
        BlockProperties::of(Block::BirchLeaves, "birch_leaves", "Birch Leaves")
            .texture(263.0F)
            .leaves(),
        BlockProperties::of(Block::JungleLeaves, "jungle_leaves", "Jungle Leaves")
            .texture(264.0F)
            .leaves(),
        BlockProperties::of(Block::AcaciaLeaves, "acacia_leaves", "Acacia Leaves")
            .texture(265.0F)
            .leaves(),
        BlockProperties::of(Block::DarkOakLeaves, "dark_oak_leaves", "Dark Oak Leaves")
            .texture(266.0F)
            .leaves(),
        BlockProperties::of(Block::SpruceSapling, "spruce_sapling", "Spruce Sapling")
            .texture(267.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::BirchSapling, "birch_sapling", "Birch Sapling")
            .texture(268.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::JungleSapling, "jungle_sapling", "Jungle Sapling")
            .texture(269.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::AcaciaSapling, "acacia_sapling", "Acacia Sapling")
            .texture(270.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        BlockProperties::of(Block::DarkOakSapling, "dark_oak_sapling", "Dark Oak Sapling")
            .texture(271.0F)
            .instantBreak()
            .cross()
            .support(BlockSupport::Soil),
        // FarmlandBlock: the tilled soil a hoe makes. The top face swaps between
        // the dry and moist textures once the orientation's moisture passes 0;
        // the sides are plain dirt, matching the vanilla model. Breaking it
        // yields dirt (see minedDrops), never farmland itself. Its solid box is
        // 15/16 tall, the vanilla FarmlandBlock.SHAPE.
        BlockProperties::of(Block::Farmland, "farmland", "Farmland")
            .texture(kFarmlandLayer, 2.0F, 2.0F)
            .strength(0.6F)
            .height(15.0F / 16.0F),
        // CropBlock: wheat/carrot/potato share the crossed-plant render, with the
        // stage texture driven by the age stored in the orientation byte. They
        // need farmland below, have no collision of their own, and never drop
        // themselves — minedDrops rolls the species' loot table from the age.
        BlockProperties::of(Block::WheatCrops, "wheat", "Wheat")
            .texture(kWheatStage0Layer)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Crop)
            .noCollision()
            .noDrops()
            .support(BlockSupport::Farmland),
        BlockProperties::of(Block::Carrots, "carrots", "Carrots")
            .texture(kCarrotStage0Layer)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Crop)
            .noCollision()
            .noDrops()
            .support(BlockSupport::Farmland),
        BlockProperties::of(Block::Potatoes, "potatoes", "Potatoes")
            .texture(kPotatoStage0Layer)
            .instantBreak()
            .renderLayer(BlockRenderLayer::Cutout)
            .model(BlockModel::Crop)
            .noCollision()
            .noDrops()
            .support(BlockSupport::Farmland),
        // Decorative stone variants. Each polished stone matches its parent's
        // hardness; smooth stone is the furnace product of stone. The texture
        // layers 242-245 occupy four of newContentTextures' placeholder slots.
        BlockProperties::of(Block::PolishedGranite, "polished_granite", "Polished Granite")
            .texture(242.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::PolishedDiorite, "polished_diorite", "Polished Diorite")
            .texture(243.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::PolishedAndesite, "polished_andesite", "Polished Andesite")
            .texture(244.0F)
            .strength(1.5F, 6.0F),
        BlockProperties::of(Block::SmoothStone, "smooth_stone", "Smooth Stone")
            .texture(245.0F)
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
        if (static_cast<std::size_t>(definition.block) != index) return false;
        if (definition.identifier.space != kNamespace || definition.identifier.path.empty()) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (kBlockRegistry[other].identifier == definition.identifier) return false;
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
        if (definition.identifier.matches(text)) return definition.block;
    }
    for (const auto& definition : kBlockRegistry) {
        if (definition.vanilla.matches(text)) return definition.block;
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
    return isRenderable(block) &&
           blockDefinition(block).renderLayer == BlockRenderLayer::Opaque;
}

[[nodiscard]] constexpr std::uint8_t skyLightOpacity(Block block) {
    if (isOpaque(block)) return 15U;
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
    if (isOpaque(block)) return 15;
    if (block == Block::Water || block == Block::Lava) return 3;
    return 0;
}

[[nodiscard]] constexpr bool hasCollision(Block block) {
    return blockDefinition(block).collision;
}

[[nodiscard]] constexpr std::uint8_t emittedLight(Block block) {
    return blockDefinition(block).light;
}

[[nodiscard]] constexpr bool isTorch(Block block) {
    return blockDefinition(block).torch;
}

// Wall torches sit flush against their wall, the way 1.16.1's WallTorchBlock
// AABB runs all the way to the block face (a north-facing torch spans z 11..16
// of 16). This is the inset of the model's root from the cell centre toward the
// wall; the mesh and the selection box share it so clicking matches the look.
inline constexpr float kWallTorchInset = 0.5F;

[[nodiscard]] constexpr bool isLog(Block block) {
    return blockDefinition(block).pillar;
}

[[nodiscard]] constexpr bool isLeaves(Block block) {
    return blockDefinition(block).leaves;
}

// Java's LeavesBlock.PERSISTENT. Leaves carry no facing, so the per-block state
// byte that a pillar uses for its axis records this property instead: leaves a
// player placed stay put, leaves that grew with a tree decay once the trunk
// that fed them is gone.
inline constexpr BlockOrientation kPersistentLeavesState = BlockOrientation::East;

[[nodiscard]] constexpr bool leavesArePersistent(BlockOrientation state) {
    return state == kPersistentLeavesState;
}

// LeavesBlock.DISTANCE only reaches 7, so leaves further than six steps through
// other leaves from any log are the ones that decay.
inline constexpr int kMaximumLeafSupportDistance = 6;

// The sapling a wood set's leaves roll on their loot table.
[[nodiscard]] constexpr Block saplingForLeaves(Block leaves) {
    switch (leaves) {
    case Block::SpruceLeaves: return Block::SpruceSapling;
    case Block::BirchLeaves: return Block::BirchSapling;
    case Block::JungleLeaves: return Block::JungleSapling;
    case Block::AcaciaLeaves: return Block::AcaciaSapling;
    case Block::DarkOakLeaves: return Block::DarkOakSapling;
    default: return Block::OakSapling;
    }
}

[[nodiscard]] constexpr bool isReplaceable(Block block) {
    return blockDefinition(block).replaceable;
}

[[nodiscard]] constexpr bool isFluid(Block block) {
    return block == Block::Water;
}

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

// Whether the block darkens a smooth-lighting AO corner (vanilla 1.16.1
// AbstractBlock#getAmbientOcclusionLightLevel: a full cube whose material is
// opaque returns 0.2, everything else 1.0). isFullCube alone is wrong: leaves,
// glass and glowstone are cube-shaped but their vanilla materials are not
// opaque, so they must not darken corners.
[[nodiscard]] constexpr bool aoOccludes(Block block) {
    return isFullCube(block) && !isLeaves(block) && isOpaque(block) &&
           block != Block::Glowstone;
}

// Flowing water washes away decoration blocks that do not block motion. Crops
// are no-collision too, but vanilla's crops survive water (their material is
// not REPLACEABLE_PLANT), so they are carved out of the fluid-destroyed set.
[[nodiscard]] constexpr bool isCrop(Block block) {
    return block == Block::WheatCrops || block == Block::Carrots ||
        block == Block::Potatoes;
}
[[nodiscard]] constexpr bool isDestroyedByFluid(Block block) {
    return isRenderable(block) && !isFluid(block) && !isCrop(block) &&
           !blockDefinition(block).collision;
}

[[nodiscard]] constexpr BlockSupport blockSupport(Block block) {
    return blockDefinition(block).support;
}

// BushBlock#mayPlaceOn in Java 1.16.1.
[[nodiscard]] constexpr bool isSoil(Block block) {
    return blockDefinition(block).soil;
}

[[nodiscard]] constexpr bool isFarmland(Block block) {
    return block == Block::Farmland;
}

// Crops and farmland reuse the per-cell orientation byte as their state slot,
// the same way leaves record their persistent flag in it. A crop stores its age
// 0-7 (vanilla's CropsBlock.AGE) and farmland its moisture 0-7 (FarmlandBlock.
// MOISTURE). Age 0 aliases the North orientation, so a freshly placed crop and
// freshly tilled farmland read as state 0 without an explicit write.
[[nodiscard]] constexpr int cropAge(BlockOrientation state) {
    return static_cast<int>(state) & 0x7;
}
[[nodiscard]] constexpr BlockOrientation cropOrientation(int age) {
    return static_cast<BlockOrientation>(age < 0 ? 0 : (age > 7 ? 7 : age));
}
[[nodiscard]] constexpr int farmlandMoisture(BlockOrientation state) {
    return static_cast<int>(state) & 0x7;
}
[[nodiscard]] constexpr BlockOrientation farmlandOrientation(int moisture) {
    return static_cast<BlockOrientation>(moisture < 0 ? 0 : (moisture > 7 ? 7 : moisture));
}

// The stage-texture index for a crop age. Wheat has one texture per age; the
// carrots/potatoes blockstate maps their eight ages onto four stage textures
// (0-1 -> stage 0, 2-3 -> stage 1, 4-6 -> stage 2, 7 -> stage 3).
[[nodiscard]] constexpr int cropStageIndex(Block block, int age) {
    if (block == Block::WheatCrops) {
        return age < 0 ? 0 : (age > 7 ? 7 : age);
    }
    if (age <= 1) return 0;
    if (age <= 3) return 1;
    if (age <= 6) return 2;
    return 3;
}

// The texture-array layer the mesher paints a crop with at the given age.
[[nodiscard]] constexpr float cropTextureLayer(Block block, int age) {
    const int stage = cropStageIndex(block, age);
    switch (block) {
    case Block::WheatCrops:
        return kWheatStage0Layer + static_cast<float>(stage);
    case Block::Carrots:
        return kCarrotStage0Layer + static_cast<float>(stage);
    case Block::Potatoes:
        return kPotatoStage0Layer + static_cast<float>(stage);
    default:
        return 0.0F;
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
// facing property. The wall that carries it is on the opposite side.
[[nodiscard]] constexpr BlockOrientation wallTorchFacing(Block block) {
    switch (block) {
    case Block::WallTorchNorth:
        return BlockOrientation::North;
    case Block::WallTorchEast:
        return BlockOrientation::East;
    case Block::WallTorchSouth:
        return BlockOrientation::South;
    case Block::WallTorchWest:
        return BlockOrientation::West;
    default:
        return BlockOrientation::Up;
    }
}

[[nodiscard]] constexpr BlockOrientation wallTorchSupportSide(Block block) {
    return oppositeOrientation(wallTorchFacing(block));
}

// The wall torch leaning toward the given horizontal direction.
[[nodiscard]] constexpr Block wallTorchWithFacing(BlockOrientation facing) {
    switch (facing) {
    case BlockOrientation::North:
        return Block::WallTorchNorth;
    case BlockOrientation::East:
        return Block::WallTorchEast;
    case BlockOrientation::South:
        return Block::WallTorchSouth;
    case BlockOrientation::West:
        return Block::WallTorchWest;
    default:
        return Block::Torch;
    }
}

[[nodiscard]] constexpr bool isSelectable(Block block) {
    // The normal interaction ray ignores fluids, but still needs to hit
    // non-colliding cross models such as grass and flowers.
    return isRenderable(block) && !isFluid(block);
}

[[nodiscard]] constexpr BlockTextureLayers textureLayers(Block block) {
    return blockDefinition(block).textures;
}

[[nodiscard]] constexpr const char* blockName(Block block) {
    return blockDefinition(block).displayName;
}

} // namespace mc::world
