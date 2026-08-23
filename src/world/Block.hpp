#pragma once

#include "core/ContentId.hpp"
#include "core/CreativeCategory.hpp"
#include "core/Identifier.hpp"
#include "world/BlockEntityType.hpp"
#include "world/StateSchema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::world {

// Blocks and items share one identifier type and one namespace.
using core::Identifier;
using core::kNamespace;
using core::kVanillaNamespace;
// AR-CI: the creative-inventory tab, shared with gameplay::Item so a block and
// an ordinary item declare tab membership the same way. See
// core/CreativeCategory.hpp for why the type lives there instead of on either
// side directly (Item.hpp already includes this header, so this header cannot
// include Item.hpp back).
using core::CreativeCategory;

// The runtime block identity: a dense uint16 the block registry hands out (see
// BlockRegistry.hpp). Every table that used to be sized to `Block::Count` and
// indexed by the enum ordinal now indexes by this instead, so content can grow
// past the 256 the u8 enum topped out at.
using core::BlockId;

// The `Block` enum is, as of R0-2, a *transitional handle* over the registry's
// BlockId rather than the identity source: a built-in block's enum ordinal is
// exactly its BlockId (BlockRegistry asserts the equality), so `blockId()` and
// `blockFromId()` below convert with no lookup. New code should prefer BlockId;
// the enum stays because 589-odd call sites still name blocks as `Block::Stone`
// and the six `switch(block)` chains are R1's to retire, not R0's. Until then
// both spell the same identity, and this file keeps answering block questions by
// enum for source compatibility.
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
    // SlabBlock: a half-height block carrying a SlabType (bottom/top/double).
    // Each reuses its parent block's texture, so no new atlas entries are
    // needed. Two slabs of the same kind merge into a double when the second is
    // placed against the first.
    OakSlab,
    SpruceSlab,
    BirchSlab,
    JungleSlab,
    AcaciaSlab,
    DarkOakSlab,
    StoneSlab,
    CobblestoneSlab,
    StoneBrickSlab,
    SmoothStoneSlab,
    // Redstone power sources (W-4). RedstoneBlock is a constant source, powering
    // every side. RedstoneTorch/RedstoneWallTorch carry a LIT state and invert
    // their input on a fixed 2gt delay, driven purely by scheduled ticks and
    // block updates — never a random tick. Their signal semantics live in the
    // by-BlockId query table in gameplay/RedstoneSignal.hpp, and their timing in
    // the redstone component layer.
    RedstoneBlock,
    RedstoneTorch,
    RedstoneWallTorch,
    // A lever: a manually toggled redstone source. POWERED is its state; FACING
    // records the direction it was attached from, so it strongly powers the block
    // it hangs on (getConnectedDirection) and weakly powers every side.
    Lever,
    // A redstone repeater (RepeaterBlock/DiodeBlock): a one-way signal relay with
    // a 1-4 tick adjustable DELAY. FACING points at its input; POWERED is its
    // output. Locking is computed live from the side inputs, so it needs no
    // stored LOCKED property here.
    Repeater,
    // A redstone comparator (ComparatorBlock/DiodeBlock): compares or subtracts
    // its side input from its back input. FACING points at its input; MODE picks
    // compare/subtract; POWERED is the boolean output and AnalogSignal its 0-15
    // analog output (Java's ComparatorBlockEntity value).
    Comparator,
    // Redstone dust (RedStoneWireBlock): carries a 0-15 POWER (its AnalogSignal)
    // that attenuates one per cell along a wire network. Connection shape and the
    // directional powering rules land with a later slice; this identity carries
    // the power the serial evaluator distributes.
    RedstoneWire,
    // An observer (ObserverBlock): watches the block state on its FACING side and
    // emits a fixed 2gt pulse out its back on any change. FACING is the watched
    // side (six directions); POWERED is the pulse. Driven by the updateShape pass
    // (it is the first real consumer of that mechanism), not by neighborChanged.
    Observer,
    // A stone button (ButtonBlock): a timed pulse source. Pressing sets POWERED
    // for a fixed number of ticks (20 for stone), then it releases itself. Signal
    // is the lever's; FACING records the direction it hangs from.
    StoneButton,
    // A piston (PistonBaseBlock): extends/retracts on redstone power via a
    // two-phase block event (trigger on the update, settle at tick end). FACING
    // is the six-way push direction; EXTENDED is stored in the POWERED bit. The
    // sticky variant pulls one block back. The actual block movement (structure
    // resolver) is a separate large task; this carries the extension state.
    Piston,
    StickyPiston,
    // A trapped chest (TrappedChestBlock): a chest in every storage respect — 27
    // slots, the same lid animation, the same spill on break — that vanilla makes
    // a distinct block so it can emit a redstone signal proportional to how many
    // players have it open. It hosts its own block-entity type (TrappedChest)
    // rather than the chest's, so the two never share storage and the redstone
    // output (deferred: BE3 lands identity + container + save, not the signal)
    // can key on the block. Reuses BlockModel::Chest, so it renders through the
    // chest path with no new model.
    TrappedChest,
    // WG-0 base blocks: the nether/end terrain palette registered as identity
    // only. WG-2/3 place these; WG-0 gives them a BlockId, properties and a
    // `minecraft:*` alias so the generator has something to place and JC import
    // maps straight onto them. No new behaviour — they are plain solid cubes
    // (Magma emits light 3, the rest inert), mirroring the 1.16.1 base blocks.
    //
    // Nether:
    SoulSand,
    SoulSoil,
    NetherQuartzOre,
    MagmaBlock,
    Basalt,
    Blackstone,
    NetherBricks,
    NetherWartBlock,
    CrimsonNylium,
    WarpedNylium,
    // End:
    EndStone,
    PurpurBlock,
    // AR-B2: the first stair/door/fence-gate species (oak), proving the
    // multi-box shape + neighbour-derived state mechanism the same way OakSlab
    // led the slab family. Later species reuse the same BlockModel and
    // updateShape handlers — adding one is a registry row, not new logic.
    OakStairs,
    OakDoor,
    OakFenceGate,
    // AR-B3: the second wave of shaped/interactive blocks — a trapdoor (single
    // cell, door-style thin box that swings between horizontal-closed and
    // vertical-open), a pressure plate (thin column, entity-triggered), and a
    // stone/cobblestone pair proving the button and wall mechanisms
    // (StoneButton already carried identity + redstone timing from the W-4/5
    // slice; this pass gives it a shape, a placement and a press interaction —
    // see the block's own comment below).
    OakTrapdoor,
    StonePressurePlate,
    CobblestoneWall,
    Count,
};

