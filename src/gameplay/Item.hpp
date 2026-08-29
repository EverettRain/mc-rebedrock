#pragma once

#include "core/CreativeCategory.hpp"
#include "core/Identifier.hpp"
#include "gameplay/DyeColor.hpp"
#include "gameplay/EquipmentSlot.hpp"
#include "gameplay/ItemUse.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
// The creative-inventory tab an item or block is filed under. The type itself
// lives in core/CreativeCategory.hpp (AR-CI) so world/Block.hpp can declare a
// block's tab too, without a circular include back to this header; the alias
// keeps every existing `gameplay::CreativeCategory` call site unchanged.
using core::CreativeCategory;

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
    // ShearsItem (26.1): no harvest table of its own — mining speed/harvest
    // level are irrelevant (nothing in kItemRegistry routes ordinary block
    // breaking through it yet) — but it wears down 1 point per shear, so it
    // needs a durability entry in toolAttributes. ToolTier::None; shears are
    // not tiered.
    Shears,
    // RW-1: BowItem — like Shears, no mining-speed/harvest-level table (a bow
    // never breaks blocks) but it does wear down (1 point per shot,
    // ItemStack#damage(1, ...) in onStoppedUsing), so it needs the same
    // durability-only slot in toolAttributes. ToolTier::None; a bow is not
    // materialed either.
    Bow,
    // AR-CX4-b: FlintAndSteelItem (26.1) — no mining/harvest table (it never
    // breaks blocks), but wears down one point per ignite (64 durability,
    // Items.FLINT_AND_STEEL's maxDamage(64)), so it takes the same durability-
    // only slot Shears/Bow use. Its right-click-on-block behaviour (place fire)
    // is dispatched by this ToolType in itemUseOnSlot, the same way the hoe's is.
    // ToolTier::None; flint and steel is not materialed.
    FlintAndSteel,
};

enum class ToolTier : std::uint8_t {
    None,
    Wood,
    Stone,
    Iron,
    Gold,
    Diamond,
};

// EQ-0: Java 1.16.1's ArmorMaterials enum — the five armor materials, kept as
// its own enum rather than reusing ToolTier because leather and chainmail
// have no tool-tier counterpart (and gold/iron/diamond's armor numbers do not
// share ToolTier's mining-speed/harvest-level axis at all). None is the "not
// armor" sentinel, mirroring ToolTier::None.
enum class ArmorMaterialId : std::uint8_t {
    None,
    Leather,
    Chainmail,
    Iron,
    Gold,
    Diamond,
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
    // AR-B2: DoorBlockItem — its useOn places two cells atomically (the lower
    // half here, the upper half above it) rather than the one every other
    // BlockItem places.
    Door,
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

    // EQ-0: marks the item as armor of the given material worn in the given
    // slot — Java 1.16.1's ArmorItem constructor. Mirrors tool()'s shape: the
    // registry table (armorAttributes below) derives protection/toughness/
    // durability/enchantability from (material, slot) rather than storing
    // them redundantly on every one of the 20 items.
    [[nodiscard]] constexpr Item armor(ArmorMaterialId material, EquipmentSlot slot) const {
        Item copy = *this;
        copy.armorMaterial = material;
        copy.armorSlot = slot;
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
    CreativeCategory creativeCategory = CreativeCategory::Ingredients;
    std::uint8_t maximumStackSize = 64U;
    // The tool role and material for tools; ToolType::None for everything else.
    ToolType toolType = ToolType::None;
    ToolTier toolTier = ToolTier::None;
    // EQ-0: the material and body slot for armor; ArmorMaterialId::None for
    // everything else (armorSlot is then meaningless — always check the
    // material, matching how toolTier is only meaningful under toolType).
    ArmorMaterialId armorMaterial = ArmorMaterialId::None;
    EquipmentSlot armorSlot = EquipmentSlot::Offhand;
    FoodValue nutrition{};
    // Item#useOn: what right-clicking with this item does. Null for an item the
    // interaction system does not route through the item (it places nothing).
    ItemUseFn useOn = nullptr;
};

// SpawnEggItem (1.16.1): an Item that knows which entity it spawns. Storing the
// EntityType supplier here (rather than in a parallel mapping) lets the renderer
// tint each egg with its species' colours and lets the interaction system spawn
// the right creature — all without a hardcoded entity list. The supplier is a
// function pointer because every species is looked up by name at call time
// (SpawnEggItems.hpp); the pointer is known at link time so the instance stays
// constexpr.
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

// DoorBlockItem (AR-B2): a door's own block item, marked so the interaction
// system routes its useOn through the two-cell atomic placement
// (PlayerInteraction.cpp's ItemUseAction::PlaceDoor) instead of the ordinary
// single-cell PlaceBlock every other BlockItem takes.
class DoorBlockItem : public BlockItem {
  public:
    constexpr DoorBlockItem() = default;

