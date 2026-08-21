#pragma once

#include "core/Identifier.hpp"
#include "gameplay/ItemUse.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::gameplay {

// Forward declaration — EntityType lives in entities/ which includes us through
// Inventory.hpp, so we can only keep a pointer (or a function that returns one).
namespace entities {
class EntityType;
} // namespace entities

using core::Identifier;
using core::kNamespace;
using core::kVanillaNamespace;

// The tool a stack is wielded as, and the material it is made from. Together
// they carry Java 1.16.1's ToolMaterials: a tier's harvest level and mining
// speed, plus the attack damage/speed each tool type adds. Material and type
// are both stored on the item so the mining system can read them from the
// registry table instead of a parallel switch (see toolAttributes).
enum class ToolType : std::uint8_t {
    None,
    Sword,
    Pickaxe,
    Axe,
    Shovel,
    Hoe,
};

enum class ToolTier : std::uint8_t {
    None,
    Wood,
    Stone,
    Iron,
    Gold,
    Diamond,
};

// The creative-inventory tab an item or block is filed under. Lives here (rather
// than in Inventory.hpp) because it is metadata every registered Item carries.
enum class CreativeCategory : std::uint8_t {
    BuildingBlocks,
    Decoration,
    Functional,
    Materials,
    Food,
    Tools,
    SpawnEggs,
    Count,
};

// What eating one item restores, following Java 1.16.1 FoodComponent.
struct FoodValue final {
    int foodLevel = 0;
    float saturationModifier = 0.0F;
};

// How the renderer paints an item's icon into the texture array. Most items are
// a single sprite; the pig spawn egg is composited from two tinted textures.
enum class TextureBuild : std::uint8_t {
    Simple,
    SpawnEggComposite,
};

// The kind of block item a block stack is wielded as, mirroring 1.16.1's
// BlockItem subclasses. None for ordinary items, Plain for a block wielded
// directly, StandingAndWall for the torch's two-variant item, and Leaves for
// the LeavesBlockItem that marks hand-placed leaves as persistent.
enum class BlockItemKind : std::uint8_t {
    None,
    Plain,
    StandingAndWall,
    Leaves,
};

// C++ representation of Java's textual description id. Keeping the three
// components separate makes the registry constexpr and lets Language perform a
// heterogeneous hash lookup without constructing `item.minecraft.apple` on
// every draw. Text encoding remains available for cold paths such as command
// suggestions and diagnostics.
enum class DescriptionType : std::uint8_t {
    Item,
    Block,
};

struct DescriptionId final {
    DescriptionType type = DescriptionType::Item;
    Identifier source{};

    [[nodiscard]] constexpr bool empty() const { return source.empty(); }
    [[nodiscard]] constexpr std::string_view prefix() const {
        return type == DescriptionType::Block ? "block" : "item";
    }
};

[[nodiscard]] inline std::string encodeDescriptionId(const DescriptionId& id) {
    if (id.empty()) return {};
    std::string result{id.prefix()};
    result.reserve(result.size() + id.source.space.size() + id.source.path.size() + 2U);
    result.push_back('.');
    result.append(id.source.space);
    result.push_back('.');
    result.append(id.source.path);
    return result;
}

// ItemUseAction, ItemUseResult and the useOn function type arrive through
// ItemUse.hpp above; world::World and world::PlacementContext are only passed
// by reference there.

// One registered item, declared as a single chained expression. An Item instance
// owns everything the registration behaviours need: its identity, creative
// category, texture/model resource and the stable source of its description id.
// Localized display text belongs to language resources, never to this registry.
// Instances live as named constexpr globals in namespace `items` below, and a
// stack refers to one by pointer.
class Item {
  public:
    constexpr Item() = default;

    // (1) Register: `path` is the `rebedrock:` key, and by default the same name
    // is exposed as a `minecraft:` alias so vanilla-style ids still resolve on
    // give commands. The texture resource defaults to item/<path>.png.
    [[nodiscard]] static constexpr Item of(std::string_view path) {
        Item item;
        item.identifier = {kNamespace, path};
        item.vanillaAlias = {kVanillaNamespace, path};
        item.textureName = path;
        return item;
    }