// The number of built-in blocks — the size every compile-time built-in table
// (random tick, state metadata) is cut to. It equals `blockRegistry().size()`
// for a build with no external content; once the External phase can add blocks
// (R0-5), the runtime tables (block tags, the save palette) size to the registry
// instead so they grow past this, while the constexpr built-in tables stay this
// wide because only built-in blocks have compile-time behaviour to bake.
inline constexpr std::size_t kBuiltinBlockCount = static_cast<std::size_t>(Block::Count);

// The bridge between the enum handle and the runtime identity. A built-in
// block's BlockId is its enum ordinal (BlockRegistry.hpp asserts this on load),
// so both directions are a cast, not a registry lookup — cheap enough to sit on
// any path the enum used to.
[[nodiscard]] constexpr BlockId blockId(Block block) {
    return BlockId::of(static_cast<BlockId::Value>(block));
}
[[nodiscard]] constexpr Block blockFromId(BlockId id) {
    return static_cast<Block>(id.value());
}

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
    // A SlabBlock: a half-height box whose SlabType property places it in the
    // bottom or top half of the cell, or fills the whole cell (double). The
    // mesher reads the property to pick the box; a double slab meshes as a full
    // cube.
    Slab,
    // A StairBlock: a Boxes shape built from Facing x Half x StairShape, ported
    // from StairBlock's five fixed VoxelShapes (AR-B2).
    Stairs,
    // A DoorBlock: two cells (Half::Bottom/Top standing in for
    // DoubleBlockHalf.LOWER/UPPER — the same two-value axis, read through
    // BlockState::doorHalf) sharing one thin Boxes shape that swings with
    // Facing x Open x Hinge (AR-B2).
    Door,
    // A FenceGateBlock: a Boxes shape on the Facing axis that empties when Open
    // (AR-B2).
    FenceGate,
    // A TrapDoorBlock (AR-B3): the same thin-leaf box family a door uses, but
    // one cell, and the closed orientation lies flat (Half decides top/bottom
    // face) rather than always standing on the facing-axis wall. Facing x Half
    // x Open (no Hinge — a trapdoor has none).
    TrapDoor,
    // A BasePressurePlateBlock (AR-B3): a thin full-footprint column, two
    // heights (raised/pressed) keyed off Powered — no Boxes shape needed, the
    // Column kind already answers "how tall", the same way a slab does.
    PressurePlate,
    // A ButtonBlock (AR-B3): a small Boxes shape on the Facing axis (wall-
    // mounted only, matching Lever's existing simplification), whose box
    // shrinks slightly while Powered.
    Button,
    // A WallBlock (AR-B3): a Boxes shape assembled from a centre post plus one
    // arm per connected side (WallNorth/East/South/West), the same
    // mask-indexed-table shape a fence's connection mechanism would use.
    Wall,
};

// SlabBlock.TYPE, the value the SlabType property serialises as. Bottom is 0 so
// a freshly placed slab (the block's default state) sits in the lower half, the
// way vanilla's SlabType.BOTTOM is the default.
enum class SlabPortion : std::uint8_t {
    Bottom,
    Top,
    Double,
};

// StairBlock.SHAPE (StairsShape): Straight is 0 so a freshly placed stair with
// no stair neighbour yet (the common case) is its default state, matching
// vanilla's StairsShape.STRAIGHT default. Derived by updateShape from the two
// horizontal neighbours along the stair's facing axis; never placed by hand.
enum class StairShape : std::uint8_t {
    Straight,
    InnerLeft,
    InnerRight,
    OuterLeft,
    OuterRight,
};

// DoorBlock.HINGE (DoorHingeSide): Left is 0, matching vanilla's
// DoorHingeSide.LEFT default. Decided once at placement (DoorBlock#getHinge);
// never recomputed afterward.
enum class DoorHinge : std::uint8_t {
    Left,
    Right,
};