    constexpr DoorBlockItem(world::Block block) : BlockItem(block) {
        blockItemKind = BlockItemKind::Door;
    }
};

[[nodiscard]] inline const DoorBlockItem* asDoorBlockItem(const Item* item) {
    if (item != nullptr && item->blockItemKind == BlockItemKind::Door) {
        return static_cast<const DoorBlockItem*>(item);
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
    // The redstone torch stands on the floor or hangs on a wall exactly as the
    // plain torch does (RedstoneTorch/RedstoneWallTorch), so it is the same
    // StandingAndWallBlockItem — without this it placed the standing variant
    // only and could never be hung on a side face.
    static const StandingAndWallBlockItem redstoneTorch{world::Block::RedstoneTorch,
                                                        world::Block::RedstoneWallTorch};
    if (block == world::Block::RedstoneTorch) return &redstoneTorch;
    if (!world::isValidBlock(block)) return nullptr;
    // Crop blocks (wheat/carrots/potatoes — the BlockModel::Crop family) have no
    // item form in vanilla: the harvested produce (Items.WHEAT/CARROT/POTATO) is
    // a separate item and the crop is planted from its seed, never wielded. A
    // null here keeps the 2D "wheat plant" block-item from ever being obtainable
    // (pick-block, catalog index, a datapack) and stops the "wheat" block name
    // from shadowing the wheat item — the root, not just the codec/give symptom.
    // Model-driven like the Door/Leaves cases above, so a new crop needs no line.
    if (world::blockDefinition(block).model == world::BlockModel::Crop) {
        return nullptr;
    }
    // AR-B2: a door is placed as two cells, so its BlockItem is a DoorBlockItem
    // — model-driven, not a per-species identity check, so a second door
    // species needs no line here.
    if (world::blockDefinition(block).model == world::BlockModel::Door) {
        static const std::array<DoorBlockItem, static_cast<std::size_t>(world::Block::Count)>
            doorItems = [] {
                std::array<DoorBlockItem, static_cast<std::size_t>(world::Block::Count)> result{};
                for (std::size_t index = 0; index < result.size(); ++index) {
                    result[index] = DoorBlockItem{static_cast<world::Block>(index)};
                }
                return result;
            }();
        return &doorItems[static_cast<std::size_t>(block)];
    }
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
    Item::of("bucket").category(CreativeCategory::Ingredients).stackSize(16U);
inline constexpr Item WaterBucket = Item::of("water_bucket")
                                        .category(CreativeCategory::Ingredients)
                                        .single();
inline constexpr Item LavaBucket = Item::of("lava_bucket")
                                       .category(CreativeCategory::Ingredients)
                                       .single();
// MilkBucketItem (26.1): AR-A3's milking product. Vanilla files it under the
// Food & Drinks creative tab (not Materials, unlike the water/lava buckets) —
// this project's catalog has no separate drinks tab, so Food is the closest
// fit. Drinking it (PlayerInteraction's use-item timeline) clears every
// active status effect and reverts to an empty Bucket in survival; creative
// keeps pouring/drinking without spending the stack, the same
// restoresHeldStack rule the water/lava buckets already follow. No FoodValue
// is set — milk does not restore hunger, only status effects, so it is not
// classified `isFood` and never enters the ordinary eating gate.
inline constexpr Item MilkBucket = Item::of("milk_bucket")
                                       .category(CreativeCategory::FoodAndDrink)
                                       .single();
inline constexpr Item Coal =
    Item::of("coal").category(CreativeCategory::Ingredients);
inline constexpr Item IronIngot =
    Item::of("iron_ingot").category(CreativeCategory::Ingredients);
inline constexpr Item GoldIngot =
    Item::of("gold_ingot").category(CreativeCategory::Ingredients);
inline constexpr Item Diamond =
    Item::of("diamond").category(CreativeCategory::Ingredients);
inline constexpr Item Emerald =
    Item::of("emerald").category(CreativeCategory::Ingredients);
// The ores' 26.1 drops: iron/gold/copper drop a *raw* ore item (smelted to the
// ingot), lapis/redstone/quartz drop their material directly. iron_ingot and
// gold_ingot already exist as the smelting products; these are the mined form.
inline constexpr Item RawIron =
    Item::of("raw_iron").category(CreativeCategory::Ingredients);
inline constexpr Item RawCopper =
    Item::of("raw_copper").category(CreativeCategory::Ingredients);
inline constexpr Item RawGold =
    Item::of("raw_gold").category(CreativeCategory::Ingredients);
inline constexpr Item LapisLazuli =
    Item::of("lapis_lazuli").category(CreativeCategory::Ingredients);
inline constexpr Item Redstone =
    Item::of("redstone").category(CreativeCategory::Redstone);
inline constexpr Item Quartz =
    Item::of("quartz").category(CreativeCategory::Ingredients);
inline constexpr Item Stick =
    Item::of("stick").category(CreativeCategory::Ingredients);
inline constexpr Item Flint =
    Item::of("flint").category(CreativeCategory::Ingredients);
inline constexpr Item Feather =
    Item::of("feather").category(CreativeCategory::Ingredients);
inline constexpr Item String =
    Item::of("string").category(CreativeCategory::Ingredients);
inline constexpr Item Leather =
    Item::of("leather").category(CreativeCategory::Ingredients);
inline constexpr Item Sugar =
    Item::of("sugar").category(CreativeCategory::Ingredients);
// AR-A1: EggItem (26.1) files under the Food tab (it is throwable AND edible
// via FoodComponents.EGG, but the creative catalog lists it under Foodstuffs,
// not Materials — this was mis-tabbed before any chicken existed to drop it).
inline constexpr Item Egg =
    Item::of("egg").category(CreativeCategory::FoodAndDrink).stackSize(16U);
inline constexpr Item Bone =
    Item::of("bone").category(CreativeCategory::Ingredients);
inline constexpr Item Paper =
    Item::of("paper").category(CreativeCategory::Ingredients);
inline constexpr Item Book =
    Item::of("book").category(CreativeCategory::Ingredients);
// WheatSeedsItem: right-clicking farmland plants the wheat crop. The behaviour
// is dispatched by item identity in itemUseOn (ItemPlacement.cpp), the way the
// buckets are, so the constexpr registrations stay free of function pointers.
inline constexpr Item WheatSeeds = Item::of("wheat_seeds")
                                       .category(CreativeCategory::Ingredients);
inline constexpr Item Wheat =
    Item::of("wheat").category(CreativeCategory::Ingredients);

// Food
inline constexpr Item Apple = Item::of("apple")
                                  .category(CreativeCategory::FoodAndDrink)
                                  .food({4, 0.3F});
inline constexpr Item Bread = Item::of("bread")
                                  .category(CreativeCategory::FoodAndDrink)
                                  .food({5, 0.6F});
inline constexpr Item Porkchop = Item::of("porkchop")
                                     .category(CreativeCategory::FoodAndDrink)
                                     .food({3, 0.3F});
inline constexpr Item CookedPorkchop = Item::of("cooked_porkchop")
                                           .category(CreativeCategory::FoodAndDrink)
                                           .food({8, 0.8F});
// Raw beef: the cow's meat drop. Vanilla food value 3 hunger / 0.3 saturation,
// identical to raw porkchop.
inline constexpr Item Beef = Item::of("beef")
                                 .category(CreativeCategory::FoodAndDrink)
                                 .food({3, 0.3F});
// Raw chicken: the chicken's meat drop. Vanilla food value 2 hunger / 0.3
// saturation (lower than the other raw meats — it also carries a hunger-effect
// chance on eat raw in 26.1, which this build does not model yet: EM2 status
// effects, not in scope here).
inline constexpr Item RawChicken = Item::of("chicken")
                                       .category(CreativeCategory::FoodAndDrink)
                                       .food({2, 0.3F});
// Mutton: the sheep's meat drop. Vanilla food value 2 hunger / 0.3 saturation.
inline constexpr Item Mutton = Item::of("mutton")
                                   .category(CreativeCategory::FoodAndDrink)
                                   .food({2, 0.3F});
// Rotten flesh: the zombie/husk melee drop. Vanilla food value 4 hunger / 0.1
// saturation — edible, but AR-M1 defers the FoodComponents.ROTTEN_FLESH
// "chance of Hunger effect on eat" behaviour (needs EM2 status effects, not in
// scope here); the item still registers and feeds like any other food.
inline constexpr Item RottenFlesh = Item::of("rotten_flesh")
                                        .category(CreativeCategory::FoodAndDrink)
                                        .food({4, 0.1F});
// Carrot and potato are both food (1.16.1 FoodComponent) and the seed of their
// own crop — a held carrot/potato plants itself on farmland, like the vanilla
// items whose useOn is a SeedsItem subclass. Planting is dispatched by item
// identity in itemUseOn; right-clicking empty ground still eats them.
inline constexpr Item Carrot = Item::of("carrot")
                                   .category(CreativeCategory::FoodAndDrink)
                                   .food({3, 0.6F});
inline constexpr Item Potato = Item::of("potato")
                                   .category(CreativeCategory::FoodAndDrink)
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
// ShearsItem (26.1): AR-A2's sheep-shearing tool. Single-stacking, 238
// durability (toolAttributes' Shears case), one point spent per shear. Its
// right-click-on-mob behaviour is dispatched in PlayerInteraction.cpp, not
// through useOn — a mob is not a block placement target.
inline constexpr Item Shears = Item::of("shears")
                                   .category(CreativeCategory::Tools)
                                   .single()
                                   .tool(ToolType::Shears, ToolTier::None);

// AR-CX4-b: FlintAndSteelItem (26.1). Single-stacking, 64 durability
// (toolAttributes' FlintAndSteel case), one point spent per ignite. Its
// right-click-on-block behaviour (place fire on the clicked face) is dispatched
// by its ToolType in ItemPlacement.cpp, the same way the hoe's till is —
// FlintAndSteelItem#useOn is a block-target use, not a mob interaction.
// Craftable from 1 iron + 1 flint (both already obtainable), so it is reachable
// the moment it registers.
inline constexpr Item FlintAndSteel = Item::of("flint_and_steel")
                                          .category(CreativeCategory::Tools)
                                          .single()
                                          .tool(ToolType::FlintAndSteel, ToolTier::None);

// RW-1: ArrowItem (1.16.1) — ordinary stackable ammunition, 64 per stack
// (Item.Settings' default maxCount, ArrowItem sets no override). Ranged
// weapons scan the whole inventory for one (Inventory::findArrowSlot below),
// not just the selected hotbar slot, mirroring PlayerEntity#getArrowType's
// inventory scan.
inline constexpr Item Arrow =
    Item::of("arrow").category(CreativeCategory::Combat);
// RW-1: BowItem (1.16.1) — a charge/startUsing item (UseAnimation::Bow,
// vanilla's UseAction.BOW), 384 durability (toolAttributes' Bow case), one
// point spent per shot. No useOn: a bow is never aimed at a block placement
// target — PlayerInteraction's release path (UseItemStop) drives the whole
// draw/release/spawn-arrow sequence directly, the same way eating's
// beginEating/tickEating pair drives food instead of a useOn callback.
inline constexpr Item Bow = Item::of("bow")
                                .category(CreativeCategory::Combat)
                                .single()
                                .tool(ToolType::Bow, ToolTier::None);

// DYE-1: the 16 DyeItems (1.16.1 Items.WHITE_DYE .. Items.BLACK_DYE). Each is an
// ordinary stackable material whose id is `<colour>_dye` — the vanilla registry
// name, so a give command resolving `minecraft:light_blue_dye` still works. A
// dye item's texture is item/<colour>_dye.png (Item::of's default), matching the
// vanilla asset layout; the renderer tints nothing (unlike a spawn egg) — each
// dye already has its own coloured sprite. The colour a dye carries is not
// stored on the Item; it is recovered from the item's position in kDyeItems
// below (position == DyeColor id, white=0 .. black=15), the same "identity is
// the array index" DOD rule DyeColor itself uses.
//
// The ids are string literals (static storage) rather than generated into a
// buffer: an Item stores its id as a string_view, so the backing characters must
// outlive every copy of the Item — a per-item buffer a copied aggregate points
// back into is not constexpr-valid. The literals are kept in DyeColor id order
// so their listing order below matches kDyeColors' names one-to-one.
inline constexpr Item WhiteDye = Item::of("white_dye").category(CreativeCategory::Ingredients);
inline constexpr Item OrangeDye = Item::of("orange_dye").category(CreativeCategory::Ingredients);
inline constexpr Item MagentaDye = Item::of("magenta_dye").category(CreativeCategory::Ingredients);
inline constexpr Item LightBlueDye =
    Item::of("light_blue_dye").category(CreativeCategory::Ingredients);
inline constexpr Item YellowDye = Item::of("yellow_dye").category(CreativeCategory::Ingredients);
inline constexpr Item LimeDye = Item::of("lime_dye").category(CreativeCategory::Ingredients);
inline constexpr Item PinkDye = Item::of("pink_dye").category(CreativeCategory::Ingredients);
inline constexpr Item GrayDye = Item::of("gray_dye").category(CreativeCategory::Ingredients);
inline constexpr Item LightGrayDye =
    Item::of("light_gray_dye").category(CreativeCategory::Ingredients);
inline constexpr Item CyanDye = Item::of("cyan_dye").category(CreativeCategory::Ingredients);
inline constexpr Item PurpleDye = Item::of("purple_dye").category(CreativeCategory::Ingredients);
inline constexpr Item BlueDye = Item::of("blue_dye").category(CreativeCategory::Ingredients);
inline constexpr Item BrownDye = Item::of("brown_dye").category(CreativeCategory::Ingredients);
inline constexpr Item GreenDye = Item::of("green_dye").category(CreativeCategory::Ingredients);
inline constexpr Item RedDye = Item::of("red_dye").category(CreativeCategory::Ingredients);
inline constexpr Item BlackDye = Item::of("black_dye").category(CreativeCategory::Ingredients);

// Pointers to each dye Item, indexed by DyeColor id — the "position == colour"
// lookup both directions of the mapping (dyeItemFor / dyeColorForItem) use. The
// order here must match the DyeColor enum exactly (a mismatch would dye a sheep
// the wrong colour); the static_assert below guards it against the id text.
inline constexpr std::array<const Item*, gameplay::kDyeColorCount> kDyeItems{
    &WhiteDye,  &OrangeDye, &MagentaDye,  &LightBlueDye, &YellowDye, &LimeDye,
    &PinkDye,   &GrayDye,   &LightGrayDye, &CyanDye,     &PurpleDye, &BlueDye,
    &BrownDye,  &GreenDye,  &RedDye,       &BlackDye,
};

// The dye at each index must be `<colour>_dye` for that index's DyeColor name,
// so position-as-colour is honest — a reordering that put orange_dye at the
// white slot is a compile error, not a silently mis-dyed sheep.
static_assert([] {
    for (std::size_t index = 0; index < gameplay::kDyeColorCount; ++index) {
        const std::string_view id = kDyeItems[index]->identifier.path;
        const std::string_view name = gameplay::kDyeColors[index].name;
        if (id.size() != name.size() + 4U ||
            id.substr(0, name.size()) != name || id.substr(name.size()) != "_dye") {
            return false;
        }
    }
    return true;
}(), "kDyeItems must list <colour>_dye in DyeColor id order");

// DYE-2: the wool Block each DyeColor drops, indexed by DyeColor id (position ==
// colour, white=0 .. black=15), the same "identity is the array index" DOD rule
// kDyeItems uses. A sheep's coloured shear/kill drop is one array read here — no
// per-colour switch that grows a case each time a colour is added, and no
// allocation on the drop's hot path. The mapping is constexpr .rodata, and the
// static_assert below pins each slot to the `<colour>_wool` block named for that
// index's DyeColor, so a reordering that dropped orange wool from a white sheep
// is a compile error, not a shipped bug.
inline constexpr std::array<world::Block, gameplay::kDyeColorCount> kWoolBlocks{
    world::Block::WhiteWool,     world::Block::OrangeWool, world::Block::MagentaWool,
    world::Block::LightBlueWool, world::Block::YellowWool, world::Block::LimeWool,
    world::Block::PinkWool,      world::Block::GrayWool,   world::Block::LightGrayWool,
    world::Block::CyanWool,      world::Block::PurpleWool, world::Block::BlueWool,
    world::Block::BrownWool,     world::Block::GreenWool,  world::Block::RedWool,
    world::Block::BlackWool,
};

// The wool Block a colour drops. A pure table lookup on the dense DyeColor id —
// the single home for "which wool does this colour give" every drop path (shear
// in PlayerInteraction, kill in the sheep loot roll) shares.
[[nodiscard]] constexpr world::Block woolBlockFor(gameplay::DyeColor color) noexcept {
    return kWoolBlocks[gameplay::dyeColorId(color)];
}

// True for any of the 16 wool blocks. Membership is tested against kWoolBlocks
// (the authoritative set) rather than an enum-ordinal range, so the predicate
// stays correct if the wool blocks are ever reordered. The single home the
// colour-remap on a coloured mob's death drops (EntitySystem::die) consults to
// find "which of these drops is wool" without hard-coding a wool identity there.
[[nodiscard]] constexpr bool isWool(world::Block block) noexcept {
    for (const world::Block wool : kWoolBlocks) {
        if (wool == block) return true;
    }
    return false;
}

// The wool at each index must be `<colour>_wool` for that index's DyeColor name,
// so position-as-colour is honest: a mismatch (wrong block at a colour slot, or
// a renamed wool block) is a compile error, not a sheep dropping the wrong wool.
static_assert([] {
    for (std::size_t index = 0; index < gameplay::kDyeColorCount; ++index) {
        const std::string_view id = world::blockDefinition(kWoolBlocks[index]).identifier.path;
        const std::string_view name = gameplay::kDyeColors[index].name;
        if (id.size() != name.size() + 5U || id.substr(0, name.size()) != name ||
            id.substr(name.size()) != "_wool") {
            return false;
        }
    }
    return true;
}(), "kWoolBlocks must list <colour>_wool in DyeColor id order");

// Armor: 5 materials (leather/chainmail/iron/gold/diamond) x 4 slots
// (head/chest/legs/feet), Java 1.16.1 ArmorItem. Each is single-stacking,
// carries its material + slot (armorAttributes below derives protection,
// toughness, durability and enchantability from that pair), and files under
// the Combat creative tab. Chainmail has no crafting recipe in 1.16.1 (see
// RecipeBakedData.inc's comment) but its item identity still registers —
// obtainable via /give, mob drops and (once loot is wired) trades, exactly
// as vanilla.
inline constexpr Item LeatherHelmet = Item::of("leather_helmet")
                                          .category(CreativeCategory::Combat)
                                          .single()
                                          .armor(ArmorMaterialId::Leather, EquipmentSlot::Head);
inline constexpr Item LeatherChestplate =
    Item::of("leather_chestplate")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Leather, EquipmentSlot::Chest);
inline constexpr Item LeatherLeggings =
    Item::of("leather_leggings")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Leather, EquipmentSlot::Legs);
inline constexpr Item LeatherBoots = Item::of("leather_boots")
                                         .category(CreativeCategory::Combat)
                                         .single()
                                         .armor(ArmorMaterialId::Leather, EquipmentSlot::Feet);
inline constexpr Item ChainmailHelmet =
    Item::of("chainmail_helmet")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Chainmail, EquipmentSlot::Head);