    // (3) Resource: overrides the texture base name when it differs from the
    // registry path (none currently do, but the hook keeps resource config here).
    [[nodiscard]] constexpr Item texture(std::string_view name) const {
        Item copy = *this;
        copy.textureName = name;
        return copy;
    }
    // Marks the icon as the two-layer spawn-egg composite the renderer tints.
    [[nodiscard]] constexpr Item spawnEggTexture() const {
        Item copy = *this;
        copy.textureBuild = TextureBuild::SpawnEggComposite;
        return copy;
    }

    // (2) Creative inventory: the tab this item appears under.
    [[nodiscard]] constexpr Item category(CreativeCategory value) const {
        Item copy = *this;
        copy.creativeCategory = value;
        return copy;
    }

    [[nodiscard]] constexpr Item stackSize(std::uint8_t size) const {
        Item copy = *this;
        copy.maximumStackSize = size;
        return copy;
    }
    // Tools and buckets of liquid go one to a slot.
    [[nodiscard]] constexpr Item single() const { return stackSize(1U); }

    // Marks the item as a tool of the given role and material.
    [[nodiscard]] constexpr Item tool(ToolType type, ToolTier tier) const {
        Item copy = *this;
        copy.toolType = type;
        copy.toolTier = tier;
        return copy;
    }

    // Declares what eating the item restores.
    [[nodiscard]] constexpr Item food(FoodValue value) const {
        Item copy = *this;
        copy.nutrition = value;
        return copy;
    }

    // Wires the item's right-click behaviour (Item#useOn). Ordinary items that
    // are not a subclass supply it here; subclasses set it in their constructor.
    [[nodiscard]] constexpr Item useAction(ItemUseFn fn) const {
        Item copy = *this;
        copy.useOn = fn;
        return copy;
    }

    // Original content with no vanilla counterpart: drop the `minecraft:` alias.
    [[nodiscard]] constexpr Item custom() const {
        Item copy = *this;
        copy.vanillaAlias = {};
        return copy;
    }
    // An item whose vanilla alias is filed under a different name.
    [[nodiscard]] constexpr Item vanillaAliasOf(std::string_view path) const {
        Item copy = *this;
        copy.vanillaAlias = {kVanillaNamespace, path};
        return copy;
    }

    // Item#getDescriptionId, following Java's registry-derived convention.
    // Vanilla-backed content deliberately uses its minecraft alias so an item
    // registered internally as rebedrock:apple resolves item.minecraft.apple.
    // Original content instead remains in the rebedrock language namespace.
    [[nodiscard]] constexpr Identifier translationIdentifier() const {
        return vanillaAlias.empty() ? identifier : vanillaAlias;
    }
    [[nodiscard]] constexpr DescriptionId descriptionId() const {
        return {descriptionType, translationIdentifier()};
    }

    // The registry key, always in this project's namespace.
    Identifier identifier{};
    // The vanilla id this one aliases (empty for original content), accepted by
    // give commands and identifier lookups.
    Identifier vanillaAlias{};
    // Java BlockItem delegates its description id to the block; all other
    // items use the ordinary item prefix.
    DescriptionType descriptionType = DescriptionType::Item;
    // Base name of the item/<name>.png sprite (unused when composited).
    std::string_view textureName{};
    TextureBuild textureBuild = TextureBuild::Simple;
    // None for ordinary items; a block item's kind identifies its subclass so
    // asBlockItem can down-cast without RTTI (same marker trick as textureBuild).
    BlockItemKind blockItemKind = BlockItemKind::None;
    CreativeCategory creativeCategory = CreativeCategory::Materials;
    std::uint8_t maximumStackSize = 64U;
    // The tool role and material for tools; ToolType::None for everything else.
    ToolType toolType = ToolType::None;
    ToolTier toolTier = ToolTier::None;
    FoodValue nutrition{};
    // Item#useOn: what right-clicking with this item does. Null for an item the
    // interaction system does not route through the item (it places nothing).
    ItemUseFn useOn = nullptr;
};