// StateProperty::SubmergedFluid's value, the vanilla `waterlogged` axis
// generalised to a small closed enum (F-2-submerged-fluid-axis.md). None is 0
// so a freshly placed submergible block's default state is dry, the way
// vanilla's `waterlogged=false` is the default. `Water` is 1 — this exact
// numbering is a load-bearing constant: compat/VanillaMapping.hpp's
// `waterlogged` override (JC1) hard-codes false->0/true->1 ahead of this
// enumerator existing, so it must not be renumbered without also updating
// `detail::waterloggedToSubmergedIn` there. `Lava` is reserved (F1's decision
// to leave a slot for it) but no block declares three values on this axis yet
// — F2 is water-only, "waterlogged对等".
enum class SubmergedFluid : std::uint8_t {
    None,
    Water,
    Lava,
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
    // The block entity this block hosts, invalid when it hosts none. Placing or
    // breaking the block reads this in one subscript to learn whether — and which
    // — block entity to build or destroy, instead of each system testing the
    // block identity (BlockEntityType.hpp). Set by the builder's blockEntity();
    // `hasBlockEntity(block)` is the derived pre-filter over it. The lifecycle
    // that acts on it stays the caller's (BE2's unified entry); BE1 only makes the
    // mapping a single indexed load.
    core::BlockEntityTypeId blockEntityType{};
    bool torch = false;
    // A LeavesBlock: decays when no log is left within six blocks of it, unless
    // a player placed it. See BlockState::persistent().
    bool leaves = false;
    // AR-CI: the creative-inventory tab this block is filed under, declared by
    // the builder's creative() below. Defaults to Hidden — a block that never
    // opts in is technical/unobtainable (Air, fluids, wall-mounted duplicate
    // variants, piston heads, the upper half of a two-cell block, ...) and
    // ContentRegistry::registerBlock skips it rather than listing it under some
    // fallback tab. This is the single source of catalog membership: no
    // parallel hand-maintained array names which blocks are reachable.
    CreativeCategory creativeCategory = CreativeCategory::Hidden;
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
    // AR-CI: declares the creative-inventory tab this block is filed under,
    // mirroring gameplay::Item::category(). A block that never calls this stays
    // CreativeCategory::Hidden (the field's default) and is technical/
    // unobtainable — registerBlock's single registry pass skips it.
    [[nodiscard]] constexpr BlockProperties creative(CreativeCategory category) const {
        BlockProperties copy = *this;
        copy.definition_.creativeCategory = category;
        return copy;
    }
    // The block entity this block hosts (BlockEntityType.Builder.of(factory,
    // blocks...) in vanilla, where the block names the entity type). Bakes the
    // built-in kind's dense id, so placement/break read the kind in one subscript.
    [[nodiscard]] constexpr BlockProperties blockEntity(BlockEntityKind kind) const {
        BlockProperties copy = *this;
        copy.definition_.blockEntityType = blockEntityTypeId(kind);
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

    // A SlabBlock: the Slab model plus its SlabType axis (bottom/top/double).
    // Cutout is wrong for a slab (its box has solid faces), so it keeps the
    // opaque layer; the mesher and collision read the SlabType to place the box,
    // and a double slab behaves as a full cube.
    [[nodiscard]] constexpr BlockProperties slab() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::Slab).state(StateProperty::SlabType, 3U);
    }

    // SimpleWaterloggedBlock: declares the SubmergedFluid axis (F2). Two values
    // (none/water) — the lava slot SubmergedFluid reserves is not opted into by
    // this helper, matching F1's "water first, lava gated" decision. A block
    // that has not called this cannot hold a parasitic fluid source at all: its
    // SubmergedFluid read is the schema's own "absent property reads back as
    // 0/none" default (StateSchema.hpp), so nothing downstream has to check
    // `canBeSubmerged` before reading `submergedFluid()` — only before *writing*
    // it, which is exactly the prefilter's job (place/break/bucket hooks).
    [[nodiscard]] constexpr BlockProperties submerges() const {
        return state(StateProperty::SubmergedFluid, 2U);
    }

    // A StairBlock (AR-B2): the Stairs model plus its Facing x Half x
    // StairShape axes. Cutout, like the vanilla stair — its box has open
    // corners a translucent-style depth sort is not needed for, but it is not a
    // full cube either.
    [[nodiscard]] constexpr BlockProperties stairs() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::Stairs)
            .renderLayer(BlockRenderLayer::Cutout)
            .horizontalFacing()
            .state(StateProperty::Half, 2U)
            .state(StateProperty::StairShape, 5U);
    }

    // A DoorBlock (AR-B2): the Door model plus Facing x Half x Open x Hinge. A
    // door is placed as two cells sharing one block identity (BE2's atomic
    // two-cell write, not this file's concern); the schema only says what one
    // cell's state can hold.
    // Collision stays on (the default): a door's thin Boxes shape supplies its
    // real collision box, not a full cube — noCollision() would zero out
    // collisionSpan entirely (hasCollision(block) gates it before the shape is
    // even read), which is wrong for a solid leaf a creature must walk around.
    [[nodiscard]] constexpr BlockProperties door() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::Door)
            .renderLayer(BlockRenderLayer::Cutout)
            .horizontalFacing()
            .state(StateProperty::Half, 2U)
            .state(StateProperty::Open, 2U)
            .state(StateProperty::Hinge, 2U);
    }

    // A FenceGateBlock (AR-B2): the FenceGate model plus Facing x Open.
    // Collision likewise stays on; shapeFenceGate answers Empty itself when
    // Open, which is the correct way to represent "swung fully clear" (a
    // per-state fact collisionSpan can see), not a per-block noCollision that
    // would leave a *closed* gate equally walkable.
    [[nodiscard]] constexpr BlockProperties fenceGate() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::FenceGate)
            .renderLayer(BlockRenderLayer::Cutout)
            .horizontalFacing()
            .state(StateProperty::Open, 2U);
    }

    // A TrapDoorBlock (AR-B3, redstone sink wired in W-signal): the TrapDoor
    // model plus Facing x Half x Open x Powered. Powered is not rendered/shaped
    // (the mesher and BlockShape only ever read Open) — it exists purely so
    // TrapDoorBlock.neighborChanged's "signal != POWERED" edge check has
    // somewhere to remember the last signal it saw, exactly as vanilla stores it
    // alongside OPEN rather than re-deriving it every neighbour notification.
    // Collision stays on — its thin box, like a door's, is the real collision
    // source (noCollision() would zero collisionSpan entirely).
    [[nodiscard]] constexpr BlockProperties trapdoor() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::TrapDoor)
            .renderLayer(BlockRenderLayer::Cutout)
            .horizontalFacing()
            .state(StateProperty::Half, 2U)
            .state(StateProperty::Open, 2U)
            .state(StateProperty::Powered, 2U);
    }

    // A BasePressurePlateBlock (AR-B3): the PressurePlate model plus Powered,
    // read as the shape's raised/pressed column height and the mesher's
    // pressed-texture bit alike (BasePressurePlateBlock.SHAPE/SHAPE_PRESSED).
    // Collision stays on; the plate itself does not require a wall/ground
    // flag here since canBlockSurvive routes a plain BlockSupport::Ground.
    [[nodiscard]] constexpr BlockProperties pressurePlate() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::PressurePlate)
            .renderLayer(BlockRenderLayer::Cutout)
            .support(BlockSupport::Ground)
            .state(StateProperty::Powered, 2U);
    }

    // A ButtonBlock (AR-B3): the Button model plus Facing x Powered, wall-
    // mounted only (Lever's existing simplification — vanilla's FACE axis for
    // floor/ceiling attachment is not carried here). Collision stays on — a
    // button's small box is real collision (an arrow can rest on it, a player
    // can stand on a floor one), the same door/gate lesson AR-B2 already
    // learned the hard way: noCollision() zeroes collisionSpan entirely
    // regardless of the shape a state answers, which would be wrong here too.
    [[nodiscard]] constexpr BlockProperties button() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::Button)
            .renderLayer(BlockRenderLayer::Cutout)
            .support(BlockSupport::Wall)
            .state(StateProperty::Facing, 6U)
            .state(StateProperty::Powered, 2U);
    }

    // A WallBlock (AR-B3): the Wall model plus the four per-side connection
    // booleans (WallNorth/East/South/West). Collision stays on; the taller
    // (1.5-cell) collision-vs-visual split a full vanilla wall has is folded
    // into one shape here (see BlockShape.hpp's wall box table comment) since
    // this pass does not carry the LOW/TALL distinction.
    [[nodiscard]] constexpr BlockProperties wall() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::Wall)
            .renderLayer(BlockRenderLayer::Cutout)
            .state(StateProperty::WallNorth, 2U)
            .state(StateProperty::WallEast, 2U)
            .state(StateProperty::WallSouth, 2U)
            .state(StateProperty::WallWest, 2U);
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
        .soil()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Dirt, "dirt", "Dirt")
        .texture("dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Stone, "stone", "Stone")
        .texture("stone")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Cobblestone, "cobblestone", "Cobblestone")
        .texture("cobblestone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::OakPlanks, "oak_planks", "Oak Planks")
        .texture("oak_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::OakLog, "oak_log", "Oak Log")
        .texture("oak_log_top", "oak_log", "oak_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Bricks, "bricks", "Bricks")
        .texture("bricks")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Bedrock, "bedrock", "Bedrock")
        .texture("bedrock")
        .unbreakable(3'600'000.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Sand, "sand", "Sand")
        .texture("sand")
        .strength(0.5F)
        .gravity()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Glass, "glass", "Glass")
        .texture("glass")
        .strength(0.3F)
        .renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::CoalOre, "coal_ore", "Coal Ore")
        .texture("coal_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::IronOre, "iron_ore", "Iron Ore")
        .texture("iron_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::GoldOre, "gold_ore", "Gold Ore")
        .texture("gold_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::DiamondOre, "diamond_ore", "Diamond Ore")
        .texture("diamond_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    // Material.REPLACEABLE_PLANT: placing a block inside tall grass replaces it.
    BlockProperties::of(Block::GrassPlant, "short_grass", "Short Grass")
        .texture("short_grass")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::Dandelion, "dandelion", "Dandelion")
        .texture("dandelion")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::OakSapling, "oak_sapling", "Oak Sapling")
        .texture("oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::OakLeaves, "oak_leaves", "Oak Leaves")
        .texture("oak_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
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
        .gravity()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SprucePlanks, "spruce_planks", "Spruce Planks")
        .texture("spruce_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BirchPlanks, "birch_planks", "Birch Planks")
        .texture("birch_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SpruceLog, "spruce_log", "Spruce Log")
        .texture("spruce_log_top", "spruce_log", "spruce_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BirchLog, "birch_log", "Birch Log")
        .texture("birch_log_top", "birch_log", "birch_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Bookshelf, "bookshelf", "Bookshelf")
        .texture("oak_planks", "bookshelf", "oak_planks")
        .strength(1.5F)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::CraftingTable, "crafting_table", "Crafting Table")
        .texture("crafting_table_top", "crafting_table_side", "oak_planks")
        .strength(2.5F)
        .container(ContainerType::CraftingTable)
        .creative(CreativeCategory::Functional),
    // One furnace, lit or not: AbstractFurnaceBlock's LIT is a state, so a
    // burning furnace is the same block and keeps its block entity (and its
    // smelt) across the swap. The lit front is picked by the mesher
    // (kFurnaceFrontOnLayer); light 13 is what the burning state emits.
    BlockProperties::of(Block::Furnace, "furnace", "Furnace")
        .texture("furnace_top", "furnace_side", "furnace_top")
        .strength(3.5F)
        .horizontalFacing()
        .lit(13U)
        .container(ContainerType::Furnace)
        .blockEntity(BlockEntityKind::Furnace)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::Obsidian, "obsidian", "Obsidian")
        .texture("obsidian")
        .strength(50.0F, 1'200.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Clay, "clay", "Clay")
        .texture("clay")
        .strength(0.6F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SnowBlock, "snow_block", "Snow Block")
        .texture("snow")
        .strength(0.2F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Netherrack, "netherrack", "Netherrack")
        .texture("netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Glowstone, "glowstone", "Glowstone")
        .texture("glowstone")
        .strength(0.3F)
        .light(15U)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::WhiteWool, "white_wool", "White Wool")
        .texture("white_wool")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::RedWool, "red_wool", "Red Wool")
        .texture("red_wool")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BlackWool, "black_wool", "Black Wool")
        .texture("black_wool")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::StoneBricks, "stone_bricks", "Stone Bricks")
        .texture("stone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MossyCobblestone, "mossy_cobblestone", "Mossy Cobblestone")
        .texture("mossy_cobblestone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Sandstone, "sandstone", "Sandstone")
        .texture("sandstone_top", "sandstone", "sandstone_bottom")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Pumpkin, "pumpkin", "Pumpkin")
        .texture("pumpkin_top", "pumpkin_side", "pumpkin_top")
        .strength(1.0F)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::Melon, "melon", "Melon")
        .texture("melon_top", "melon_side", "melon_top")
        .strength(1.0F)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::Tnt, "tnt", "TNT")
        .texture("tnt_top", "tnt_side", "tnt_bottom")
        .instantBreak()
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::Granite, "granite", "Granite")
        .texture("granite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Diorite, "diorite", "Diorite")
        .texture("diorite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Andesite, "andesite", "Andesite")
        .texture("andesite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CoarseDirt, "coarse_dirt", "Coarse Dirt")
        .texture("coarse_dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Podzol, "podzol", "Podzol")
        .texture("podzol_top", "podzol_side", "dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::RedSand, "red_sand", "Red Sand")
        .texture("red_sand")
        .strength(0.5F)
        .gravity()
        .creative(CreativeCategory::BuildingBlocks),
    // Carved cells below y=11 become lava (CaveCarver#carveAtPoint). Rendered
    // as a solid self-lit cube for now; it carries no fluid simulation. The
    // top face uses the animated still strip and the sides the animated flow
    // strip. Their bases/counts come from BlockAtlasLayout.hpp so resource-pack
    // animation data cannot drift away from shader literals.
    // AR-CI: no .creative() — a fluid source is world-generated/bucket-placed
    // only, never a block item in the creative catalog (vanilla LAVA has no
    // BlockItem either); stays Hidden.
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
        .torch()
        .creative(CreativeCategory::Functional),
    // WallTorchBlock's FACING is a state, not four blocks. AR-CI: no
    // .creative() — this is the placed-against-a-wall variant of Torch, reached
    // only through StandingAndWallBlockItem placement (blockItemFor's torch
    // special case), never its own catalog entry — exactly the "duplicate/
    // placed-only state" the task calls out to keep Hidden.
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
        .container(ContainerType::Chest)
        .blockEntity(BlockEntityKind::Chest)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::LapisOre, "lapis_ore", "Lapis Lazuli Ore")
        .texture("lapis_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    // AR-CI: kept in Functional (its pre-existing tab from the old hand-array)
    // per the no-regression rule, even though vanilla files redstone_ore's item
    // under naturalBlocks — moving it to the new Redstone tab is a judgment
    // call this pass declines to make unasked.
    BlockProperties::of(Block::RedstoneOre, "redstone_ore", "Redstone Ore")
        .texture("redstone_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::EmeraldOre, "emerald_ore", "Emerald Ore")
        .texture("emerald_ore")
        .strength(3.0F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::MossyStoneBricks, "mossy_stone_bricks", "Mossy Stone Bricks")
        .texture("mossy_stone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledStoneBricks, "chiseled_stone_bricks",
                        "Chiseled Stone Bricks")
        .texture("chiseled_stone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::QuartzBlock, "quartz_block", "Block of Quartz")
        .texture("quartz_block_top", "quartz_block_side", "quartz_block_top")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::JungleLog, "jungle_log", "Jungle Log")
        .texture("jungle_log_top", "jungle_log", "jungle_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::JunglePlanks, "jungle_planks", "Jungle Planks")
        .texture("jungle_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::AcaciaLog, "acacia_log", "Acacia Log")
        .texture("acacia_log_top", "acacia_log", "acacia_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::AcaciaPlanks, "acacia_planks", "Acacia Planks")
        .texture("acacia_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DarkOakLog, "dark_oak_log", "Dark Oak Log")
        .texture("dark_oak_log_top", "dark_oak_log", "dark_oak_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DarkOakPlanks, "dark_oak_planks", "Dark Oak Planks")
        .texture("dark_oak_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SpruceLeaves, "spruce_leaves", "Spruce Leaves")
        .texture("spruce_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::BirchLeaves, "birch_leaves", "Birch Leaves")
        .texture("birch_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::JungleLeaves, "jungle_leaves", "Jungle Leaves")
        .texture("jungle_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::AcaciaLeaves, "acacia_leaves", "Acacia Leaves")
        .texture("acacia_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::DarkOakLeaves, "dark_oak_leaves", "Dark Oak Leaves")
        .texture("dark_oak_leaves")
        .leaves()
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::SpruceSapling, "spruce_sapling", "Spruce Sapling")
        .texture("spruce_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::BirchSapling, "birch_sapling", "Birch Sapling")
        .texture("birch_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::JungleSapling, "jungle_sapling", "Jungle Sapling")
        .texture("jungle_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::AcaciaSapling, "acacia_sapling", "Acacia Sapling")
        .texture("acacia_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
    BlockProperties::of(Block::DarkOakSapling, "dark_oak_sapling", "Dark Oak Sapling")
        .texture("dark_oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::Decoration),
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
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedDiorite, "polished_diorite", "Polished Diorite")
        .texture("polished_diorite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedAndesite, "polished_andesite", "Polished Andesite")
        .texture("polished_andesite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SmoothStone, "smooth_stone", "Smooth Stone")
        .texture("smooth_stone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Slabs: each mirrors its parent block's texture and hardness. The SlabType
    // property (bottom/top/double) is declared by slab(); breaking a double slab
    // yields two slab items (MiningSystem), a single slab yields one.
    // SimpleWaterloggedBlock: a slab is the first (and, until AR-B2/AR-B's stair
    // and fence land, only) block that declares submerges() — F2's scope is
    // limited to slabs on purpose (F-2-submerged-fluid-axis.md's "台阶先行").
    BlockProperties::of(Block::OakSlab, "oak_slab", "Oak Slab")
        .texture("oak_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SpruceSlab, "spruce_slab", "Spruce Slab")
        .texture("spruce_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BirchSlab, "birch_slab", "Birch Slab")
        .texture("birch_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::JungleSlab, "jungle_slab", "Jungle Slab")
        .texture("jungle_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::AcaciaSlab, "acacia_slab", "Acacia Slab")
        .texture("acacia_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DarkOakSlab, "dark_oak_slab", "Dark Oak Slab")
        .texture("dark_oak_planks")
        .strength(2.0F, 3.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::StoneSlab, "stone_slab", "Stone Slab")
        .texture("stone")
        .strength(1.5F, 6.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CobblestoneSlab, "cobblestone_slab", "Cobblestone Slab")
        .texture("cobblestone")
        .strength(2.0F, 6.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::StoneBrickSlab, "stone_brick_slab", "Stone Brick Slab")
        .texture("stone_bricks")
        .strength(1.5F, 6.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SmoothStoneSlab, "smooth_stone_slab", "Smooth Stone Slab")
        .texture("smooth_stone")
        .strength(2.0F, 6.0F)
        .slab()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    // RedstoneBlock: a full solid cube that is a constant redstone source. The
    // power itself is not a property — it is answered by the signal table for
    // every side — so the block needs no extra state.
    BlockProperties::of(Block::RedstoneBlock, "redstone_block", "Block of Redstone")
        .texture("redstone_block")
        .strength(5.0F, 6.0F)
        .creative(CreativeCategory::Redstone),
    // RedstoneTorch: mounts on the ground like a torch and carries a LIT state
    // (default handled by the placement/component layer, vanilla default true).
    // Emits light 7 while lit; the redstone signal comes from the signal table,
    // not the light level.
    BlockProperties::of(Block::RedstoneTorch, "redstone_torch", "Redstone Torch")
        .texture("redstone_torch")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .support(BlockSupport::Ground)
        .torch()
        .lit(7U)
        .creative(CreativeCategory::Redstone),
    // RedstoneWallTorch: the wall-mounted variant, FACING as a state exactly like
    // WallTorch, plus the LIT state. AR-CI: no .creative() — same "placed-
    // against-a-wall duplicate variant" as WallTorch, reached only through
    // StandingAndWallBlockItem placement, never its own catalog entry.
    BlockProperties::of(Block::RedstoneWallTorch, "redstone_wall_torch", "Redstone Wall Torch")
        .vanillaAlias("redstone_wall_torch")
        .texture("redstone_torch")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .support(BlockSupport::Wall)
        .horizontalFacing()
        .torch()
        .lit(7U),
    // Lever: FACING is stored as the full six directions (getConnectedDirection
    // can be UP/DOWN for a floor/ceiling lever or a horizontal for a wall one),
    // plus the POWERED toggle. Wall-mounted in this slice.
    BlockProperties::of(Block::Lever, "lever", "Lever")
        .texture("lever")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .noCollision()
        .support(BlockSupport::Wall)
        .state(StateProperty::Facing, 6U)
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    // Repeater: horizontal FACING, a DELAY of 1-4 ticks, and a POWERED output.
    // The Torch model is a placeholder that keeps it out of the full-cube (and so
    // the redstone-conductor) set until a repeater model lands in the renderer.
    BlockProperties::of(Block::Repeater, "repeater", "Redstone Repeater")
        .texture("repeater")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .support(BlockSupport::Ground)
        .horizontalFacing()
        .state(StateProperty::Delay, 4U)
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    // Comparator: horizontal FACING, a MODE (compare/subtract), a POWERED
    // boolean output and a 0-15 AnalogSignal output. Torch-model placeholder as
    // for the repeater.
    BlockProperties::of(Block::Comparator, "comparator", "Redstone Comparator")
        .texture("comparator")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .support(BlockSupport::Ground)
        .horizontalFacing()
        .state(StateProperty::ComparatorMode, 2U)
        .state(StateProperty::Powered, 2U)
        .state(StateProperty::AnalogSignal, 16U)
        .creative(CreativeCategory::Redstone),
    // Redstone dust: a flat wire carrying POWER 0-15 in its AnalogSignal. Torch
    // model placeholder keeps it non-full-cube; needs a sturdy floor.
    BlockProperties::of(Block::RedstoneWire, "redstone_wire", "Redstone Dust")
        .texture("redstone_dust_line")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Torch)
        .noCollision()
        .support(BlockSupport::Ground)
        .state(StateProperty::AnalogSignal, 16U)
        .creative(CreativeCategory::Redstone),
    // Observer: FACING is the six-way watched direction, POWERED the pulse. Torch
    // model placeholder keeps it out of the redstone-conductor set.
    BlockProperties::of(Block::Observer, "observer", "Observer")
        .texture("observer")
        .strength(3.0F)
        .model(BlockModel::Torch)
        .state(StateProperty::Facing, 6U)
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    // Stone button: like the lever (attach + POWERED), but a press is timed.
    // AR-B3: gains a real shape/collision through .button() — it used to sit
    // on the Torch placeholder model with noCollision(), which predates this
    // pass giving it its own shape family (see BlockShape.hpp's shapeButton).
    BlockProperties::of(Block::StoneButton, "stone_button", "Stone Button")
        .texture("stone")
        .instantBreak()
        .button()
        .creative(CreativeCategory::Redstone),
    // Piston: a full-cube block with a six-way FACING and EXTENDED in POWERED.
    BlockProperties::of(Block::Piston, "piston", "Piston")
        .texture("piston_side")
        .strength(1.5F)
        .state(StateProperty::Facing, 6U)
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::StickyPiston, "sticky_piston", "Sticky Piston")
        .texture("piston_side")
        .strength(1.5F)
        .state(StateProperty::Facing, 6U)
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    // Identical to the chest in every rendered and stored respect (same texture,
    // model, container UI and horizontal facing), differing only in identity: it
    // hosts the TrappedChest block-entity type, so its storage and save section
    // are its own. The redstone output is deferred (BE3), so nothing here yet
    // distinguishes its behaviour from a chest.
    BlockProperties::of(Block::TrappedChest, "trapped_chest", "Trapped Chest")
        .texture("chest", "chest", "chest")
        .strength(2.5F)
        .renderLayer(BlockRenderLayer::Cutout)
        .model(BlockModel::Chest)
        .horizontalFacing()
        .container(ContainerType::Chest)
        .blockEntity(BlockEntityKind::TrappedChest)
        .creative(CreativeCategory::Redstone),
    // WG-0 nether base blocks. Strengths and the magma light level mirror
    // 1.16.1/26.1 Blocks.java; textures name the vanilla sprites so the
    // name-driven atlas build resolves them. These are identity only — placement
    // is WG-2, playable content (nether-brick building, nether wart farming)
    // is AR-B.
    // AR-CI judgment call: WG-0's own comment calls these "identity only", but
    // NetherGenerator.cpp/EndGenerator.cpp already place every one of them in
    // generated terrain and MiningSystem breaks/drops them like any other
    // block — nothing here is placed-only or a duplicate variant, so they are
    // filed as obtainable BuildingBlocks (matching how the pre-existing array
    // grouped Netherrack/Sand/Granite as "natural building material") rather
    // than left Hidden pending WG-2/WG-3's full dimension reachability.
    BlockProperties::of(Block::SoulSand, "soul_sand", "Soul Sand")
        .texture("soul_sand")
        .strength(0.5F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SoulSoil, "soul_soil", "Soul Soil")
        .texture("soul_soil")
        .strength(0.5F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::NetherQuartzOre, "nether_quartz_ore", "Nether Quartz Ore")
        .texture("nether_quartz_ore")
        .strength(3.0F, 3.0F)
        .creative(CreativeCategory::Functional),
    // MagmaBlock: a full solid cube that glows at light level 3 (vanilla
    // lightLevel(3)). The damage-on-standing behaviour is AR-B, not WG-0.
    BlockProperties::of(Block::MagmaBlock, "magma_block", "Magma Block")
        .texture("magma")
        .strength(0.5F)
        .light(3U)
        .creative(CreativeCategory::BuildingBlocks),
    // Basalt: a RotatedPillarBlock in vanilla (top/side end grain), so it takes
    // the axis of the face it is placed against, like a log.
    BlockProperties::of(Block::Basalt, "basalt", "Basalt")
        .texture("basalt_top", "basalt_side", "basalt_top")
        .strength(1.25F, 4.2F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Blackstone, "blackstone", "Blackstone")
        .texture("blackstone_top", "blackstone", "blackstone_top")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::NetherBricks, "nether_bricks", "Nether Bricks")
        .texture("nether_bricks")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::NetherWartBlock, "nether_wart_block", "Nether Wart Block")
        .texture("nether_wart_block")
        .strength(1.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CrimsonNylium, "crimson_nylium", "Crimson Nylium")
        .texture("crimson_nylium", "crimson_nylium_side", "netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WarpedNylium, "warped_nylium", "Warped Nylium")
        .texture("warped_nylium", "warped_nylium_side", "netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::BuildingBlocks),
    // WG-0 end base blocks. WG-3 places these; playable content stays AR-B.
    BlockProperties::of(Block::EndStone, "end_stone", "End Stone")
        .texture("end_stone")
        .strength(3.0F, 9.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PurpurBlock, "purpur_block", "Purpur Block")
        .texture("purpur_block")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // AR-B2: the first stair/door/fence-gate species. Hardness/blast resistance
    // mirror oak_planks, the parent block each recipe crafts from.
    // F2 extension (this pass): StairBlock is `implements SimpleWaterloggedBlock`
    // in vanilla 26.1 (StairBlock.java:32, WATERLOGGED field at :39) — stairs
    // submerge like slabs.
    BlockProperties::of(Block::OakStairs, "oak_stairs", "Oak Stairs")
        .texture("oak_planks")
        .strength(2.0F, 3.0F)
        .stairs()
        .submerges()
        .creative(CreativeCategory::BuildingBlocks),
    // DoorBlock: `top`/`side` name the two vanilla door sprites (the upper and
    // lower halves), reused here as the top/side texture slots the mesher's
    // per-half box lookup reads — a door has no bottom face on either half, so
    // the third slot goes unused like a cross plant's.
    // F2 extension: DoorBlock does *not* implement SimpleWaterloggedBlock in
    // vanilla (`class DoorBlock extends Block` — no WATERLOGGED property
    // anywhere in DoorBlock.java) — doors deliberately do not call
    // .submerges(). Do not add it; a two-cell block occupying both a solid
    // and an air-ish cell has no vanilla waterlogged precedent to copy.
    BlockProperties::of(Block::OakDoor, "oak_door", "Oak Door")
        .texture("oak_door_top", "oak_door_bottom", "oak_door_bottom")
        .strength(3.0F)
        .door()
        .creative(CreativeCategory::BuildingBlocks),
    // F2 extension: FenceGateBlock also does *not* implement
    // SimpleWaterloggedBlock in vanilla (`class FenceGateBlock extends
    // HorizontalDirectionalBlock` — no WATERLOGGED property in
    // FenceGateBlock.java, unlike FenceBlock/WallBlock which route through
    // CrossCollisionBlock's SimpleWaterloggedBlock). Confirmed directly
    // against the 26.1 source, contra the initial task assumption that fence
    // gates were waterloggable like fences/walls — they are not. No
    // .submerges() here either.
    BlockProperties::of(Block::OakFenceGate, "oak_fence_gate", "Oak Fence Gate")
        .texture("oak_planks")
        .strength(2.0F, 3.0F)
        .fenceGate()
        .creative(CreativeCategory::BuildingBlocks),
    // AR-B3: TrapDoorBlock `implements SimpleWaterloggedBlock` in vanilla
    // (TrapDoorBlock.java:43) — unlike the door, a trapdoor *does* submerge.
    // Left un-opted for this pass (matching AR-B2's own stated boundary: F2
    // extension is a separate, deliberate pass per block family) — recorded
    // here as the same "interface ready, not yet wired" note AR-B2 left doors.
    BlockProperties::of(Block::OakTrapdoor, "oak_trapdoor", "Oak Trapdoor")
        .texture("oak_trapdoor", "oak_trapdoor", "oak_trapdoor")
        .strength(3.0F)
        .trapdoor()
        .creative(CreativeCategory::BuildingBlocks),
    // BasePressurePlateBlock: not a SimpleWaterloggedBlock in vanilla either
    // (PressurePlateBlock.java has no WATERLOGGED property) — no .submerges().
    BlockProperties::of(Block::StonePressurePlate, "stone_pressure_plate", "Stone Pressure Plate")
        .texture("stone")
        .instantBreak()
        .pressurePlate()
        .creative(CreativeCategory::BuildingBlocks),
    // WallBlock: the first cobblestone-family wall, proving the four-side
    // connection mechanism the way OakStairs proved the join-shape mechanism.
    BlockProperties::of(Block::CobblestoneWall, "cobblestone_wall", "Cobblestone Wall")
        .texture("cobblestone")
        .strength(2.0F, 6.0F)
        .wall()
        .creative(CreativeCategory::BuildingBlocks),
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

// A SlabBlock: its box is a half of the cell (or the whole cell when double),
// decided by the state's SlabType property rather than the block identity.
[[nodiscard]] constexpr bool isSlab(Block block) {
    return blockDefinition(block).model == BlockModel::Slab;
}

// SimpleWaterloggedBlock's own prefilter: can this block hold a parasitic
// fluid source at all? Derived from the schema (`.submerges()`'s axis) rather
// than a second hand-set flag, so there is exactly one place that says "this
// block can be submerged" — the state declaration itself — and no way for the
// two to disagree. The place/break/bucket hooks check this before *writing*
// SubmergedFluid (F-2-submerged-fluid-axis.md's sabotage #1: a hook that
// instead special-cases `block == Stairs` would let an unrelated future block
// "leak" water it never declared, which is exactly the bug this predicate
// exists to make impossible — there is no identity check anywhere in it).
[[nodiscard]] constexpr bool canBeSubmerged(Block block) {
    return blockDefinition(block).states.has(StateProperty::SubmergedFluid);
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
    if (block == Block::WheatCrops) return "wheat_stage0";
    if (block == Block::Carrots) return "carrots_stage0";
    if (block == Block::Potatoes) return "potatoes_stage0";
    return nullptr;
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

// The block-entity type this block hosts, invalid when it hosts none. deref =
// one subscript into the block table; the returned id derefs into the
// block-entity type registry. Chest -> chest, Furnace -> furnace, everything
// else invalid.
[[nodiscard]] constexpr core::BlockEntityTypeId blockEntityTypeOf(Block block) {
    return blockDefinition(block).blockEntityType;
}

// Java's BlockEntityType.isValid(block) compressed to a single indexed bit-test:
// does placing this block create a block entity? This is the pre-filter the
// placement/break path checks before deciding to build or destroy one, so the
// overwhelming majority of blocks (stone, dirt, ore) reject in one load without
// naming any block-entity kind.
[[nodiscard]] constexpr bool hasBlockEntity(Block block) {
    return blockDefinition(block).blockEntityType.valid();
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

// Direction#getClockWise / getCounterClockWise, restricted to the horizontal
// four (the only ones a stair/door/gate ever names). AR-B2's stair-shape and
// door-hinge derivations both need "the direction 90 degrees left/right of
// facing" — this is that primitive, ported once here rather than reimplemented
// per derivation. A non-horizontal input returns itself unchanged (Up/Down have
// no clockwise neighbour on this axis), which callers never feed it in practice
// since every consumer first filters to a horizontal facing.
[[nodiscard]] constexpr BlockOrientation clockwiseOrientation(BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::North:
        return BlockOrientation::East;
    case BlockOrientation::East:
        return BlockOrientation::South;
    case BlockOrientation::South:
        return BlockOrientation::West;
    case BlockOrientation::West:
        return BlockOrientation::North;
    case BlockOrientation::Up:
    case BlockOrientation::Down:
        return orientation;
    }
    return orientation;
}
[[nodiscard]] constexpr BlockOrientation counterClockwiseOrientation(BlockOrientation orientation) {
    // The inverse of clockwiseOrientation: three 90-degree clockwise turns is
    // one counter-clockwise turn on a four-cycle. (Applying opposite() around
    // a single clockwise turn is *not* the inverse — opposite is its own
    // inverse and commutes with the rotation, so that composition collapses
    // back to the same clockwise turn instead of undoing it.)
    return clockwiseOrientation(clockwiseOrientation(clockwiseOrientation(orientation)));
}
// Direction.Axis equality for the horizontal four: true when both directions
// share a line (North/South are one axis, East/West the other). Doors and
// gates both gate a neighbour reaction on "is this the axis facing runs
// along", the same test JE's `Direction.Axis` enum answers directly.
[[nodiscard]] constexpr bool sameHorizontalAxis(BlockOrientation a, BlockOrientation b) {
    const bool aNorthSouth = a == BlockOrientation::North || a == BlockOrientation::South;
    const bool bNorthSouth = b == BlockOrientation::North || b == BlockOrientation::South;
    return aNorthSouth == bNorthSouth;
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