inline constexpr Item ChainmailChestplate =
    Item::of("chainmail_chestplate")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Chainmail, EquipmentSlot::Chest);
inline constexpr Item ChainmailLeggings =
    Item::of("chainmail_leggings")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Chainmail, EquipmentSlot::Legs);
inline constexpr Item ChainmailBoots =
    Item::of("chainmail_boots")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Chainmail, EquipmentSlot::Feet);
inline constexpr Item IronHelmet = Item::of("iron_helmet")
                                       .category(CreativeCategory::Combat)
                                       .single()
                                       .armor(ArmorMaterialId::Iron, EquipmentSlot::Head);
inline constexpr Item IronChestplate = Item::of("iron_chestplate")
                                           .category(CreativeCategory::Combat)
                                           .single()
                                           .armor(ArmorMaterialId::Iron, EquipmentSlot::Chest);
inline constexpr Item IronLeggings = Item::of("iron_leggings")
                                         .category(CreativeCategory::Combat)
                                         .single()
                                         .armor(ArmorMaterialId::Iron, EquipmentSlot::Legs);
inline constexpr Item IronBoots = Item::of("iron_boots")
                                      .category(CreativeCategory::Combat)
                                      .single()
                                      .armor(ArmorMaterialId::Iron, EquipmentSlot::Feet);