// SpawnEggItem (1.16.1): an Item that knows which entity it spawns. Storing the
// EntityType supplier here (rather than in a parallel mapping) lets the renderer
// tint each egg with its species' colours and lets the interaction system spawn
// the right creature — all without a hardcoded entity list. The supplier is a
// function pointer because PigEntity::type() and friends return a static-local
// reference; the pointer is known at link time so the instance stays constexpr.
class SpawnEggItem : public Item {
  public:
    using EntitySupplier = const entities::EntityType& (*)();

    constexpr SpawnEggItem(std::string_view path, EntitySupplier supplier)
        : entitySupplier_(supplier) {
        identifier = {kNamespace, path};
        vanillaAlias = {kVanillaNamespace, path};
        textureName = path;
        creativeCategory = CreativeCategory::SpawnEggs;
        textureBuild = TextureBuild::SpawnEggComposite;
    }

    [[nodiscard]] const entities::EntityType& entityType() const {
        return entitySupplier_();
    }

  private:
    EntitySupplier entitySupplier_ = nullptr;
};

// Returns non-null when `item` is a spawn egg, so callers can reach entityType().
[[nodiscard]] inline const SpawnEggItem* asSpawnEgg(const Item* item) {
    if (item != nullptr && item->textureBuild == TextureBuild::SpawnEggComposite) {
        return static_cast<const SpawnEggItem*>(item);
    }
    return nullptr;
}

// BlockItem (1.16.1): the Item a block is wielded as. Where vanilla writes
// `new BlockItem(block, props)` into its Items registry, a stack here points at
// the block's own BlockItem. Identity, stack size and fallback name all come
// from the block's registry entry, so a block and its item always agree. The
// constructor is constexpr, so block items can live in a static table like every
// other registered item.
class BlockItem : public Item {
  public:
    constexpr BlockItem() = default;

    constexpr BlockItem(world::Block block) : block_(world::blockId(block)) {
        const auto& definition = world::blockDefinition(block);
        identifier = definition.identifier;
        vanillaAlias = definition.vanilla;
        textureName = definition.identifier.path;
        maximumStackSize = definition.maximumStackSize;
        descriptionType = DescriptionType::Block;
        blockItemKind = BlockItemKind::Plain;
    }

    // The block this item places, held as a dense BlockId (the DOD "holder = id"
    // rule) rather than the Block enum: a block item references block identity by
    // id, and block() converts back for the callers that still speak in Block.
    [[nodiscard]] constexpr world::Block block() const { return world::blockFromId(block_); }
    [[nodiscard]] constexpr world::BlockId blockId() const { return block_; }

  private:
    world::BlockId block_ = world::blockId(world::Block::Air);
};

// StandingAndWallBlockItem (1.16.1): a block item that places one of two blocks
// — the wall variant wins on a side face, the standing variant otherwise. The
// torch is the registered instance; its placement policy lives in
// world::standingAndWallPlacement, which reads the clicked face.
class StandingAndWallBlockItem : public BlockItem {
  public:
    constexpr StandingAndWallBlockItem(world::Block standing, world::Block wall)
        : BlockItem(standing), wallBlock_(world::blockId(wall)) {
        blockItemKind = BlockItemKind::StandingAndWall;
    }

    [[nodiscard]] constexpr world::Block wallBlock() const {
        return world::blockFromId(wallBlock_);
    }

  private:
    world::BlockId wallBlock_ = world::blockId(world::Block::Air);
};

// Returns non-null when `item` is a block item, so callers can reach its block().
[[nodiscard]] inline const BlockItem* asBlockItem(const Item* item) {
    if (item != nullptr && item->blockItemKind != BlockItemKind::None) {
        return static_cast<const BlockItem*>(item);
    }
    return nullptr;
}