inline constexpr Item GoldHelmet = Item::of("golden_helmet")
                                       .category(CreativeCategory::Combat)
                                       .single()
                                       .armor(ArmorMaterialId::Gold, EquipmentSlot::Head);
inline constexpr Item GoldChestplate =
    Item::of("golden_chestplate")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Gold, EquipmentSlot::Chest);
inline constexpr Item GoldLeggings = Item::of("golden_leggings")
                                         .category(CreativeCategory::Combat)
                                         .single()
                                         .armor(ArmorMaterialId::Gold, EquipmentSlot::Legs);
inline constexpr Item GoldBoots = Item::of("golden_boots")
                                      .category(CreativeCategory::Combat)
                                      .single()
                                      .armor(ArmorMaterialId::Gold, EquipmentSlot::Feet);
inline constexpr Item DiamondHelmet =
    Item::of("diamond_helmet")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Diamond, EquipmentSlot::Head);
inline constexpr Item DiamondChestplate =
    Item::of("diamond_chestplate")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Diamond, EquipmentSlot::Chest);
inline constexpr Item DiamondLeggings =
    Item::of("diamond_leggings")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Diamond, EquipmentSlot::Legs);
inline constexpr Item DiamondBoots =
    Item::of("diamond_boots")
        .category(CreativeCategory::Combat)
        .single()
        .armor(ArmorMaterialId::Diamond, EquipmentSlot::Feet);

// Spawn eggs are defined in SpawnEggItems.hpp (they need entity headers that
// live above Item.hpp in the include graph). See kSpawnEggItems there.

} // namespace items

// The item registry: every registered item whose definition can live here.
// Spawn eggs are listed separately in kSpawnEggItems (SpawnEggItems.hpp) because
// their constructors need entity headers that sit above us in the include graph.
// The order sets both the creative-catalog order within each tab and the item
// texture-array layout the renderer appends. Grouped materials / food / tools.
inline constexpr std::array<const Item*, 102> kItemRegistry{
    &items::Bucket,     &items::WaterBucket, &items::LavaBucket, &items::MilkBucket,
    &items::Coal,
    &items::IronIngot,
    &items::GoldIngot,  &items::Diamond,     &items::Emerald,
    // The mined ore items (26.1 raw ores + lapis/redstone/quartz materials).
    &items::RawIron,    &items::RawCopper,   &items::RawGold,
    &items::LapisLazuli, &items::Redstone,   &items::Quartz,
    &items::Stick,
    &items::Flint,      &items::Feather,     &items::String,     &items::Leather,
    &items::Sugar,      &items::Egg,         &items::Bone,       &items::Paper,
    &items::Book,       &items::WheatSeeds,  &items::Wheat,
    &items::Apple,      &items::Bread,       &items::Porkchop,   &items::CookedPorkchop,
    &items::Beef,       &items::RawChicken,  &items::Mutton,     &items::RottenFlesh,
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
    &items::Shears,
    // AR-CX4-b: flint and steel (the ignite tool).
    &items::FlintAndSteel,
    // RW-1: arrow (ammunition) + bow (the charge/draw ranged weapon).
    &items::Arrow,      &items::Bow,
    // EQ-0: armor, 5 materials x 4 slots (head, chest, legs, feet order).
    &items::LeatherHelmet,   &items::LeatherChestplate, &items::LeatherLeggings,
    &items::LeatherBoots,
    &items::ChainmailHelmet, &items::ChainmailChestplate, &items::ChainmailLeggings,
    &items::ChainmailBoots,
    &items::IronHelmet,      &items::IronChestplate,    &items::IronLeggings,
    &items::IronBoots,
    &items::GoldHelmet,      &items::GoldChestplate,    &items::GoldLeggings,
    &items::GoldBoots,
    &items::DiamondHelmet,   &items::DiamondChestplate, &items::DiamondLeggings,
    &items::DiamondBoots,
    // DYE-1: the 16 dyes, in DyeColor id order (white=0 .. black=15) — the same
    // order kDyeItems indexes them, so the creative catalog lists them in the
    // vanilla colour sequence.
    items::kDyeItems[0],  items::kDyeItems[1],  items::kDyeItems[2],  items::kDyeItems[3],
    items::kDyeItems[4],  items::kDyeItems[5],  items::kDyeItems[6],  items::kDyeItems[7],
    items::kDyeItems[8],  items::kDyeItems[9],  items::kDyeItems[10], items::kDyeItems[11],
    items::kDyeItems[12], items::kDyeItems[13], items::kDyeItems[14], items::kDyeItems[15],
};