[[nodiscard]] inline const StandingAndWallBlockItem* asStandingAndWallBlockItem(
    const Item* item) {
    if (item != nullptr && item->blockItemKind == BlockItemKind::StandingAndWall) {
        return static_cast<const StandingAndWallBlockItem*>(item);
    }
    return nullptr;
}

// LeavesBlockItem (1.16.1): the block item leaves are wielded as. Its placement
// marks the leaves persistent so hand-placed leaves never decay; the flag is the
// block's own PERSISTENT property, so the class carries the behaviour and
// BlockState::withPersistent records it.
class LeavesBlockItem : public BlockItem {
  public:
    constexpr LeavesBlockItem() = default;

    constexpr LeavesBlockItem(world::Block block) : BlockItem(block) {
        blockItemKind = BlockItemKind::Leaves;
    }
};

[[nodiscard]] inline const LeavesBlockItem* asLeavesBlockItem(const Item* item) {
    if (item != nullptr && item->blockItemKind == BlockItemKind::Leaves) {
        return static_cast<const LeavesBlockItem*>(item);
    }
    return nullptr;
}

// The one BlockItem a block is wielded as, mirroring the entry every block gets
// in vanilla's Items registry. The pointer is stable, so two stacks of the same
// block always point at the same item. The torch is a StandingAndWallBlockItem,
// leaves a LeavesBlockItem, and every other block a plain BlockItem built from
// its registry entry.
[[nodiscard]] inline const BlockItem* blockItemFor(world::Block block) {
    static const StandingAndWallBlockItem torch{world::Block::Torch,
                                                world::Block::WallTorch};
    if (block == world::Block::Torch) return &torch;
    if (!world::isValidBlock(block)) return nullptr;
    if (world::isLeaves(block)) {
        static const std::array<LeavesBlockItem,
                                static_cast<std::size_t>(world::Block::Count)> leavesItems =
            [] {
                std::array<LeavesBlockItem,
                           static_cast<std::size_t>(world::Block::Count)> result{};
                for (std::size_t index = 0; index < result.size(); ++index) {
                    result[index] = LeavesBlockItem{static_cast<world::Block>(index)};
                }
                return result;
            }();
        return &leavesItems[static_cast<std::size_t>(block)];
    }
    static const std::array<BlockItem, static_cast<std::size_t>(world::Block::Count)> items =
        [] {
            std::array<BlockItem, static_cast<std::size_t>(world::Block::Count)> result{};
            for (std::size_t index = 0; index < result.size(); ++index) {
                result[index] = BlockItem{static_cast<world::Block>(index)};
            }
            return result;
        }();
    return &items[static_cast<std::size_t>(block)];
}

// A runtime-extensible slot for items that cannot be listed in the constexpr
// kItemRegistry (spawn eggs, whose constructors reference entity headers that
// live above us in the include graph). Populated by SpawnEggItems.hpp.
inline std::vector<const Item*>& extraItemRegistry() {
    static std::vector<const Item*> registry;
    return registry;
}