// A forgotten count bump (adding an item without growing the array, or vice
// versa) is a compile error, not a silent truncation: 57 pre-EQ-0 items + the
// 20 armor items EQ-0 added + the 2 (arrow, bow) RW-1 adds + the 16 dyes DYE-1
// adds here.
static_assert(kItemRegistry.size() == 57U + 20U + 2U + 16U + 1U + 6U,
              "kItemRegistry size must track every entry listed above — bump "
              "this alongside the array when adding or removing items "
              "(the +1 is AR-CX4-b's flint_and_steel; the +6 are the mined ore "
              "items raw_iron/raw_copper/raw_gold/lapis_lazuli/redstone/quartz)");

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
    case ToolType::Shears:
        // ShearsItem (26.1): 238 durability, no tier table (it is not
        // materialed) — a flat entry independent of `tier`/`material` above.
        return {15.0F, 0U, 1.0F, 1.0F, 238U};
    case ToolType::Bow:
        // BowItem (1.16.1): 384 durability (Items.java's
        // `new BowItem(new Item.Settings().maxDamage(384)...)`), same flat
        // shape as Shears — no mining speed/harvest level/attack numbers a
        // bow ever consults.
        return {1.0F, 0U, 1.0F, 1.0F, 384U};
    case ToolType::FlintAndSteel:
        // FlintAndSteelItem (26.1): 64 durability (Items.FLINT_AND_STEEL's
        // maxDamage(64)), one point per ignite. Same flat shape as Shears/Bow —
        // no mining speed/harvest level/attack numbers a lighter ever consults.
        return {1.0F, 0U, 1.0F, 1.0F, 64U};
    default:
        return {};
    }
}