// The named item instances. Each is a complete registration: identity, creative
// tab and texture resource. Its localized name is resolved from the derived
// description id. Code refers to one by address, e.g. &items::Diamond.
namespace items {

// Materials
inline constexpr Item Bucket =
    Item::of("bucket").category(CreativeCategory::Materials).stackSize(16U);
inline constexpr Item WaterBucket = Item::of("water_bucket")
                                        .category(CreativeCategory::Materials)
                                        .single();
inline constexpr Item LavaBucket = Item::of("lava_bucket")
                                       .category(CreativeCategory::Materials)
                                       .single();
inline constexpr Item Coal =
    Item::of("coal").category(CreativeCategory::Materials);
inline constexpr Item IronIngot =
    Item::of("iron_ingot").category(CreativeCategory::Materials);
inline constexpr Item GoldIngot =
    Item::of("gold_ingot").category(CreativeCategory::Materials);
inline constexpr Item Diamond =
    Item::of("diamond").category(CreativeCategory::Materials);
inline constexpr Item Emerald =
    Item::of("emerald").category(CreativeCategory::Materials);
inline constexpr Item Stick =
    Item::of("stick").category(CreativeCategory::Materials);
inline constexpr Item Flint =
    Item::of("flint").category(CreativeCategory::Materials);
inline constexpr Item Feather =
    Item::of("feather").category(CreativeCategory::Materials);
inline constexpr Item String =
    Item::of("string").category(CreativeCategory::Materials);
inline constexpr Item Leather =
    Item::of("leather").category(CreativeCategory::Materials);
inline constexpr Item Sugar =
    Item::of("sugar").category(CreativeCategory::Materials);
inline constexpr Item Egg =
    Item::of("egg").category(CreativeCategory::Materials).stackSize(16U);
inline constexpr Item Bone =
    Item::of("bone").category(CreativeCategory::Materials);
inline constexpr Item Paper =
    Item::of("paper").category(CreativeCategory::Materials);
inline constexpr Item Book =
    Item::of("book").category(CreativeCategory::Materials);
// WheatSeedsItem: right-clicking farmland plants the wheat crop. The behaviour
// is dispatched by item identity in itemUseOn (ItemPlacement.cpp), the way the
// buckets are, so the constexpr registrations stay free of function pointers.
inline constexpr Item WheatSeeds = Item::of("wheat_seeds")
                                       .category(CreativeCategory::Materials);
inline constexpr Item Wheat =
    Item::of("wheat").category(CreativeCategory::Materials);

// Food
inline constexpr Item Apple = Item::of("apple")
                                  .category(CreativeCategory::Food)
                                  .food({4, 0.3F});
inline constexpr Item Bread = Item::of("bread")
                                  .category(CreativeCategory::Food)
                                  .food({5, 0.6F});
inline constexpr Item Porkchop = Item::of("porkchop")
                                     .category(CreativeCategory::Food)
                                     .food({3, 0.3F});
inline constexpr Item CookedPorkchop = Item::of("cooked_porkchop")
                                           .category(CreativeCategory::Food)
                                           .food({8, 0.8F});
// Raw beef: the cow's meat drop. Vanilla food value 3 hunger / 0.3 saturation,
// identical to raw porkchop.
inline constexpr Item Beef = Item::of("beef")
                                 .category(CreativeCategory::Food)
                                 .food({3, 0.3F});
// Carrot and potato are both food (1.16.1 FoodComponent) and the seed of their
// own crop — a held carrot/potato plants itself on farmland, like the vanilla
// items whose useOn is a SeedsItem subclass. Planting is dispatched by item
// identity in itemUseOn; right-clicking empty ground still eats them.
inline constexpr Item Carrot = Item::of("carrot")
                                   .category(CreativeCategory::Food)
                                   .food({3, 0.6F});
inline constexpr Item Potato = Item::of("potato")
                                   .category(CreativeCategory::Food)
                                   .food({1, 0.3F});

// Tools: pickaxes, axes, shovels, hoes, swords, each single-stacking.
inline constexpr Item WoodenPickaxe = Item::of("wooden_pickaxe")
                                          .category(CreativeCategory::Tools)
                                          .single()
                                          .tool(ToolType::Pickaxe, ToolTier::Wood);
inline constexpr Item StonePickaxe = Item::of("stone_pickaxe")
                                         .category(CreativeCategory::Tools)
                                         .single()
                                         .tool(ToolType::Pickaxe, ToolTier::Stone);
inline constexpr Item IronPickaxe = Item::of("iron_pickaxe")
                                        .category(CreativeCategory::Tools)
                                        .single()
                                        .tool(ToolType::Pickaxe, ToolTier::Iron);
inline constexpr Item DiamondPickaxe = Item::of("diamond_pickaxe")
                                           .category(CreativeCategory::Tools)
                                           .single()
                                           .tool(ToolType::Pickaxe, ToolTier::Diamond);
inline constexpr Item GoldPickaxe = Item::of("golden_pickaxe")
                                        .category(CreativeCategory::Tools)
                                        .single()
                                        .tool(ToolType::Pickaxe, ToolTier::Gold);
inline constexpr Item WoodenAxe = Item::of("wooden_axe")
                                      .category(CreativeCategory::Tools)
                                      .single()
                                      .tool(ToolType::Axe, ToolTier::Wood);
inline constexpr Item StoneAxe = Item::of("stone_axe")
                                     .category(CreativeCategory::Tools)
                                     .single()
                                     .tool(ToolType::Axe, ToolTier::Stone);
inline constexpr Item IronAxe = Item::of("iron_axe")
                                    .category(CreativeCategory::Tools)
                                    .single()
                                    .tool(ToolType::Axe, ToolTier::Iron);
inline constexpr Item DiamondAxe = Item::of("diamond_axe")
                                       .category(CreativeCategory::Tools)
                                       .single()
                                       .tool(ToolType::Axe, ToolTier::Diamond);
inline constexpr Item GoldAxe = Item::of("golden_axe")
                                    .category(CreativeCategory::Tools)
                                    .single()
                                    .tool(ToolType::Axe, ToolTier::Gold);
inline constexpr Item WoodenShovel = Item::of("wooden_shovel")
                                         .category(CreativeCategory::Tools)
                                         .single()
                                         .tool(ToolType::Shovel, ToolTier::Wood);
inline constexpr Item StoneShovel = Item::of("stone_shovel")
                                        .category(CreativeCategory::Tools)
                                        .single()
                                        .tool(ToolType::Shovel, ToolTier::Stone);
inline constexpr Item IronShovel = Item::of("iron_shovel")
                                       .category(CreativeCategory::Tools)
                                       .single()
                                       .tool(ToolType::Shovel, ToolTier::Iron);
inline constexpr Item DiamondShovel = Item::of("diamond_shovel")
                                          .category(CreativeCategory::Tools)
                                          .single()
                                          .tool(ToolType::Shovel, ToolTier::Diamond);
inline constexpr Item GoldShovel = Item::of("golden_shovel")
                                       .category(CreativeCategory::Tools)
                                       .single()
                                       .tool(ToolType::Shovel, ToolTier::Gold);
// HoeItem#useOn: right-clicking dirt-family blocks tills farmland (or re-tills
// coarse dirt into dirt). The behaviour is dispatched by the item's ToolType in
// itemUseOn (ItemPlacement.cpp); the tool role and tier only set the
// mining-speed table here.
inline constexpr Item WoodenHoe = Item::of("wooden_hoe")
                                      .category(CreativeCategory::Tools)
                                      .single()
                                      .tool(ToolType::Hoe, ToolTier::Wood);
inline constexpr Item StoneHoe = Item::of("stone_hoe")
                                     .category(CreativeCategory::Tools)
                                     .single()
                                     .tool(ToolType::Hoe, ToolTier::Stone);
inline constexpr Item IronHoe = Item::of("iron_hoe")
                                    .category(CreativeCategory::Tools)
                                    .single()
                                    .tool(ToolType::Hoe, ToolTier::Iron);
inline constexpr Item DiamondHoe = Item::of("diamond_hoe")
                                       .category(CreativeCategory::Tools)
                                       .single()
                                       .tool(ToolType::Hoe, ToolTier::Diamond);
inline constexpr Item GoldHoe = Item::of("golden_hoe")
                                    .category(CreativeCategory::Tools)
                                    .single()
                                    .tool(ToolType::Hoe, ToolTier::Gold);
inline constexpr Item WoodenSword = Item::of("wooden_sword")
                                        .category(CreativeCategory::Tools)
                                        .single()
                                        .tool(ToolType::Sword, ToolTier::Wood);
inline constexpr Item StoneSword = Item::of("stone_sword")
                                       .category(CreativeCategory::Tools)
                                       .single()
                                       .tool(ToolType::Sword, ToolTier::Stone);
inline constexpr Item IronSword = Item::of("iron_sword")
                                      .category(CreativeCategory::Tools)
                                      .single()
                                      .tool(ToolType::Sword, ToolTier::Iron);
inline constexpr Item DiamondSword = Item::of("diamond_sword")
                                         .category(CreativeCategory::Tools)
                                         .single()
                                         .tool(ToolType::Sword, ToolTier::Diamond);
inline constexpr Item GoldSword = Item::of("golden_sword")
                                      .category(CreativeCategory::Tools)
                                      .single()
                                      .tool(ToolType::Sword, ToolTier::Gold);

// Spawn eggs are defined in SpawnEggItems.hpp (they need entity headers that
// live above Item.hpp in the include graph). See kSpawnEggItems there.

} // namespace items