// EQ-0: the position of an armor slot in the per-slot tables below, matching
// vanilla's own EquipmentSlot#getEntitySlotId ordering (FEET=0, LEGS=1,
// CHEST=2, HEAD=3) — the exact order Java's ArmorMaterials/ArmorItem index
// PROTECTION_VALUES and BASE_DURABILITY by. gameplay::EquipmentSlot's own
// numeric values differ (Offhand=0 first, so this project can index its own
// kArmorSlots/EquipmentSlots array too), so this is a deliberate second,
// vanilla-shaped index rather than reusing EquipmentSlot's underlying value.
[[nodiscard]] constexpr std::size_t armorSlotIndex(EquipmentSlot slot) {
    switch (slot) {
    case EquipmentSlot::Feet: return 0;
    case EquipmentSlot::Legs: return 1;
    case EquipmentSlot::Chest: return 2;
    case EquipmentSlot::Head: return 3;
    case EquipmentSlot::Offhand: return 0;
    }
    return 0;
}

// The position of an armor material in the per-material tables below.
[[nodiscard]] constexpr std::size_t armorMaterialIndex(ArmorMaterialId material) {
    switch (material) {
    case ArmorMaterialId::Leather: return 0;
    case ArmorMaterialId::Chainmail: return 1;
    case ArmorMaterialId::Iron: return 2;
    case ArmorMaterialId::Gold: return 3;
    case ArmorMaterialId::Diamond: return 4;
    case ArmorMaterialId::None: return 0;
    }
    return 0;
}

// Java 1.16.1 ArmorMaterial: what one piece of armor (a material x slot pair)
// carries. toughness/enchantability are per-material (every slot of the same
// material shares them); protection and durability vary by slot too.
struct ArmorAttributes final {
    std::uint8_t protection = 0;
    float toughness = 0.0F;
    std::uint8_t enchantability = 0;
    std::uint16_t durability = 0;
};

// Java 1.16.1 ArmorMaterials enum (net.minecraft.item.ArmorMaterials, yarn
// 1.16.1) — transcribed from the decompiled source, not the wiki. Material
// order below is Leather, Chainmail, Iron, Gold, Diamond (armorMaterialIndex
// above); each PROTECTION_VALUES row is {feet, legs, chest, head}
// (armorSlotIndex above) exactly as ArmorMaterials declares
// `new int[]{feet, legs, chest, head}`:
//   LEATHER   {1, 2, 3, 1}   durabilityMultiplier  5   toughness 0.0  ench 15
//   CHAIN     {1, 4, 5, 2}   durabilityMultiplier 15   toughness 0.0  ench 12
//   IRON      {2, 5, 6, 2}   durabilityMultiplier 15   toughness 0.0  ench  9
//   GOLD      {1, 3, 5, 2}   durabilityMultiplier  7   toughness 0.0  ench 25
//   DIAMOND   {3, 6, 8, 3}   durabilityMultiplier 33   toughness 2.0  ench 10
// getDurability(slot) = BASE_DURABILITY[slot] * durabilityMultiplier, where
// BASE_DURABILITY = {13, 15, 16, 11} (feet, legs, chest, head) — so the
// per-slot durability actually stored below is that product, precomputed
// (the multiplier itself is not otherwise needed anywhere in this project).
[[nodiscard]] constexpr ArmorAttributes armorAttributes(ArmorMaterialId material,
                                                         EquipmentSlot slot) {
    constexpr std::array<std::uint16_t, 4> kBaseDurability{13, 15, 16, 11};
    constexpr std::array<std::array<std::uint8_t, 4>, 5> kProtection{{
        {1, 2, 3, 1},  // Leather
        {1, 4, 5, 2},  // Chainmail
        {2, 5, 6, 2},  // Iron
        {1, 3, 5, 2},  // Gold
        {3, 6, 8, 3},  // Diamond
    }};
    constexpr std::array<std::uint8_t, 5> kDurabilityMultiplier{5, 15, 15, 7, 33};
    constexpr std::array<float, 5> kToughness{0.0F, 0.0F, 0.0F, 0.0F, 2.0F};
    constexpr std::array<std::uint8_t, 5> kEnchantability{15, 12, 9, 25, 10};

    if (material == ArmorMaterialId::None) return {};
    const std::size_t materialIndex = armorMaterialIndex(material);
    const std::size_t slotIndex = armorSlotIndex(slot);
    const std::uint16_t durability = static_cast<std::uint16_t>(
        kBaseDurability[slotIndex] * kDurabilityMultiplier[materialIndex]);
    return {kProtection[materialIndex][slotIndex], kToughness[materialIndex],
            kEnchantability[materialIndex], durability};
}

[[nodiscard]] constexpr bool isArmor(const Item* item) {
    return item != nullptr && item->armorMaterial != ArmorMaterialId::None;
}

// ArmorItem#getSlotType: which body slot an armor item occupies. Meaningless
// (and unused) for a non-armor item.
[[nodiscard]] constexpr EquipmentSlot armorSlotOf(const Item* item) {
    return item == nullptr ? EquipmentSlot::Offhand : item->armorSlot;
}

// The seam EQ-1 (wearing/removing armor) and EQ-2 (the still-empty
// armor/toughness stage in Damage.hpp's mitigation pipeline) both read
// through: how many armor points and how much toughness one worn item stack
// contributes. Zero for a non-armor item — summing these across the four
// equipped armor slots is exactly vanilla's LivingEntity#getArmor /
// #getAttributeValue(GENERIC_ARMOR_TOUGHNESS) totals (EntityAttributeModifier
// ADDITION across each equipped ArmorItem's per-slot modifier).
[[nodiscard]] constexpr std::uint8_t armorValue(const Item* item) {
    if (!isArmor(item)) return 0U;
    return armorAttributes(item->armorMaterial, item->armorSlot).protection;
}

[[nodiscard]] constexpr float armorToughness(const Item* item) {
    if (!isArmor(item)) return 0.0F;
    return armorAttributes(item->armorMaterial, item->armorSlot).toughness;
}

[[nodiscard]] constexpr FoodValue foodValue(const Item* item) {
    return item == nullptr ? FoodValue{} : item->nutrition;
}

[[nodiscard]] constexpr bool isFood(const Item* item) {
    return foodValue(item).foodLevel > 0;
}

// RW-1: BowItem#getProjectiles' BOW_PROJECTILES predicate (`stack ->
// stack.getItem() instanceof ArrowItem`) — this project has exactly one arrow
// item today, so a direct pointer match stands in for the tag/instanceof
// check; a second arrow species (spectral/tipped) would extend this the same
// way isDrinkable would extend for a second drinkable.
[[nodiscard]] constexpr bool isArrow(const Item* item) {
    return item == &items::Arrow;
}

// RW-1: BowItem's own identity, so PlayerInteraction can gate the draw/
// release path without an ItemUseAction/useOn hook (a bow is never aimed at a
// block placement target, so it has no useOn at all).
[[nodiscard]] constexpr bool isBow(const Item* item) {
    return item == &items::Bow;
}

// BowItem#getMaxUseTime (1.16.1): 72000 ticks — effectively "until released",
// never a self-expiring countdown the way eating's fixed 32 ticks is. Passed
// to PlayerActionState::startUsing as the draw's durationTicks so the shared
// use timeline never auto-finishes a held bow out from under the player.
inline constexpr std::uint32_t kBowMaxUseTicks = 72000U;
// BowItem#getPullProgress: `f = useTicks / 20.0`, then vanilla's own eased
// curve `(f*f + f*2) / 3`, clamped to 1 past full draw — NOT a bare linear
// t/20 (a straight ratio would make the draw feel front-loaded; the quadratic
// term is why the last few ticks before full draw gain pull faster than the
// first few). `useTicks` is elapsed ticks since the draw started
// (getMaxUseTime(stack) - remainingUseTicks in vanilla; the caller passes
// durationTicks - remainingTicks, the same subtraction against
// PlayerActionState's countdown).
[[nodiscard]] constexpr float bowPullProgress(std::uint32_t useTicks) {
    const float f = static_cast<float>(useTicks) / 20.0F;
    const float eased = (f * f + f * 2.0F) / 3.0F;
    return eased > 1.0F ? 1.0F : eased;
}
// AbstractArrow/BowItem's own full-draw numbers: velocity 3.0 blocks/tick
// (setProperties' `f * 3.0F` modifierZ term) and the PersistentProjectileEntity
// base damage field's default value 2.0 — onEntityHit's actual applied damage
// is `ceil(velocity.length() * damage)`, so a full draw (velocity length 3.0)
// deals ceil(3.0 * 2.0) = 6, matching vanilla's known full-draw arrow damage.
inline constexpr float kBowFullDrawVelocity = 3.0F;
inline constexpr float kArrowBaseDamage = 2.0F;
// BowItem#onStoppedUsing: `if (!(f < 0.1))` — a draw under 10% pull progress
// releases nothing at all (too weak to nock), the vanilla "tap and release"
// no-op.
inline constexpr float kBowMinimumPullProgress = 0.1F;

// MilkBucketItem#getUseAction (26.1): milk is drunk on the same held-right-
// click timeline as food (UseAction.DRINK, still 32 ticks — MilkBucketItem#
// getMaxUseTime), but it carries no FoodValue (drinking restores no hunger,
// only clears status effects), so it cannot piggyback isFood's "is this
// consumable" test. A single named predicate rather than a per-item flag: milk
// is the only drinkable item registered today, and a second one (a future
// potion) would need its own finish-of-use behaviour anyway, not just this
// bit — see GameSession::tickEating's branch, which is the actual behavioural
// fork.
[[nodiscard]] constexpr bool isDrinkable(const Item* item) {
    return item == &items::MilkBucket;
}

// Whether holding `item` down starts the shared 32-tick use timeline
// (PlayerInteraction's eat/drink gate) — food to eat, or milk to drink.
[[nodiscard]] constexpr bool startsUseTimeline(const Item* item) {
    return isFood(item) || isDrinkable(item);
}

// DYE-1: the DyeItem for a colour — DyeItem's own registry entry vanilla looks
// up as `DyeItem.byColor(color)`. Position == colour id (items::kDyeItems), so
// this is a bare array index, no scan.
[[nodiscard]] constexpr const Item* dyeItemFor(DyeColor color) {
    return items::kDyeItems[dyeColorId(color)];
}

// DYE-1: the colour a held item dyes with, or empty when the item is not one of
// the 16 dyes — the `item instanceof DyeItem ? ((DyeItem)item).getColor() : —`
// test SheepEntity#mobInteract runs. A linear match over 16 stable pointers; the
// interaction path is cold (one right-click), so a table lookup is not warranted.
[[nodiscard]] constexpr std::optional<DyeColor> dyeColorForItem(const Item* item) {
    if (item == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kDyeColorCount; ++index) {
        if (items::kDyeItems[index] == item) {
            return static_cast<DyeColor>(index);
        }
    }
    return std::nullopt;
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