// The item registry: every registered item whose definition can live here.
// Spawn eggs are listed separately in kSpawnEggItems (SpawnEggItems.hpp) because
// their constructors need entity headers that sit above us in the include graph.
// The order sets both the creative-catalog order within each tab and the item
// texture-array layout the renderer appends. Grouped materials / food / tools.
inline constexpr std::array<const Item*, 52> kItemRegistry{
    &items::Bucket,     &items::WaterBucket, &items::LavaBucket, &items::Coal,
    &items::IronIngot,
    &items::GoldIngot,  &items::Diamond,     &items::Emerald,    &items::Stick,
    &items::Flint,      &items::Feather,     &items::String,     &items::Leather,
    &items::Sugar,      &items::Egg,         &items::Bone,       &items::Paper,
    &items::Book,       &items::WheatSeeds,  &items::Wheat,
    &items::Apple,      &items::Bread,       &items::Porkchop,   &items::CookedPorkchop,
    &items::Beef,
    &items::Carrot,     &items::Potato,
    &items::WoodenPickaxe,  &items::StonePickaxe,  &items::IronPickaxe,
    &items::DiamondPickaxe, &items::GoldPickaxe,
    &items::WoodenAxe,      &items::StoneAxe,      &items::IronAxe,
    &items::DiamondAxe,     &items::GoldAxe,
    &items::WoodenShovel,   &items::StoneShovel,   &items::IronShovel,
    &items::DiamondShovel,  &items::GoldShovel,
    &items::WoodenHoe,      &items::StoneHoe,      &items::IronHoe,
    &items::DiamondHoe,     &items::GoldHoe,
    &items::WoodenSword,    &items::StoneSword,    &items::IronSword,
    &items::DiamondSword,   &items::GoldSword,
};

// The registry is well formed when every entry is in this project's namespace,
// has a non-empty path, and no two entries share an identifier.
constexpr bool itemRegistryIsWellFormed() {
    for (std::size_t index = 0; index < kItemRegistry.size(); ++index) {
        const Item& item = *kItemRegistry[index];
        if (item.identifier.space != kNamespace || item.identifier.path.empty()) return false;
        for (std::size_t other = 0; other < index; ++other) {
            if (kItemRegistry[other]->identifier == item.identifier) return false;
        }
    }
    return true;
}
static_assert(itemRegistryIsWellFormed(),
              "kItemRegistry entries must be namespaced and uniquely identified");

// itemFromIdentifier — resolving a registry key to its item — now lives in
// gameplay/ItemRegistry.hpp, where it is a view over the runtime ItemRegistry
// (the single item-identity source) rather than a hand-rolled scan of the
// tables here. Item.hpp keeps only the constexpr definitions; the registry that
// hands out ItemIds and resolves names is built from them one layer up.

// The harvest and combat parameters Java 1.16.1 assigns to one tool. The
// project does not consume durability yet, but the numbers are kept so the
// registry answers the same values 1.16.1 does.
struct ToolAttributes final {
    float miningSpeed = 1.0F;
    std::uint8_t harvestLevel = 0;
    float attackDamage = 1.0F;
    float attackSpeed = 4.0F;
    std::uint16_t durability = 0;
};

// The position of a tool material in the per-tier tables below.
[[nodiscard]] constexpr std::size_t toolTierIndex(ToolTier tier) {
    switch (tier) {
    case ToolTier::Wood: return 0;
    case ToolTier::Stone: return 1;
    case ToolTier::Iron: return 2;
    case ToolTier::Gold: return 3;
    case ToolTier::Diamond: return 4;
    case ToolTier::None: return 0;
    }
    return 0;
}

// Java 1.16.1 ToolMaterial + SwordItem/PickaxeItem/... constructors: per-tier
// mining speed and harvest level, plus the attack damage and speed each tool
// type carries. Material order below is Wood, Stone, Iron, Gold, Diamond.
[[nodiscard]] constexpr ToolAttributes toolAttributes(ToolType type, ToolTier tier) {
    constexpr std::array<float, 5> kMiningSpeed{2.0F, 4.0F, 6.0F, 12.0F, 8.0F};
    constexpr std::array<std::uint8_t, 5> kHarvestLevel{0, 1, 2, 0, 3};
    constexpr std::array<std::uint16_t, 5> kDurability{59, 131, 250, 32, 1561};
    const std::size_t material = toolTierIndex(tier);
    switch (type) {
    case ToolType::Sword: {
        constexpr std::array<float, 5> kDamage{4.0F, 5.0F, 6.0F, 4.0F, 7.0F};
        return {1.0F, kHarvestLevel[material], kDamage[material], 1.6F,
                kDurability[material]};
    }
    case ToolType::Pickaxe: {
        constexpr std::array<float, 5> kDamage{2.0F, 3.0F, 4.0F, 2.0F, 5.0F};
        return {kMiningSpeed[material], kHarvestLevel[material], kDamage[material], 1.2F,
                kDurability[material]};
    }
    case ToolType::Axe: {
        constexpr std::array<float, 5> kDamage{7.0F, 9.0F, 9.0F, 7.0F, 9.0F};
        constexpr std::array<float, 5> kSpeed{0.8F, 0.8F, 0.9F, 1.0F, 1.0F};
        return {kMiningSpeed[material], kHarvestLevel[material], kDamage[material],
                kSpeed[material], kDurability[material]};
    }
    case ToolType::Shovel:
        return {kMiningSpeed[material], kHarvestLevel[material], 5.5F, 1.0F,
                kDurability[material]};
    case ToolType::Hoe: {
        constexpr std::array<float, 5> kSpeed{1.0F, 1.0F, 2.0F, 3.0F, 4.0F};
        return {kMiningSpeed[material], kHarvestLevel[material], 1.0F, kSpeed[material],
                kDurability[material]};
    }
    default:
        return {};
    }
}

[[nodiscard]] constexpr FoodValue foodValue(const Item* item) {
    return item == nullptr ? FoodValue{} : item->nutrition;
}

[[nodiscard]] constexpr bool isFood(const Item* item) {
    return foodValue(item).foodLevel > 0;
}

// The texture-array layer the renderer assigned an item's icon. gameplay only
// stores the mapping; render/ owns the numbering and fills it in when it builds
// the atlas. Returns 0 for the block sentinel or an item without a layer yet.
inline std::unordered_map<const Item*, float>& itemTextureLayerTable() {
    static std::unordered_map<const Item*, float> layers;
    return layers;
}

inline void setItemTextureLayer(const Item* item, float layer) {
    itemTextureLayerTable()[item] = layer;
}

[[nodiscard]] inline float itemTextureLayer(const Item* item) {
    if (item == nullptr) return 0.0F;
    const auto found = itemTextureLayerTable().find(item);
    return found == itemTextureLayerTable().end() ? 0.0F : found->second;
}

} // namespace mc::gameplay
