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
// The enum's underlying type is uint16_t, not uint8_t: the built-in roster has
// grown past the 256 a byte could hold (STRUCT structure-block registration), and
// the identity plumbing was already built for it — BlockId is a uint16 (see the
// `using core::BlockId` note above), and BlockStateMetadataTable stores each
// state's block as a BlockId "so a state's block can range past 256 once external
// content exists" (BlockStateTable.hpp). Widening the handle to match is the last
// piece. Blocks serialise by stable name (format 5+) and BlockState by its uint16
// rawId, so the ordinal is never written to a save or the wire — the width change
// is memory-layout only. The real remaining budget is the interned state-id space
// (kBlockStateCount < 65536, asserted in BlockStateTable.hpp), not the enum.
enum class Block : std::uint16_t {
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
    // DYE-2: the remaining 13 dyed wool colours, so every DyeColor has a wool
    // block to drop. Grouped here with the three pre-existing wools; the exact
    // enum ordinal does not matter to saves (blocks serialise by their stable
    // name in the palette, format 5+), so inserting them mid-enum never touches
    // an old save's block ids. The DyeColor->Block mapping is a constexpr table
    // (woolBlockFor, below) so a per-colour drop is a table lookup, never a
    // switch that grows a case per colour.
    OrangeWool,
    MagentaWool,
    LightBlueWool,
    YellowWool,
    LimeWool,
    PinkWool,
    GrayWool,
    LightGrayWool,
    CyanWool,
    PurpleWool,
    BlueWool,
    BrownWool,
    GreenWool,
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
    // is smelted from stone in the furnace (no crafting recipe, like vanilla).
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
    // (Magma emits light 3, the rest inert), mirroring the vanilla base blocks.
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
    // AR-CX2: sugar cane, the plant block that closes the paper -> book chain
    // (the CX1 paper recipe names rebedrock:sugar_cane). A cross-model plant
    // like a flower, but with SugarCaneBlock's own placement rule (soil/sand
    // beside water, or another sugar cane below) and a vertical random-tick
    // growth to a stack of three. Carries the Age 0-15 property vanilla's
    // SugarCaneBlock.AGE uses to pace that growth.
    SugarCane,
    // AR-CX4-b: fire, the block flint_and_steel places on a flammable/solid
    // surface. Non-solid, no drops, emits full light (FireBlock's lightLevel
    // 15), a cross-model plant-like block. Carries AGE 0-15 (FireBlock.AGE) to
    // pace its random-tick burn-out; its own Fire support rule keeps it only
    // where FireBlock#canSurvive would (a solid face below, or a flammable
    // neighbour). Enum ordinal is irrelevant to saves (blocks serialise by
    // stable name, format 5+), so appending it never touches an old save.
    Fire,
    // RN-4b content: prismarine and sea lantern — full cubes whose textures are
    // multi-frame .mcmeta strips (prismarine 4 frames, sea lantern 5), the first
    // roster blocks to exercise the generalised block-texture animation. Sea
    // lantern also emits full light. Appended before Count; saves serialise blocks
    // by stable name, so the new ordinals never touch an old save.
    Prismarine,
    SeaLantern,
    // STRUCT content: ice — a translucent full cube (igloo floors, frozen
    // surfaces). Appended before Count so existing ordinals are untouched.
    Ice,
    // STRUCT AR-B batch 1: plain full-cube (and pillar) building blocks the vanilla
    // structure templates reference. Every one is a solid cube or a
    // RotatedPillarBlock — no new BlockModel, no BlockShape/kShapeByModel change, so
    // they carry zero SIGBUS risk (that guard fires only when a new model is added).
    // Grouped by family for readability; enum order is irrelevant to saves (blocks
    // serialise by stable name, format 5+), so appending never touches an old save.
    //
    // Stone-brick variants (igloo bottom): the cracked brick and the three infested
    // silverfish blocks, which render exactly as their host brick (InfestedBlock
    // reuses the host texture). Silverfish behaviour is not modelled — structure
    // rendering only needs the cube.
    CrackedStoneBricks,
    InfestedStoneBricks,
    InfestedMossyStoneBricks,
    InfestedChiseledStoneBricks,
    // Blackstone family (bastion/ruined-portal palettes).
    PolishedBlackstoneBricks,
    CrackedPolishedBlackstoneBricks,
    PolishedBlackstone,
    ChiseledPolishedBlackstone,
    GildedBlackstone,
    // Tuff family (trail ruins).
    Tuff,
    PolishedTuff,
    TuffBricks,
    ChiseledTuff,
    ChiseledTuffBricks,
    // Deepslate family (trial chambers / deep structures). Deepslate itself is a
    // RotatedPillarBlock (axis), the rest are plain cubes.
    Deepslate,
    CobbledDeepslate,
    PolishedDeepslate,
    DeepslateBricks,
    CrackedDeepslateBricks,
    DeepslateTiles,
    CrackedDeepslateTiles,
    ChiseledDeepslate,
    // Sandstone variants (desert structures) — smooth/cut/chiseled reuse the
    // sandstone_top sprite for the faces vanilla's models do.
    SmoothSandstone,
    CutSandstone,
    ChiseledSandstone,
    // Mud family (trail ruins / mangrove).
    Mud,
    PackedMud,
    MudBricks,
    // Waxed copper full cubes (trail ruins). Only the full-cube members — the
    // stairs/slab/grate/bulb variants are deferred (need their own models).
    WaxedCopperBlock,
    WaxedExposedCopper,
    WaxedWeatheredCopper,
    WaxedOxidizedCopper,
    WaxedCutCopper,
    WaxedExposedCutCopper,
    WaxedWeatheredCutCopper,
    WaxedOxidizedCutCopper,
    WaxedChiseledCopper,
    // Metal block.
    GoldBlock,
    // Terracotta — the plain block plus all 16 dyed colours (villages/structures
    // use them heavily in aggregate). Each reuses its vanilla "*_terracotta" sprite.
    Terracotta,
    WhiteTerracotta,
    OrangeTerracotta,
    MagentaTerracotta,
    LightBlueTerracotta,
    YellowTerracotta,
    LimeTerracotta,
    PinkTerracotta,
    GrayTerracotta,
    LightGrayTerracotta,
    CyanTerracotta,
    PurpleTerracotta,
    BlueTerracotta,
    BrownTerracotta,
    GreenTerracotta,
    RedTerracotta,
    BlackTerracotta,
    // Pillar blocks (RotatedPillarBlock): a six-way FACING axis, top/side sprites.
    BoneBlock,
    PolishedBasalt,
    PurpurPillar,
    AcaciaWood,
    StrippedSpruceWood,
    StrippedSpruceLog,
    // Dirt path — a 15/16-high cube (like farmland), its own top/side sprites.
    DirtPath,
    // STRUCT AR-B batch 2: the stair / slab / wall / door / trapdoor variants the
    // structure templates reference, each reusing an existing BlockModel (Stairs /
    // Slab / Wall / Door / TrapDoor) via the matching builder helper. Adding these
    // touches no BlockModel enum and no kShapeByModel table, so — like batch 1 —
    // there is no SIGBUS risk (that guard fires only for a new model). Textures
    // reuse the parent block's sprites, exactly as OakStairs reuses oak_planks.
    // Stairs:
    AcaciaStairs,
    BirchStairs,
    DarkOakStairs,
    SpruceStairs,
    CobblestoneStairs,
    MossyCobblestoneStairs,
    StoneBrickStairs,
    BrickStairs,
    SandstoneStairs,
    SmoothSandstoneStairs,
    GraniteStairs,
    DioriteStairs,
    PurpurStairs,
    BlackstoneStairs,
    PolishedBlackstoneBrickStairs,
    MudBrickStairs,
    CobbledDeepslateStairs,
    PolishedDeepslateStairs,
    DeepslateBrickStairs,
    DeepslateTileStairs,
    WaxedCutCopperStairs,
    WaxedOxidizedCutCopperStairs,
    // Slabs:
    SandstoneSlab,
    SmoothSandstoneSlab,
    BrickSlab,
    MossyCobblestoneSlab,
    DioriteSlab,
    PurpurSlab,
    SmoothQuartzSlab,
    BlackstoneSlab,
    MudBrickSlab,
    CobbledDeepslateSlab,
    PolishedDeepslateSlab,
    DeepslateBrickSlab,
    DeepslateTileSlab,
    PolishedTuffSlab,
    WaxedCutCopperSlab,
    WaxedOxidizedCutCopperSlab,
    // Walls:
    MossyCobblestoneWall,
    StoneBrickWall,
    BrickWall,
    SandstoneWall,
    GraniteWall,
    DioriteWall,
    BlackstoneWall,
    MudBrickWall,
    CobbledDeepslateWall,
    PolishedDeepslateWall,
    DeepslateBrickWall,
    DeepslateTileWall,
    // Doors:
    SpruceDoor,
    JungleDoor,
    AcaciaDoor,
    DarkOakDoor,
    IronDoor,
    WaxedCopperDoor,
    WaxedOxidizedCopperDoor,
    // Trapdoors:
    SpruceTrapdoor,
    JungleTrapdoor,
    IronTrapdoor,
    OxidizedCopperTrapdoor,
    WaxedOxidizedCopperTrapdoor,
    // STRUCT AR-B batch 3: cross-model plants and decorations the structures
    // reference. All reuse the existing BlockModel::Cross (via .cross()), so no new
    // model and no kShapeByModel change. Flowers/grass take the same XZ per-position
    // jitter and Soil support as the existing Dandelion/Short Grass; cobweb and the
    // mushrooms sit centred (no offset). Fern/large_fern/tall_grass render untinted
    // for now (foliage-colour tint is a later fidelity pass); the block still
    // resolves and places. Double plants (tall_grass/large_fern) register once and
    // resolve both the upper and lower palette halves.
    Cobweb,
    Poppy,
    OxeyeDaisy,
    Fern,
    TallGrass,
    LargeFern,
    DeadBush,
    RedMushroom,
    BrownMushroom,
    // STRUCT AR-B batch 4: more no-new-model blocks. Full cubes (glazed terracotta
    // renders the pattern on every face — the facing rotation is a later fidelity
    // pass; stained glass is a translucent cube like Glass), pillars, and
    // pressure-plate/button variants reusing the existing PressurePlate/Button
    // models. Still no new BlockModel / kShapeByModel entry, so no SIGBUS risk.
    // Glazed terracotta (16 colours):
    WhiteGlazedTerracotta,
    OrangeGlazedTerracotta,
    MagentaGlazedTerracotta,
    LightBlueGlazedTerracotta,
    YellowGlazedTerracotta,
    LimeGlazedTerracotta,
    PinkGlazedTerracotta,
    GrayGlazedTerracotta,
    LightGrayGlazedTerracotta,
    CyanGlazedTerracotta,
    PurpleGlazedTerracotta,
    BlueGlazedTerracotta,
    BrownGlazedTerracotta,
    GreenGlazedTerracotta,
    RedGlazedTerracotta,
    BlackGlazedTerracotta,
    // Stained glass (16 colours) — translucent cubes.
    WhiteStainedGlass,
    OrangeStainedGlass,
    MagentaStainedGlass,
    LightBlueStainedGlass,
    YellowStainedGlass,
    LimeStainedGlass,
    PinkStainedGlass,
    GrayStainedGlass,
    LightGrayStainedGlass,
    CyanStainedGlass,
    PurpleStainedGlass,
    BlueStainedGlass,
    BrownStainedGlass,
    GreenStainedGlass,
    RedStainedGlass,
    BlackStainedGlass,
    // Misc full cubes.
    PackedIce,
    EndStoneBricks,
    RedstoneLamp,
    // Pillars.
    HayBlock,
    StrippedOakLog,
    // Pressure plates / button reusing existing shaped models.
    OakPressurePlate,
    AcaciaPressurePlate,
    JungleButton,
    // STRUCT AR-B batch 5: family-completing cubes / pillars / leaves (all
    // no-new-model). Rounds out the copper, deepslate, mangrove, wood and common
    // building-block families the structures still reference.
    SmoothBasalt,
    BlueIce,
    CopperBlock,
    OxidizedCutCopper,
    WaxedOxidizedChiseledCopper,
    ReinforcedDeepslate,
    Target,
    DiamondBlock,
    LapisBlock,
    CoalBlock,
    MossBlock,
    SmoothQuartz,
    WhiteConcrete,
    RedConcrete,
    InfestedCobblestone,
    // Pillars.
    SpruceWood,
    StrippedOakWood,
    StrippedAcaciaLog,
    MangroveLog,
    MangroveWood,
    MuddyMangroveRoots,
    MangroveRoots,
    // Leaves.
    MangroveLeaves,
    // STRUCT/WG terrain: copper ore plus the full deepslate ore set. The deepslate
    // variants are the y<0 deep-layer forms (harder than their stone counterparts);
    // all are plain cubes. Registered so terrain generation / JC import / structures
    // have the identities to place — same low-risk cube path as the other ores.
    CopperOre,
    DeepslateCoalOre,
    DeepslateIronOre,
    DeepslateCopperOre,
    DeepslateGoldOre,
    DeepslateRedstoneOre,
    DeepslateEmeraldOre,
    DeepslateLapisOre,
    DeepslateDiamondOre,
    // ENCH-2: the enchanting table. Appended at the tail because the properties
    // table below is indexed by this enum's ordinal — a value inserted in the
    // middle would silently hand every later block another block's definition
    // (blockRegistryIsWellFormed catches it, but at compile time only). Ordinals
    // never reach a save (states persist by name + properties), so the tail is
    // free.
    EnchantingTable,
    // ENCH-3: the anvil's three damage states. Vanilla models them as three
    // separate blocks (not a state axis) because each has its own item, its own
    // loot and its own model, and using one degrades to the next; the roster
    // follows that rather than inventing a DAMAGE property vanilla does not have.
    Anvil,
    ChippedAnvil,
    DamagedAnvil,
    // ENCH-3: the anvil's recipe needs it, and its absence is why the anvil
    // would otherwise be another uncraftable block. A plain cube.
    IronBlock,
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

// RN-8c: which cube model json a Cube/DirectionalCube block is drawn from — the
// per-face `uv` rects and `rotation` quadrants that model declares. This replaces
// a per-block array of six hand-written quarter-turns: a turn count is only half
// of what a json face says, and the observer proves it, since its up face carries
// an inverted rect (`"uv": [0,16,16,0]`) that no rotation can express.
//
// It names a MODEL, not a block: every plain cube in the roster shares Default,
// both pistons share PistonTemplate. The rects themselves live with the mesher,
// which is the only thing that needs them.
enum class CubeUvModel : std::uint8_t {
    Default,        // block/cube: every face uv [0,0,16,16], no rotation
    PistonTemplate, // template_piston.json: down 180, west 270, east 90
    Observer,       // observer.json: up face uv [0,16,16,0], no rotation
};

// RN-8c-D: which model a block's ITEM is drawn from. vanilla keeps this in
// assets/minecraft/items/<block>.json, and it is not always the block's own
// model: items/observer.json and items/furnace.json point at block/observer and
// block/furnace, but items/piston.json points at block/piston_inventory — a plain
// `cube_bottom_top` with the platform on TOP, because a piston item is not a
// piston in the world.
//
// The distinction only matters for a block whose model has faces the flat
// top/side/bottom triple cannot express. A PlainCube item takes that triple and
// nothing else; a BlockModel item takes the block's own six faces and its own
// face rotations.
enum class CubeItemModel : std::uint8_t {
    BlockModel, // items/<block>.json points at the block's own model
    PlainCube,  // it points at a cube_bottom_top variant (the pistons)
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
    // RN-4a: a full cube whose six faces carry independent textures and rotate
    // with the block's FACING (observer/piston style): the front face points along
    // FACING, the back along its opposite (swapped to an "active" sprite while
    // powered, e.g. observer_back_on), and top/bottom/side fill the rest. Unlike
    // the ElementModel diodes it is still geometrically a full cube, so isFullCube()
    // counts it (it occludes neighbours and is face-sturdy) — that is exactly what
    // was wrong while observer sat on the Torch placeholder and leaked light. The
    // furnace's *horizontal* (4-way) front is a different orientation semantics and
    // keeps its own hardcoded path for now.
    DirectionalCube,
    // RN-4a-2: a small multi-box model whose elements each carry their own texture
    // and UV rect, transcribed from a vanilla model json's `elements` — the diodes
    // (repeater/comparator: a slab base plus redstone-torch nubs) and the lever
    // (cobble base plus a 45°-tilted handle). Geometrically a decoration, never a
    // full cube: isFullCube() must stay false for it (that is the fix for the lever,
    // which used to default to Cube and be wrongly treated as a solid occluder).
    ElementModel,
    // RN-6: redstone dust — a flat wire meshed from a power-tinted centre dot plus
    // one line arm per connected neighbour (and a climbing strip up a solid side),
    // the multipart "connection mask" model. Its connections are derived from
    // neighbours at mesh time (like a fence), not stored in the state; its POWER
    // (AnalogSignal 0-15) drives the red gradient tint. Not a full cube.
    RedstoneWire,
    // RN-7: fire — neighbour-driven billowing planes (floor cross when a solid/
    // flammable block is below, a wall-hugging sheet on each flammable side, an
    // overhead sheet under a flammable ceiling), transcribed from vanilla's
    // template_fire_floor/side/up. Replaces the Cross model that could only draw
    // the two diagonal quads and never the side flames. Full-bright, no collision.
    Fire,
};

// Whether a model is a shaped block — one whose real geometry is a `BlockShape`
// box set (or the pressure plate's thin Column) rather than a full cube or a
// plant/torch/chest special case. These are exactly the models the renderer
// meshes from `BlockShape` (RN-2): stairs, doors, fence gates, trapdoors,
// buttons, walls and the pressure plate. Cube/Cross/Crop/Torch/Chest/Slab keep
// their own dedicated mesh and icon paths, so they are excluded here. A single
// predicate keeps the world mesher and the HUD icon router reading the one list
// instead of two switch statements that could drift.
[[nodiscard]] constexpr bool isShapedBlockModel(BlockModel model) {
    switch (model) {
    case BlockModel::Stairs:
    case BlockModel::Door:
    case BlockModel::FenceGate:
    case BlockModel::TrapDoor:
    case BlockModel::PressurePlate:
    case BlockModel::Button:
    case BlockModel::Wall:
        return true;
    case BlockModel::Cube:
    case BlockModel::Cross:
    case BlockModel::Crop:
    case BlockModel::Torch:
    case BlockModel::Chest:
    case BlockModel::Slab:
    case BlockModel::DirectionalCube:
    case BlockModel::ElementModel:
    case BlockModel::RedstoneWire:
    case BlockModel::Fire:
        return false;
    }
    return false;
}

// Whether a shaped block's item icon is drawn as a flat item sprite rather than
// a 3D block cube. Vanilla renders a door and trapdoor item as a flat sprite
// (they are thin leaves with no useful 3D inventory silhouette), while a stair,
// wall, fence gate, button and pressure plate item show a 3D block icon. This is
// the HUD "thin leaf -> sprite" special case RN-2 carries; the world mesh always
// draws the real 3D box for all of them.
[[nodiscard]] constexpr bool isThinLeafIconModel(BlockModel model) {
    return model == BlockModel::Door || model == BlockModel::TrapDoor;
}

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
    // AR-CX2: SugarCaneBlock#canSurvive: another sugar cane directly below, or
    // grass/dirt/sand/podzol/coarse_dirt below with a water block orthogonally
    // adjacent to that supporting cell. Its own category because the rule reads
    // the four horizontal neighbours of the block *below*, which none of the
    // other support shapes do.
    SugarCane,
    // AR-CX4-b: FireBlock#canSurvive — fire survives on a sturdy face below it
    // (the ordinary case: fire lit on the top of a solid block) or when at least
    // one of its six neighbours is flammable (fire clinging to a wooden wall).
    // Its own category because it consults both the block below *and* the six
    // neighbours' flammability tag, which no other support shape does.
    Fire,
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
    // ENCH-2. Unlike the three above, this container holds no block entity: its
    // two slots (item + lapis) live on the player's open menu and are handed
    // back when the screen closes, exactly the way EnchantmentMenu owns a
    // `SimpleContainer(2)` and clears it in removed().
    EnchantingTable,
    // ENCH-3: likewise menu-scoped (ItemCombinerMenu owns its inputs).
    Anvil,
};

// The resolved atlas layer for each face of a block, filled by the renderer's
// name-driven atlas build (see textureLayers/setBlockTextureLayers below).
struct BlockTextureLayers final {
    float top = 0.0F;
    float side = 0.0F;
    float bottom = 0.0F;
};

// RN-4a: the resolved atlas layers of a DirectionalCube's six faces, filled by
// the atlas builder from DirectionalTextureNames the same way kBlockTextureLayers
// is filled from BlockTextureNames.
struct DirectionalTextureLayers final {
    float front = 0.0F;
    float frontActive = 0.0F;
    float back = 0.0F;
    float backActive = 0.0F;
    float top = 0.0F;
    float bottom = 0.0F;
    float side = 0.0F;
};

// The block's textures by vanilla file name ("granite", "grass_block_top",
// "dirt"), mirroring how vanilla blocks reference sprites by ResourceLocation.
// The renderer resolves the names into atlas layer indices once at startup and
// writes the per-block layers into kBlockTextureLayers; the mesher and the GUI
// read those precomputed floats, so the hot paths never touch a string.
struct BlockTextureNames final {
    const char* top = nullptr;
    const char* side = nullptr;
    const char* bottom = nullptr;
};

// RN-4a: the six texture faces of a BlockModel::DirectionalCube, by vanilla file
// name. `front` points along FACING, `back` along its opposite (`backActive` is
// the powered variant, e.g. observer_back_on — null falls back to `back`), and
// `top`/`bottom`/`side` fill the other faces (which face is which rotates with
// FACING, resolved by directionalCubeSlot). The renderer resolves the names to
// atlas layers once at startup, exactly like BlockTextureNames.
struct DirectionalTextureNames final {
    const char* front = nullptr;
    // The powered/lit variant of the front face (furnace_front_on). Null means the
    // front never changes (observer, whose active state swaps the back instead).
    const char* frontActive = nullptr;
    const char* back = nullptr;
    const char* backActive = nullptr;
    const char* top = nullptr;
    const char* bottom = nullptr;
    const char* side = nullptr;
};

// RN-4a-2: the most texture slots any ElementModel block references (repeater and
// comparator use four: slab/top/unlit/lit).
inline constexpr std::size_t kMaxModelTextureSlots = 5;

// A block's sound group — 26.1's BlockBehaviour.Properties.sound(SoundType), the
// single identity every break/step/place/hit sound derives from. This is the
// closed set of vanilla SoundType groups the current block roster actually uses;
// it is a baked property, not a runtime-parsed table, so the audio hot path pays
// one enum-indexed array lookup (compileBlockEvents resolves each group's event
// ids once at startup) rather than a per-play string build or hash. `Empty` is a
// block with no sound at all (fluids), matching SoundType.EMPTY. Order is
// arbitrary except that Stone is the default a block falls back to, the same
// default BlockBehaviour.Properties starts from.
enum class SoundType : std::uint8_t {
    Empty,
    Stone,
    Wood,
    Gravel,
    Grass,
    Sand,
    Wool,
    Glass,
    Metal,
    Snow,
    Crop,
    WartBlock,
    NetherBricks,
    NetherOre,
    Netherrack,
    SoulSand,
    SoulSoil,
    Basalt,
    Nylium,
    Count,
};

// Everything the engine knows about one block. Instances are produced by
// BlockProperties (below) and live in the registry table, never built by hand.
struct BlockDefinition final {
    Block block = Block::Air;
    // The registry key, always in this project's namespace.
    Identifier identifier{};
    // The vanilla block this one mirrors, empty for original content. Drives
    // translation keys and vanilla asset lookups; several block states may share
    // one vanilla name, the way the four wall torches do.
    Identifier vanilla{};
    // Fallback English name. Localized text comes from the translation key the
    // identifiers below produce; this is what shows when a key is missing.
    const char* displayName = "";
    BlockTextureNames textures{};
    // RN-4a: the six named faces of a DirectionalCube. Empty for every other
    // model, which reads `textures` instead.
    DirectionalTextureNames directional{};
    // RN-4a-2: an ElementModel block's texture slots by vanilla name, indexed by
    // the transcription's slot constants (e.g. repeater 0=slab 1=top 2=unlit
    // 3=lit). Empty for every other model.
    std::array<const char*, kMaxModelTextureSlots> modelTextures{};
    // RN-4c: the per-model-face base UV rotation (quarter-turns 0..3), indexed by
    // RN-8c: which cube model json this block is drawn from, i.e. what its six
    // faces declare for `uv` and `rotation`. The mesher bakes those, plus the
    // FACING transform, into the block's UVs once at startup.
    CubeUvModel cubeUvModel = CubeUvModel::Default;
    CubeItemModel cubeItemModel = CubeItemModel::BlockModel;
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
    // DirectionalBlock: reads a full six-way FACING chosen at placement from the
    // player's nearest looking direction (observer, piston). Distinct from
    // horizontalFacing, which is the four-way HorizontalDirectionalBlock; a block
    // owns at most one of the two.
    bool directionalFacing = false;
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
    // RN-4a: declare a BlockModel::DirectionalCube and its six texture faces in
    // one call. `front` points along FACING, `back`/`backActive` along its
    // opposite (backActive is the powered sprite), the rest fill top/bottom/side.
    // Sets the model too, so no separate .model() call is needed.
    [[nodiscard]] constexpr BlockProperties directionalCube(
        const char* front, const char* frontActive, const char* back, const char* backActive,
        const char* top, const char* bottom, const char* side) const {
        BlockProperties copy = *this;
        copy.definition_.model = BlockModel::DirectionalCube;
        copy.definition_.directional = {front, frontActive, back, backActive, top, bottom, side};
        return copy;
    }
    // RN-4a-2: declare a BlockModel::ElementModel and its texture slots (by vanilla
    // name), indexed by the mesher's per-block transcription. Sets the model too.
    [[nodiscard]] constexpr BlockProperties elementModel(
        const char* slot0, const char* slot1 = nullptr, const char* slot2 = nullptr,
        const char* slot3 = nullptr, const char* slot4 = nullptr) const {
        BlockProperties copy = *this;
        copy.definition_.model = BlockModel::ElementModel;
        copy.definition_.modelTextures = {slot0, slot1, slot2, slot3, slot4};
        return copy;
    }
    // RN-8c: which cube model json this block is drawn from. Declaring the model
    // rather than six turn counts is what lets a face carry an inverted uv rect
    // as well as a rotation.
    [[nodiscard]] constexpr BlockProperties cubeUvModel(CubeUvModel model) const {
        BlockProperties copy = *this;
        copy.definition_.cubeUvModel = model;
        return copy;
    }
    // RN-8c-D: declare that this block's ITEM is drawn from a plain cube rather
    // than from the block's own model, the way items/piston.json points at
    // block/piston_inventory. The item then shows the block's `.texture()`
    // triple — which is what that inventory model's top/side/bottom are.
    [[nodiscard]] constexpr BlockProperties cubeItemModel(CubeItemModel model) const {
        BlockProperties copy = *this;
        copy.definition_.cubeItemModel = model;
        return copy;
    }
    // RN-6: declare BlockModel::RedstoneWire and its two texture slots — 0 the
    // centre dot, 1 the line arm — resolved to layers the same way ElementModel's
    // slots are.
    [[nodiscard]] constexpr BlockProperties redstoneWireModel(const char* dot,
                                                              const char* line) const {
        BlockProperties copy = *this;
        copy.definition_.model = BlockModel::RedstoneWire;
        copy.definition_.modelTextures = {dot, line, nullptr, nullptr, nullptr};
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
    // DirectionalBlock: a full six-way FACING taken from the placer's nearest
    // looking direction (see BlockPlacement's placementOrientation). Observer and
    // the piston family use this; without it a six-way-FACING block would fall
    // through to defaultOrientation and never rotate with placement.
    [[nodiscard]] constexpr BlockProperties directionalFacing() const {
        BlockProperties copy = *this;
        copy.definition_.directionalFacing = true;
        return copy.state(StateProperty::Facing, 6U);
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
    // Collision is *empty*, matching BasePressurePlateBlock#getCollisionShape
    // (`Shapes.empty()`): a plate never lifts or blocks an entity, so the raised
    // Column shape is an outline/visual/pick shape only (blockShape), and
    // collisionShape filters it out via hasCollision — the same visual-vs-
    // collision split an open fence gate already uses. Keeping collision on made
    // the raised(1/16)->pressed(0.5/16) height toggle oscillate against the
    // feet-cell probe and bounced the player. The plate needs no wall/ground
    // flag here since canBlockSurvive routes a plain BlockSupport::Ground.
    [[nodiscard]] constexpr BlockProperties pressurePlate() const {
        BlockProperties copy = *this;
        return copy.model(BlockModel::PressurePlate)
            .renderLayer(BlockRenderLayer::Cutout)
            .noCollision()
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
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Dirt, "dirt", "Dirt")
        .texture("dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Stone, "stone", "Stone")
        .texture("stone")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Cobblestone, "cobblestone", "Cobblestone")
        .texture("cobblestone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::OakPlanks, "oak_planks", "Oak Planks")
        .texture("oak_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::OakLog, "oak_log", "Oak Log")
        .texture("oak_log_top", "oak_log", "oak_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Bricks, "bricks", "Bricks")
        .texture("bricks")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::Bedrock, "bedrock", "Bedrock")
        .texture("bedrock")
        .unbreakable(3'600'000.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Sand, "sand", "Sand")
        .texture("sand")
        .strength(0.5F)
        .gravity()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Glass, "glass", "Glass")
        .texture("glass")
        .strength(0.3F)
        .renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::CoalOre, "coal_ore", "Coal Ore")
        .texture("coal_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::IronOre, "iron_ore", "Iron Ore")
        .texture("iron_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::GoldOre, "gold_ore", "Gold Ore")
        .texture("gold_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DiamondOre, "diamond_ore", "Diamond Ore")
        .texture("diamond_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    // Material.REPLACEABLE_PLANT: placing a block inside tall grass replaces it.
    BlockProperties::of(Block::GrassPlant, "short_grass", "Short Grass")
        .texture("short_grass")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Dandelion, "dandelion", "Dandelion")
        .texture("dandelion")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::OakSapling, "oak_sapling", "Oak Sapling")
        .texture("oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::OakLeaves, "oak_leaves", "Oak Leaves")
        .texture("oak_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
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
        .creative(CreativeCategory::NaturalBlocks),
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
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::BirchLog, "birch_log", "Birch Log")
        .texture("birch_log_top", "birch_log", "birch_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Bookshelf, "bookshelf", "Bookshelf")
        .texture("oak_planks", "bookshelf", "oak_planks")
        .strength(1.5F)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::CraftingTable, "crafting_table", "Crafting Table")
        .texture("crafting_table_top", "crafting_table_side", "oak_planks")
        .strength(2.5F)
        .container(ContainerType::CraftingTable)
        .creative(CreativeCategory::Functional),
    // One furnace, lit or not: AbstractFurnaceBlock's LIT is a state, so a
    // burning furnace is the same block and keeps its block entity (and its
    // smelt) across the swap. The lit front (furnace_front_on) is the
    // DirectionalCube's frontActive slot; light 13 is what the burning state emits.
    BlockProperties::of(Block::Furnace, "furnace", "Furnace")
        // RN-4a follow-up: a horizontal DirectionalCube — front faces FACING and
        // LIT swaps it to furnace_front_on; the other five faces are furnace_side
        // (back too) and furnace_top (top/bottom). This retires the furnace-front
        // hardcode (the fixed atlas layers 167/168 and the textureLayer special
        // case) so a horizontally-oriented cube is now general, not a furnace-only
        // branch. Keeps .texture() for the flat item sprite / dropped item.
        .texture("furnace_top", "furnace_side", "furnace_top")
        .directionalCube("furnace_front", "furnace_front_on", "furnace_side", nullptr,
                         "furnace_top", "furnace_top", "furnace_side")
        .strength(3.5F)
        .horizontalFacing()
        .lit(13U)
        .container(ContainerType::Furnace)
        .blockEntity(BlockEntityKind::Furnace)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::Obsidian, "obsidian", "Obsidian")
        .texture("obsidian")
        .strength(50.0F, 1'200.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Clay, "clay", "Clay")
        .texture("clay")
        .strength(0.6F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::SnowBlock, "snow_block", "Snow Block")
        .texture("snow")
        .strength(0.2F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Netherrack, "netherrack", "Netherrack")
        .texture("netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Glowstone, "glowstone", "Glowstone")
        .texture("glowstone")
        .strength(0.3F)
        .light(15U)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::WhiteWool, "white_wool", "White Wool")
        .texture("white_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::RedWool, "red_wool", "Red Wool")
        .texture("red_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlackWool, "black_wool", "Black Wool")
        .texture("black_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    // DYE-2: the remaining 13 dyed wools. Same strength/category as the three
    // above; each carries its stable "<colour>_wool" name (the save-palette /
    // JC anchor) and its own texture. Registered in the same DyeColor palette
    // order used by kDyeColors so woolBlockFor's table stays a straight zip.
    BlockProperties::of(Block::OrangeWool, "orange_wool", "Orange Wool")
        .texture("orange_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::MagentaWool, "magenta_wool", "Magenta Wool")
        .texture("magenta_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightBlueWool, "light_blue_wool", "Light Blue Wool")
        .texture("light_blue_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::YellowWool, "yellow_wool", "Yellow Wool")
        .texture("yellow_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LimeWool, "lime_wool", "Lime Wool")
        .texture("lime_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PinkWool, "pink_wool", "Pink Wool")
        .texture("pink_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GrayWool, "gray_wool", "Gray Wool")
        .texture("gray_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightGrayWool, "light_gray_wool", "Light Gray Wool")
        .texture("light_gray_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::CyanWool, "cyan_wool", "Cyan Wool")
        .texture("cyan_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PurpleWool, "purple_wool", "Purple Wool")
        .texture("purple_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlueWool, "blue_wool", "Blue Wool")
        .texture("blue_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BrownWool, "brown_wool", "Brown Wool")
        .texture("brown_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GreenWool, "green_wool", "Green Wool")
        .texture("green_wool")
        .strength(0.8F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::StoneBricks, "stone_bricks", "Stone Bricks")
        .texture("stone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MossyCobblestone, "mossy_cobblestone", "Mossy Cobblestone")
        .texture("mossy_cobblestone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Sandstone, "sandstone", "Sandstone")
        .texture("sandstone_top", "sandstone", "sandstone_bottom")
        .strength(0.8F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Pumpkin, "pumpkin", "Pumpkin")
        .texture("pumpkin_top", "pumpkin_side", "pumpkin_top")
        .strength(1.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Melon, "melon", "Melon")
        .texture("melon_top", "melon_side", "melon_top")
        .strength(1.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Tnt, "tnt", "TNT")
        .texture("tnt_top", "tnt_side", "tnt_bottom")
        .instantBreak()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::Granite, "granite", "Granite")
        .texture("granite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Diorite, "diorite", "Diorite")
        .texture("diorite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Andesite, "andesite", "Andesite")
        .texture("andesite")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::CoarseDirt, "coarse_dirt", "Coarse Dirt")
        .texture("coarse_dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Podzol, "podzol", "Podzol")
        .texture("podzol_top", "podzol_side", "dirt")
        .strength(0.5F)
        .soil()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::RedSand, "red_sand", "Red Sand")
        .texture("red_sand")
        .strength(0.5F)
        .gravity()
        .creative(CreativeCategory::NaturalBlocks),
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
        .creative(CreativeCategory::NaturalBlocks),
    // AR-CI: kept in Functional (its pre-existing tab from the old hand-array)
    // per the no-regression rule, even though vanilla files redstone_ore's item
    // under naturalBlocks — moving it to the new Redstone tab is a judgment
    // call this pass declines to make unasked.
    BlockProperties::of(Block::RedstoneOre, "redstone_ore", "Redstone Ore")
        .texture("redstone_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::EmeraldOre, "emerald_ore", "Emerald Ore")
        .texture("emerald_ore")
        .strength(3.0F)
        .creative(CreativeCategory::NaturalBlocks),
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
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::JunglePlanks, "jungle_planks", "Jungle Planks")
        .texture("jungle_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::AcaciaLog, "acacia_log", "Acacia Log")
        .texture("acacia_log_top", "acacia_log", "acacia_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::AcaciaPlanks, "acacia_planks", "Acacia Planks")
        .texture("acacia_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DarkOakLog, "dark_oak_log", "Dark Oak Log")
        .texture("dark_oak_log_top", "dark_oak_log", "dark_oak_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DarkOakPlanks, "dark_oak_planks", "Dark Oak Planks")
        .texture("dark_oak_planks")
        .strength(2.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SpruceLeaves, "spruce_leaves", "Spruce Leaves")
        .texture("spruce_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::BirchLeaves, "birch_leaves", "Birch Leaves")
        .texture("birch_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::JungleLeaves, "jungle_leaves", "Jungle Leaves")
        .texture("jungle_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::AcaciaLeaves, "acacia_leaves", "Acacia Leaves")
        .texture("acacia_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DarkOakLeaves, "dark_oak_leaves", "Dark Oak Leaves")
        .texture("dark_oak_leaves")
        .leaves()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::SpruceSapling, "spruce_sapling", "Spruce Sapling")
        .texture("spruce_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::BirchSapling, "birch_sapling", "Birch Sapling")
        .texture("birch_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::JungleSapling, "jungle_sapling", "Jungle Sapling")
        .texture("jungle_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::AcaciaSapling, "acacia_sapling", "Acacia Sapling")
        .texture("acacia_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DarkOakSapling, "dark_oak_sapling", "Dark Oak Sapling")
        .texture("dark_oak_sapling")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
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
        // RN-4a-2: cobblestone base + a 45°-tilted handle, transcribed from vanilla
        // models/block/lever(_on).json. Replaces the default Cube model, which had
        // wrongly made the lever a full-cube occluder (isFullCube is now false).
        .elementModel("cobblestone", "lever")
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
        // RN-4a-2: real diode geometry (smooth-stone slab base + redstone-torch
        // nubs), transcribed from vanilla models/block/repeater_*tick*.json.
        .elementModel("smooth_stone", "repeater", "redstone_torch_off", "redstone_torch")
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
        // RN-4a-2: slab base + three redstone-torch nubs, transcribed from vanilla
        // models/block/comparator*.json.
        .elementModel("smooth_stone", "comparator", "redstone_torch_off", "redstone_torch")
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
        // RN-6: item icon is the flat redstone item sprite; the world model is a
        // power-tinted flat wire (dot + line arms), transcribed from vanilla's
        // redstone_dust_* models. The old `.texture("redstone_dust_line")` named a
        // file that does not exist (the real sprites are redstone_dust_line0/dot).
        .texture("redstone_dust_dot")
        .redstoneWireModel("redstone_dust_dot", "redstone_dust_line0")
        .instantBreak()
        .renderLayer(BlockRenderLayer::Cutout)
        .noCollision()
        .support(BlockSupport::Ground)
        .state(StateProperty::AnalogSignal, 16U)
        .creative(CreativeCategory::Redstone),
    // Observer: FACING is the six-way watched direction, POWERED the pulse. RN-4a:
    // a real six-face DirectionalCube (front faces FACING, back = observer_back /
    // _back_on by POWERED, top/bottom share observer_top, sides observer_side),
    // transcribed from vanilla models/block/observer.json. As a full cube it now
    // occludes and is face-sturdy — the Torch placeholder used to leak light.
    BlockProperties::of(Block::Observer, "observer", "Observer")
        // RN-8c-D: the flat triple every block declares — break particles read its
        // side, and until now the observer had none, so the atlas baker had to
        // invent one out of the directional faces.
        .texture("observer_top", "observer_side", "observer_top")
        // front never changes; POWERED swaps the back to observer_back_on.
        .directionalCube("observer_front", nullptr, "observer_back", "observer_back_on",
                         "observer_top", "observer_top", "observer_side")
        // RN-8c: observer.json declares its UP face with an inverted rect
        // ("uv": [0,16,16,0]) — the registered "observer top uv-rect flipped"
        // defect, which is a property of the model, not of the renderer.
        .cubeUvModel(CubeUvModel::Observer)
        .strength(3.0F)
        .directionalFacing()
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
    // RN-4a follow-up: a six-way DirectionalCube (like the observer). FACING's
    // face is the piston platform (piston_top); the opposite face is piston_bottom
    // and the other four are piston_side, transcribed from vanilla template_piston.
    // This retires the plain-Cube-of-piston_side that showed the same side texture
    // on every face (no platform, no orientation) in both the world and the icon.
    // The flat .texture() stays the piston_side item/dropped fallback.
    BlockProperties::of(Block::Piston, "piston", "Piston")
        // RN-8c-D: items/piston.json points at block/piston_inventory, a plain
        // cube_bottom_top whose top is piston_top — a piston ITEM shows its
        // platform upward, not on a side. This triple IS that inventory model.
        .texture("piston_top", "piston_side", "piston_bottom")
        .cubeItemModel(CubeItemModel::PlainCube)
        .directionalCube("piston_top", nullptr, "piston_bottom", nullptr, "piston_side",
                         "piston_side", "piston_side")
        // RN-8c: template_piston.json's own per-face rotations (down 180, west
        // 270, east 90) wrap piston_side's frame around the platform.
        .cubeUvModel(CubeUvModel::PistonTemplate)
        .strength(1.5F)
        .directionalFacing()
        .state(StateProperty::Powered, 2U)
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::StickyPiston, "sticky_piston", "Sticky Piston")
        // RN-8c-D: block/sticky_piston_inventory, the same plain cube_bottom_top.
        .texture("piston_top_sticky", "piston_side", "piston_bottom")
        .cubeItemModel(CubeItemModel::PlainCube)
        .directionalCube("piston_top_sticky", nullptr, "piston_bottom", nullptr, "piston_side",
                         "piston_side", "piston_side")
        // RN-8c: the same template_piston.json face rotations as the plain piston.
        .cubeUvModel(CubeUvModel::PistonTemplate)
        .strength(1.5F)
        .directionalFacing()
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
    // 26.1's Blocks.java; textures name the vanilla sprites so the
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
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::SoulSoil, "soul_soil", "Soul Soil")
        .texture("soul_soil")
        .strength(0.5F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::NetherQuartzOre, "nether_quartz_ore", "Nether Quartz Ore")
        .texture("nether_quartz_ore")
        .strength(3.0F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    // MagmaBlock: a full solid cube that glows at light level 3 (vanilla
    // lightLevel(3)). The damage-on-standing behaviour is AR-B, not WG-0.
    BlockProperties::of(Block::MagmaBlock, "magma_block", "Magma Block")
        .texture("magma")
        .strength(0.5F)
        .light(3U)
        .creative(CreativeCategory::NaturalBlocks),
    // Basalt: a RotatedPillarBlock in vanilla (top/side end grain), so it takes
    // the axis of the face it is placed against, like a log.
    BlockProperties::of(Block::Basalt, "basalt", "Basalt")
        .texture("basalt_top", "basalt_side", "basalt_top")
        .strength(1.25F, 4.2F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Blackstone, "blackstone", "Blackstone")
        .texture("blackstone_top", "blackstone", "blackstone_top")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::NetherBricks, "nether_bricks", "Nether Bricks")
        .texture("nether_bricks")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::NetherWartBlock, "nether_wart_block", "Nether Wart Block")
        .texture("nether_wart_block")
        .strength(1.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::CrimsonNylium, "crimson_nylium", "Crimson Nylium")
        .texture("crimson_nylium", "crimson_nylium_side", "netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::WarpedNylium, "warped_nylium", "Warped Nylium")
        .texture("warped_nylium", "warped_nylium_side", "netherrack")
        .strength(0.4F)
        .creative(CreativeCategory::NaturalBlocks),
    // WG-0 end base blocks. WG-3 places these; playable content stays AR-B.
    BlockProperties::of(Block::EndStone, "end_stone", "End Stone")
        .texture("end_stone")
        .strength(3.0F, 9.0F)
        .creative(CreativeCategory::NaturalBlocks),
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
        .creative(CreativeCategory::Redstone),
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
        .creative(CreativeCategory::Redstone),
    // AR-B3: TrapDoorBlock `implements SimpleWaterloggedBlock` in vanilla
    // (TrapDoorBlock.java:43) — unlike the door, a trapdoor *does* submerge.
    // Left un-opted for this pass (matching AR-B2's own stated boundary: F2
    // extension is a separate, deliberate pass per block family) — recorded
    // here as the same "interface ready, not yet wired" note AR-B2 left doors.
    BlockProperties::of(Block::OakTrapdoor, "oak_trapdoor", "Oak Trapdoor")
        .texture("oak_trapdoor", "oak_trapdoor", "oak_trapdoor")
        .strength(3.0F)
        .trapdoor()
        .creative(CreativeCategory::Redstone),
    // BasePressurePlateBlock: not a SimpleWaterloggedBlock in vanilla either
    // (PressurePlateBlock.java has no WATERLOGGED property) — no .submerges().
    BlockProperties::of(Block::StonePressurePlate, "stone_pressure_plate", "Stone Pressure Plate")
        .texture("stone")
        .instantBreak()
        .pressurePlate()
        .creative(CreativeCategory::Redstone),
    // WallBlock: the first cobblestone-family wall, proving the four-side
    // connection mechanism the way OakStairs proved the join-shape mechanism.
    BlockProperties::of(Block::CobblestoneWall, "cobblestone_wall", "Cobblestone Wall")
        .texture("cobblestone")
        .strength(2.0F, 6.0F)
        .wall()
        .creative(CreativeCategory::BuildingBlocks),
    // AR-CX2: SugarCaneBlock. A cross-model plant (no collision, cutout, instant
    // break), dropping itself as its block item (dropsItem stays true), placed
    // by its own SugarCane support rule. AGE 0-15 (SugarCaneBlock.AGE) paces the
    // vertical random-tick growth in WorldSimulation. Filed under Decoration so
    // it is reachable in creative like the other plants.
    BlockProperties::of(Block::SugarCane, "sugar_cane", "Sugar Cane")
        .texture("sugar_cane")
        .instantBreak()
        .cross()
        .support(BlockSupport::SugarCane)
        .state(StateProperty::Age, 16U)
        .creative(CreativeCategory::NaturalBlocks),
    // AR-CX4-b: FireBlock. A non-solid, instantly-broken, light-15 block drawn
    // with the cross-plant path (name-driven atlas: block/fire.png, a
    // missing-texture placeholder until the asset lands — mac visual deferred).
    // No collision, no drops (dropsItem false — you never pick fire up), placed
    // by flint_and_steel. AGE 0-15 (FireBlock.AGE) paces its random-tick burn.
    // Hidden from creative: fire is a technical block, obtained only by igniting
    // a surface, exactly like vanilla lists no fire item in the creative tabs.
    BlockProperties::of(Block::Fire, "fire", "Fire")
        // RN-7: the real animated fire sprite is fire_0 (a 32-frame strip baked as
        // a run by RN-4b), never "fire" (no such file — the old name loaded the
        // magenta placeholder). BlockModel::Fire meshes the billowing planes.
        .texture("fire_0")
        .instantBreak()
        .model(BlockModel::Fire)
        .renderLayer(BlockRenderLayer::Cutout)
        .noCollision()
        .noDrops()
        .light(15U)
        .support(BlockSupport::Fire)
        .state(StateProperty::Age, 16U),
    // RN-4b content: prismarine — a full cube whose "prismarine" texture is a
    // 4-frame animated strip (baked as a run by RN-4b, cycled by the terrain
    // shader). Vanilla strength 1.5/6.0, pickaxe block.
    BlockProperties::of(Block::Prismarine, "prismarine", "Prismarine")
        .texture("prismarine")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // RN-4b content: sea lantern — a full-bright cube whose "sea_lantern" texture
    // is a 5-frame animated strip. Emits light 15.
    BlockProperties::of(Block::SeaLantern, "sea_lantern", "Sea Lantern")
        .texture("sea_lantern")
        .strength(0.3F)
        .light(15U)
        .creative(CreativeCategory::NaturalBlocks),
    // STRUCT content: ice — a translucent full cube, the way glass renders, so an
    // igloo's ice floor resolves instead of leaving a hole. Vanilla strength 0.5.
    BlockProperties::of(Block::Ice, "ice", "Ice")
        .texture("ice")
        .strength(0.5F)
        .renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::NaturalBlocks),
    // --- STRUCT AR-B batch 1: plain cubes + pillars (see enum comment) ---------
    // Stone-brick variants. Infested blocks reuse the host brick's sprite.
    BlockProperties::of(Block::CrackedStoneBricks, "cracked_stone_bricks", "Cracked Stone Bricks")
        .texture("cracked_stone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::InfestedStoneBricks, "infested_stone_bricks", "Infested Stone Bricks")
        .texture("stone_bricks")
        .strength(0.0F, 0.75F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::InfestedMossyStoneBricks, "infested_mossy_stone_bricks",
                        "Infested Mossy Stone Bricks")
        .texture("mossy_stone_bricks")
        .strength(0.0F, 0.75F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::InfestedChiseledStoneBricks, "infested_chiseled_stone_bricks",
                        "Infested Chiseled Stone Bricks")
        .texture("chiseled_stone_bricks")
        .strength(0.0F, 0.75F)
        .creative(CreativeCategory::NaturalBlocks),
    // Blackstone family.
    BlockProperties::of(Block::PolishedBlackstoneBricks, "polished_blackstone_bricks",
                        "Polished Blackstone Bricks")
        .texture("polished_blackstone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CrackedPolishedBlackstoneBricks, "cracked_polished_blackstone_bricks",
                        "Cracked Polished Blackstone Bricks")
        .texture("cracked_polished_blackstone_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedBlackstone, "polished_blackstone", "Polished Blackstone")
        .texture("polished_blackstone")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledPolishedBlackstone, "chiseled_polished_blackstone",
                        "Chiseled Polished Blackstone")
        .texture("chiseled_polished_blackstone")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::GildedBlackstone, "gilded_blackstone", "Gilded Blackstone")
        .texture("gilded_blackstone")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    // Tuff family.
    BlockProperties::of(Block::Tuff, "tuff", "Tuff")
        .texture("tuff")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::PolishedTuff, "polished_tuff", "Polished Tuff")
        .texture("polished_tuff")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::TuffBricks, "tuff_bricks", "Tuff Bricks")
        .texture("tuff_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledTuff, "chiseled_tuff", "Chiseled Tuff")
        .texture("chiseled_tuff")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledTuffBricks, "chiseled_tuff_bricks", "Chiseled Tuff Bricks")
        .texture("chiseled_tuff_bricks")
        .strength(1.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Deepslate family. Deepslate is a pillar (axis); the rest are plain cubes.
    BlockProperties::of(Block::Deepslate, "deepslate", "Deepslate")
        .texture("deepslate_top", "deepslate", "deepslate_top")
        .strength(3.0F, 6.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::CobbledDeepslate, "cobbled_deepslate", "Cobbled Deepslate")
        .texture("cobbled_deepslate")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::PolishedDeepslate, "polished_deepslate", "Polished Deepslate")
        .texture("polished_deepslate")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateBricks, "deepslate_bricks", "Deepslate Bricks")
        .texture("deepslate_bricks")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CrackedDeepslateBricks, "cracked_deepslate_bricks",
                        "Cracked Deepslate Bricks")
        .texture("cracked_deepslate_bricks")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateTiles, "deepslate_tiles", "Deepslate Tiles")
        .texture("deepslate_tiles")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CrackedDeepslateTiles, "cracked_deepslate_tiles",
                        "Cracked Deepslate Tiles")
        .texture("cracked_deepslate_tiles")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledDeepslate, "chiseled_deepslate", "Chiseled Deepslate")
        .texture("chiseled_deepslate")
        .strength(3.5F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Sandstone variants — smooth/cut/chiseled reuse sandstone_top for the faces
    // vanilla's models do.
    BlockProperties::of(Block::SmoothSandstone, "smooth_sandstone", "Smooth Sandstone")
        .texture("sandstone_top")
        .strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CutSandstone, "cut_sandstone", "Cut Sandstone")
        .texture("sandstone_top", "cut_sandstone", "sandstone_top")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ChiseledSandstone, "chiseled_sandstone", "Chiseled Sandstone")
        .texture("sandstone_top", "chiseled_sandstone", "sandstone_top")
        .strength(0.8F)
        .creative(CreativeCategory::BuildingBlocks),
    // Mud family.
    BlockProperties::of(Block::Mud, "mud", "Mud")
        .texture("mud")
        .strength(0.5F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::PackedMud, "packed_mud", "Packed Mud")
        .texture("packed_mud")
        .strength(1.0F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MudBricks, "mud_bricks", "Mud Bricks")
        .texture("mud_bricks")
        .strength(1.5F, 3.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Waxed copper full cubes.
    BlockProperties::of(Block::WaxedCopperBlock, "waxed_copper_block", "Waxed Block of Copper")
        .texture("copper_block")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedExposedCopper, "waxed_exposed_copper", "Waxed Exposed Copper")
        .texture("exposed_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedWeatheredCopper, "waxed_weathered_copper",
                        "Waxed Weathered Copper")
        .texture("weathered_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedOxidizedCopper, "waxed_oxidized_copper", "Waxed Oxidized Copper")
        .texture("oxidized_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedCutCopper, "waxed_cut_copper", "Waxed Cut Copper")
        .texture("cut_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedExposedCutCopper, "waxed_exposed_cut_copper",
                        "Waxed Exposed Cut Copper")
        .texture("exposed_cut_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedWeatheredCutCopper, "waxed_weathered_cut_copper",
                        "Waxed Weathered Cut Copper")
        .texture("weathered_cut_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedOxidizedCutCopper, "waxed_oxidized_cut_copper",
                        "Waxed Oxidized Cut Copper")
        .texture("oxidized_cut_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedChiseledCopper, "waxed_chiseled_copper", "Waxed Chiseled Copper")
        .texture("chiseled_copper")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Metal.
    BlockProperties::of(Block::GoldBlock, "gold_block", "Block of Gold")
        .texture("gold_block")
        .strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    // Terracotta — plain plus 16 colours.
    BlockProperties::of(Block::Terracotta, "terracotta", "Terracotta")
        .texture("terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WhiteTerracotta, "white_terracotta", "White Terracotta")
        .texture("white_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::OrangeTerracotta, "orange_terracotta", "Orange Terracotta")
        .texture("orange_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::MagentaTerracotta, "magenta_terracotta", "Magenta Terracotta")
        .texture("magenta_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightBlueTerracotta, "light_blue_terracotta", "Light Blue Terracotta")
        .texture("light_blue_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::YellowTerracotta, "yellow_terracotta", "Yellow Terracotta")
        .texture("yellow_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LimeTerracotta, "lime_terracotta", "Lime Terracotta")
        .texture("lime_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PinkTerracotta, "pink_terracotta", "Pink Terracotta")
        .texture("pink_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GrayTerracotta, "gray_terracotta", "Gray Terracotta")
        .texture("gray_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightGrayTerracotta, "light_gray_terracotta", "Light Gray Terracotta")
        .texture("light_gray_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::CyanTerracotta, "cyan_terracotta", "Cyan Terracotta")
        .texture("cyan_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PurpleTerracotta, "purple_terracotta", "Purple Terracotta")
        .texture("purple_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlueTerracotta, "blue_terracotta", "Blue Terracotta")
        .texture("blue_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BrownTerracotta, "brown_terracotta", "Brown Terracotta")
        .texture("brown_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GreenTerracotta, "green_terracotta", "Green Terracotta")
        .texture("green_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::RedTerracotta, "red_terracotta", "Red Terracotta")
        .texture("red_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlackTerracotta, "black_terracotta", "Black Terracotta")
        .texture("black_terracotta")
        .strength(1.25F, 4.2F)
        .creative(CreativeCategory::ColoredBlocks),
    // Pillar blocks (RotatedPillarBlock: six-way FACING axis).
    BlockProperties::of(Block::BoneBlock, "bone_block", "Bone Block")
        .texture("bone_block_top", "bone_block_side", "bone_block_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::PolishedBasalt, "polished_basalt", "Polished Basalt")
        .texture("polished_basalt_top", "polished_basalt_side", "polished_basalt_top")
        .strength(1.25F, 4.2F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PurpurPillar, "purpur_pillar", "Purpur Pillar")
        .texture("purpur_pillar_top", "purpur_pillar", "purpur_pillar_top")
        .strength(1.5F, 6.0F)
        .pillar()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::AcaciaWood, "acacia_wood", "Acacia Wood")
        .texture("acacia_log")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::StrippedSpruceWood, "stripped_spruce_wood", "Stripped Spruce Wood")
        .texture("stripped_spruce_log")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::StrippedSpruceLog, "stripped_spruce_log", "Stripped Spruce Log")
        .texture("stripped_spruce_log_top", "stripped_spruce_log", "stripped_spruce_log_top")
        .strength(2.0F)
        .pillar()
        .creative(CreativeCategory::NaturalBlocks),
    // Dirt path — a 15/16-high cube with its own top/side sprites (like farmland).
    BlockProperties::of(Block::DirtPath, "dirt_path", "Dirt Path")
        .texture("dirt_path_top", "dirt_path_side", "dirt")
        .strength(0.65F)
        .height(0.9375F)
        .creative(CreativeCategory::NaturalBlocks),
    // --- STRUCT AR-B batch 2: stairs / slabs / walls / doors / trapdoors --------
    // Stairs (texture = parent block's sprites, .stairs() supplies the model+axes).
    BlockProperties::of(Block::AcaciaStairs, "acacia_stairs", "Acacia Stairs")
        .texture("acacia_planks").strength(2.0F, 3.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BirchStairs, "birch_stairs", "Birch Stairs")
        .texture("birch_planks").strength(2.0F, 3.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DarkOakStairs, "dark_oak_stairs", "Dark Oak Stairs")
        .texture("dark_oak_planks").strength(2.0F, 3.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SpruceStairs, "spruce_stairs", "Spruce Stairs")
        .texture("spruce_planks").strength(2.0F, 3.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CobblestoneStairs, "cobblestone_stairs", "Cobblestone Stairs")
        .texture("cobblestone").strength(2.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MossyCobblestoneStairs, "mossy_cobblestone_stairs",
                        "Mossy Cobblestone Stairs")
        .texture("mossy_cobblestone").strength(2.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::StoneBrickStairs, "stone_brick_stairs", "Stone Brick Stairs")
        .texture("stone_bricks").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BrickStairs, "brick_stairs", "Brick Stairs")
        .texture("bricks").strength(2.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SandstoneStairs, "sandstone_stairs", "Sandstone Stairs")
        .texture("sandstone_top", "sandstone", "sandstone_bottom").strength(0.8F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SmoothSandstoneStairs, "smooth_sandstone_stairs",
                        "Smooth Sandstone Stairs")
        .texture("sandstone_top").strength(2.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::GraniteStairs, "granite_stairs", "Granite Stairs")
        .texture("granite").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DioriteStairs, "diorite_stairs", "Diorite Stairs")
        .texture("diorite").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PurpurStairs, "purpur_stairs", "Purpur Stairs")
        .texture("purpur_block").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BlackstoneStairs, "blackstone_stairs", "Blackstone Stairs")
        .texture("blackstone_top", "blackstone", "blackstone_top").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedBlackstoneBrickStairs, "polished_blackstone_brick_stairs",
                        "Polished Blackstone Brick Stairs")
        .texture("polished_blackstone_bricks").strength(1.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MudBrickStairs, "mud_brick_stairs", "Mud Brick Stairs")
        .texture("mud_bricks").strength(1.5F, 3.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CobbledDeepslateStairs, "cobbled_deepslate_stairs",
                        "Cobbled Deepslate Stairs")
        .texture("cobbled_deepslate").strength(3.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedDeepslateStairs, "polished_deepslate_stairs",
                        "Polished Deepslate Stairs")
        .texture("polished_deepslate").strength(3.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateBrickStairs, "deepslate_brick_stairs",
                        "Deepslate Brick Stairs")
        .texture("deepslate_bricks").strength(3.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateTileStairs, "deepslate_tile_stairs", "Deepslate Tile Stairs")
        .texture("deepslate_tiles").strength(3.5F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedCutCopperStairs, "waxed_cut_copper_stairs",
                        "Waxed Cut Copper Stairs")
        .texture("cut_copper").strength(3.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedOxidizedCutCopperStairs, "waxed_oxidized_cut_copper_stairs",
                        "Waxed Oxidized Cut Copper Stairs")
        .texture("oxidized_cut_copper").strength(3.0F, 6.0F).stairs()
        .creative(CreativeCategory::BuildingBlocks),
    // Slabs (texture = parent block, .slab() supplies model + SlabType axis).
    BlockProperties::of(Block::SandstoneSlab, "sandstone_slab", "Sandstone Slab")
        .texture("sandstone_top", "sandstone", "sandstone_bottom").strength(0.8F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SmoothSandstoneSlab, "smooth_sandstone_slab", "Smooth Sandstone Slab")
        .texture("sandstone_top").strength(2.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BrickSlab, "brick_slab", "Brick Slab")
        .texture("bricks").strength(2.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MossyCobblestoneSlab, "mossy_cobblestone_slab",
                        "Mossy Cobblestone Slab")
        .texture("mossy_cobblestone").strength(2.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DioriteSlab, "diorite_slab", "Diorite Slab")
        .texture("diorite").strength(1.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PurpurSlab, "purpur_slab", "Purpur Slab")
        .texture("purpur_block").strength(1.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SmoothQuartzSlab, "smooth_quartz_slab", "Smooth Quartz Slab")
        .texture("quartz_block_bottom").strength(2.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BlackstoneSlab, "blackstone_slab", "Blackstone Slab")
        .texture("blackstone_top", "blackstone", "blackstone_top").strength(1.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MudBrickSlab, "mud_brick_slab", "Mud Brick Slab")
        .texture("mud_bricks").strength(1.5F, 3.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CobbledDeepslateSlab, "cobbled_deepslate_slab",
                        "Cobbled Deepslate Slab")
        .texture("cobbled_deepslate").strength(3.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedDeepslateSlab, "polished_deepslate_slab",
                        "Polished Deepslate Slab")
        .texture("polished_deepslate").strength(3.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateBrickSlab, "deepslate_brick_slab", "Deepslate Brick Slab")
        .texture("deepslate_bricks").strength(3.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateTileSlab, "deepslate_tile_slab", "Deepslate Tile Slab")
        .texture("deepslate_tiles").strength(3.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedTuffSlab, "polished_tuff_slab", "Polished Tuff Slab")
        .texture("polished_tuff").strength(1.5F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedCutCopperSlab, "waxed_cut_copper_slab", "Waxed Cut Copper Slab")
        .texture("cut_copper").strength(3.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedOxidizedCutCopperSlab, "waxed_oxidized_cut_copper_slab",
                        "Waxed Oxidized Cut Copper Slab")
        .texture("oxidized_cut_copper").strength(3.0F, 6.0F).slab()
        .creative(CreativeCategory::BuildingBlocks),
    // Walls (texture = parent block, .wall() supplies model + connection axes).
    BlockProperties::of(Block::MossyCobblestoneWall, "mossy_cobblestone_wall",
                        "Mossy Cobblestone Wall")
        .texture("mossy_cobblestone").strength(2.0F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::StoneBrickWall, "stone_brick_wall", "Stone Brick Wall")
        .texture("stone_bricks").strength(1.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BrickWall, "brick_wall", "Brick Wall")
        .texture("bricks").strength(2.0F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::SandstoneWall, "sandstone_wall", "Sandstone Wall")
        .texture("sandstone_top", "sandstone", "sandstone_bottom").strength(0.8F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::GraniteWall, "granite_wall", "Granite Wall")
        .texture("granite").strength(1.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DioriteWall, "diorite_wall", "Diorite Wall")
        .texture("diorite").strength(1.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BlackstoneWall, "blackstone_wall", "Blackstone Wall")
        .texture("blackstone_top", "blackstone", "blackstone_top").strength(1.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MudBrickWall, "mud_brick_wall", "Mud Brick Wall")
        .texture("mud_bricks").strength(1.5F, 3.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CobbledDeepslateWall, "cobbled_deepslate_wall",
                        "Cobbled Deepslate Wall")
        .texture("cobbled_deepslate").strength(3.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::PolishedDeepslateWall, "polished_deepslate_wall",
                        "Polished Deepslate Wall")
        .texture("polished_deepslate").strength(3.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateBrickWall, "deepslate_brick_wall", "Deepslate Brick Wall")
        .texture("deepslate_bricks").strength(3.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::DeepslateTileWall, "deepslate_tile_wall", "Deepslate Tile Wall")
        .texture("deepslate_tiles").strength(3.5F, 6.0F).wall()
        .creative(CreativeCategory::BuildingBlocks),
    // Doors (.door() supplies model + Facing/Half/Open/Hinge; two-cell {top,bottom}
    // sprites, as OakDoor uses).
    BlockProperties::of(Block::SpruceDoor, "spruce_door", "Spruce Door")
        .texture("spruce_door_top", "spruce_door_bottom", "spruce_door_bottom").strength(3.0F).door()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::JungleDoor, "jungle_door", "Jungle Door")
        .texture("jungle_door_top", "jungle_door_bottom", "jungle_door_bottom").strength(3.0F).door()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::AcaciaDoor, "acacia_door", "Acacia Door")
        .texture("acacia_door_top", "acacia_door_bottom", "acacia_door_bottom").strength(3.0F).door()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::DarkOakDoor, "dark_oak_door", "Dark Oak Door")
        .texture("dark_oak_door_top", "dark_oak_door_bottom", "dark_oak_door_bottom").strength(3.0F)
        .door().creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::IronDoor, "iron_door", "Iron Door")
        .texture("iron_door_top", "iron_door_bottom", "iron_door_bottom").strength(5.0F).door()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::WaxedCopperDoor, "waxed_copper_door", "Waxed Copper Door")
        .texture("copper_door_top", "copper_door_bottom", "copper_door_bottom").strength(3.0F).door()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::WaxedOxidizedCopperDoor, "waxed_oxidized_copper_door",
                        "Waxed Oxidized Copper Door")
        .texture("oxidized_copper_door_top", "oxidized_copper_door_bottom",
                 "oxidized_copper_door_bottom")
        .strength(3.0F).door().creative(CreativeCategory::Redstone),
    // Trapdoors (.trapdoor() supplies model + Facing/Half/Open/Powered; single sprite).
    BlockProperties::of(Block::SpruceTrapdoor, "spruce_trapdoor", "Spruce Trapdoor")
        .texture("spruce_trapdoor", "spruce_trapdoor", "spruce_trapdoor").strength(3.0F).trapdoor()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::JungleTrapdoor, "jungle_trapdoor", "Jungle Trapdoor")
        .texture("jungle_trapdoor", "jungle_trapdoor", "jungle_trapdoor").strength(3.0F).trapdoor()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::IronTrapdoor, "iron_trapdoor", "Iron Trapdoor")
        .texture("iron_trapdoor", "iron_trapdoor", "iron_trapdoor").strength(5.0F).trapdoor()
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::OxidizedCopperTrapdoor, "oxidized_copper_trapdoor",
                        "Oxidized Copper Trapdoor")
        .texture("oxidized_copper_trapdoor", "oxidized_copper_trapdoor", "oxidized_copper_trapdoor")
        .strength(3.0F).trapdoor().creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::WaxedOxidizedCopperTrapdoor, "waxed_oxidized_copper_trapdoor",
                        "Waxed Oxidized Copper Trapdoor")
        .texture("oxidized_copper_trapdoor", "oxidized_copper_trapdoor", "oxidized_copper_trapdoor")
        .strength(3.0F).trapdoor().creative(CreativeCategory::Redstone),
    // --- STRUCT AR-B batch 3: cross-model plants (see enum comment) -------------
    // Cobweb: a centred cross (no XZ jitter), no support requirement (it floats).
    BlockProperties::of(Block::Cobweb, "cobweb", "Cobweb")
        .texture("cobweb")
        .strength(4.0F)
        .cross()
        .noDrops()
        .creative(CreativeCategory::NaturalBlocks),
    // Flowers — the Dandelion recipe (XZ jitter, Soil support).
    BlockProperties::of(Block::Poppy, "poppy", "Poppy")
        .texture("poppy")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::OxeyeDaisy, "oxeye_daisy", "Oxeye Daisy")
        .texture("oxeye_daisy")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    // Fern and the double plants — Short Grass recipe (replaceable grass-like),
    // rendered from the bottom sprite (untinted for now).
    BlockProperties::of(Block::Fern, "fern", "Fern")
        .texture("fern")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::TallGrass, "tall_grass", "Tall Grass")
        .texture("tall_grass_bottom")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::LargeFern, "large_fern", "Large Fern")
        .texture("large_fern_bottom")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .replaceable()
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeadBush, "dead_bush", "Dead Bush")
        .texture("dead_bush")
        .instantBreak()
        .cross()
        .offsetType(BlockOffsetType::XZ)
        .noDrops()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    // Mushrooms — centred cross, Soil support (structure placement only needs the
    // render; the vanilla light/placement survival rules are not modelled here).
    BlockProperties::of(Block::RedMushroom, "red_mushroom", "Red Mushroom")
        .texture("red_mushroom")
        .instantBreak()
        .cross()
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::BrownMushroom, "brown_mushroom", "Brown Mushroom")
        .texture("brown_mushroom")
        .instantBreak()
        .cross()
        .light(1U)
        .support(BlockSupport::Soil)
        .creative(CreativeCategory::NaturalBlocks),
    // --- STRUCT AR-B batch 4: more no-new-model blocks (see enum comment) -------
    // Glazed terracotta (16 colours) — full cubes, pattern on every face.
    BlockProperties::of(Block::WhiteGlazedTerracotta, "white_glazed_terracotta",
                        "White Glazed Terracotta")
        .texture("white_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::OrangeGlazedTerracotta, "orange_glazed_terracotta",
                        "Orange Glazed Terracotta")
        .texture("orange_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::MagentaGlazedTerracotta, "magenta_glazed_terracotta",
                        "Magenta Glazed Terracotta")
        .texture("magenta_glazed_terracotta").strength(1.4F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightBlueGlazedTerracotta, "light_blue_glazed_terracotta",
                        "Light Blue Glazed Terracotta")
        .texture("light_blue_glazed_terracotta").strength(1.4F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::YellowGlazedTerracotta, "yellow_glazed_terracotta",
                        "Yellow Glazed Terracotta")
        .texture("yellow_glazed_terracotta").strength(1.4F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LimeGlazedTerracotta, "lime_glazed_terracotta",
                        "Lime Glazed Terracotta")
        .texture("lime_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PinkGlazedTerracotta, "pink_glazed_terracotta",
                        "Pink Glazed Terracotta")
        .texture("pink_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GrayGlazedTerracotta, "gray_glazed_terracotta",
                        "Gray Glazed Terracotta")
        .texture("gray_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightGrayGlazedTerracotta, "light_gray_glazed_terracotta",
                        "Light Gray Glazed Terracotta")
        .texture("light_gray_glazed_terracotta").strength(1.4F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::CyanGlazedTerracotta, "cyan_glazed_terracotta",
                        "Cyan Glazed Terracotta")
        .texture("cyan_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PurpleGlazedTerracotta, "purple_glazed_terracotta",
                        "Purple Glazed Terracotta")
        .texture("purple_glazed_terracotta").strength(1.4F)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlueGlazedTerracotta, "blue_glazed_terracotta",
                        "Blue Glazed Terracotta")
        .texture("blue_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BrownGlazedTerracotta, "brown_glazed_terracotta",
                        "Brown Glazed Terracotta")
        .texture("brown_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GreenGlazedTerracotta, "green_glazed_terracotta",
                        "Green Glazed Terracotta")
        .texture("green_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::RedGlazedTerracotta, "red_glazed_terracotta", "Red Glazed Terracotta")
        .texture("red_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlackGlazedTerracotta, "black_glazed_terracotta",
                        "Black Glazed Terracotta")
        .texture("black_glazed_terracotta").strength(1.4F).creative(CreativeCategory::ColoredBlocks),
    // Stained glass (16 colours) — translucent cubes, the Glass recipe.
    BlockProperties::of(Block::WhiteStainedGlass, "white_stained_glass", "White Stained Glass")
        .texture("white_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::OrangeStainedGlass, "orange_stained_glass", "Orange Stained Glass")
        .texture("orange_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::MagentaStainedGlass, "magenta_stained_glass", "Magenta Stained Glass")
        .texture("magenta_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightBlueStainedGlass, "light_blue_stained_glass",
                        "Light Blue Stained Glass")
        .texture("light_blue_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::YellowStainedGlass, "yellow_stained_glass", "Yellow Stained Glass")
        .texture("yellow_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LimeStainedGlass, "lime_stained_glass", "Lime Stained Glass")
        .texture("lime_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PinkStainedGlass, "pink_stained_glass", "Pink Stained Glass")
        .texture("pink_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GrayStainedGlass, "gray_stained_glass", "Gray Stained Glass")
        .texture("gray_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::LightGrayStainedGlass, "light_gray_stained_glass",
                        "Light Gray Stained Glass")
        .texture("light_gray_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::CyanStainedGlass, "cyan_stained_glass", "Cyan Stained Glass")
        .texture("cyan_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::PurpleStainedGlass, "purple_stained_glass", "Purple Stained Glass")
        .texture("purple_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlueStainedGlass, "blue_stained_glass", "Blue Stained Glass")
        .texture("blue_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BrownStainedGlass, "brown_stained_glass", "Brown Stained Glass")
        .texture("brown_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::GreenStainedGlass, "green_stained_glass", "Green Stained Glass")
        .texture("green_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::RedStainedGlass, "red_stained_glass", "Red Stained Glass")
        .texture("red_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::BlackStainedGlass, "black_stained_glass", "Black Stained Glass")
        .texture("black_stained_glass").strength(0.3F).renderLayer(BlockRenderLayer::Translucent)
        .creative(CreativeCategory::ColoredBlocks),
    // Misc full cubes.
    BlockProperties::of(Block::PackedIce, "packed_ice", "Packed Ice")
        .texture("packed_ice").strength(0.5F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::EndStoneBricks, "end_stone_bricks", "End Stone Bricks")
        .texture("end_stone_bricks").strength(3.0F, 9.0F).creative(CreativeCategory::BuildingBlocks),
    // Redstone lamp — rendered as its unlit cube for now (the lit-texture swap needs
    // the Cube path to carry a lit variant; deferred).
    BlockProperties::of(Block::RedstoneLamp, "redstone_lamp", "Redstone Lamp")
        .texture("redstone_lamp").strength(0.3F).creative(CreativeCategory::Redstone),
    // Pillars.
    BlockProperties::of(Block::HayBlock, "hay_block", "Hay Bale")
        .texture("hay_block_top", "hay_block_side", "hay_block_top").strength(0.5F).pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::StrippedOakLog, "stripped_oak_log", "Stripped Oak Log")
        .texture("stripped_oak_log_top", "stripped_oak_log", "stripped_oak_log_top").strength(2.0F)
        .pillar().creative(CreativeCategory::NaturalBlocks),
    // Pressure plates / button reusing existing shaped models.
    BlockProperties::of(Block::OakPressurePlate, "oak_pressure_plate", "Oak Pressure Plate")
        .texture("oak_planks").strength(0.5F).pressurePlate().creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::AcaciaPressurePlate, "acacia_pressure_plate", "Acacia Pressure Plate")
        .texture("acacia_planks").strength(0.5F).pressurePlate().creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::JungleButton, "jungle_button", "Jungle Button")
        .texture("jungle_planks").instantBreak().button().creative(CreativeCategory::Redstone),
    // --- STRUCT AR-B batch 5: family-completing cubes / pillars / leaves --------
    BlockProperties::of(Block::SmoothBasalt, "smooth_basalt", "Smooth Basalt")
        .texture("smooth_basalt").strength(1.25F, 4.2F).creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::BlueIce, "blue_ice", "Blue Ice")
        .texture("blue_ice").strength(2.8F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::CopperBlock, "copper_block", "Block of Copper")
        .texture("copper_block").strength(3.0F, 6.0F).creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::OxidizedCutCopper, "oxidized_cut_copper", "Oxidized Cut Copper")
        .texture("oxidized_cut_copper").strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WaxedOxidizedChiseledCopper, "waxed_oxidized_chiseled_copper",
                        "Waxed Oxidized Chiseled Copper")
        .texture("oxidized_chiseled_copper").strength(3.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::ReinforcedDeepslate, "reinforced_deepslate", "Reinforced Deepslate")
        .texture("reinforced_deepslate_top", "reinforced_deepslate_side",
                 "reinforced_deepslate_bottom")
        .strength(55.0F, 1200.0F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::Target, "target", "Target")
        .texture("target_top", "target_side", "target_top").strength(0.5F)
        .creative(CreativeCategory::Redstone),
    BlockProperties::of(Block::DiamondBlock, "diamond_block", "Block of Diamond")
        .texture("diamond_block").strength(5.0F, 6.0F).creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::LapisBlock, "lapis_block", "Block of Lapis Lazuli")
        .texture("lapis_block").strength(3.0F, 3.0F).creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::CoalBlock, "coal_block", "Block of Coal")
        .texture("coal_block").strength(5.0F, 6.0F).creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::MossBlock, "moss_block", "Moss Block")
        .texture("moss_block").strength(0.1F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::SmoothQuartz, "smooth_quartz", "Smooth Quartz Block")
        .texture("quartz_block_bottom").strength(2.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
    BlockProperties::of(Block::WhiteConcrete, "white_concrete", "White Concrete")
        .texture("white_concrete").strength(1.8F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::RedConcrete, "red_concrete", "Red Concrete")
        .texture("red_concrete").strength(1.8F).creative(CreativeCategory::ColoredBlocks),
    BlockProperties::of(Block::InfestedCobblestone, "infested_cobblestone", "Infested Cobblestone")
        .texture("cobblestone").strength(0.0F, 0.75F).creative(CreativeCategory::NaturalBlocks),
    // Pillars.
    BlockProperties::of(Block::SpruceWood, "spruce_wood", "Spruce Wood")
        .texture("spruce_log").strength(2.0F).pillar().creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::StrippedOakWood, "stripped_oak_wood", "Stripped Oak Wood")
        .texture("stripped_oak_log").strength(2.0F).pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::StrippedAcaciaLog, "stripped_acacia_log", "Stripped Acacia Log")
        .texture("stripped_acacia_log_top", "stripped_acacia_log", "stripped_acacia_log_top")
        .strength(2.0F).pillar().creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::MangroveLog, "mangrove_log", "Mangrove Log")
        .texture("mangrove_log_top", "mangrove_log", "mangrove_log_top").strength(2.0F).pillar()
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::MangroveWood, "mangrove_wood", "Mangrove Wood")
        .texture("mangrove_log").strength(2.0F).pillar().creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::MuddyMangroveRoots, "muddy_mangrove_roots", "Muddy Mangrove Roots")
        .texture("muddy_mangrove_roots_top", "muddy_mangrove_roots_side",
                 "muddy_mangrove_roots_top")
        .strength(0.7F).pillar().creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::MangroveRoots, "mangrove_roots", "Mangrove Roots")
        .texture("mangrove_roots_top", "mangrove_roots_side", "mangrove_roots_top")
        .strength(0.7F).pillar().renderLayer(BlockRenderLayer::Cutout)
        .creative(CreativeCategory::NaturalBlocks),
    // Leaves.
    BlockProperties::of(Block::MangroveLeaves, "mangrove_leaves", "Mangrove Leaves")
        .texture("mangrove_leaves").leaves().creative(CreativeCategory::NaturalBlocks),
    // --- STRUCT/WG terrain: copper ore + deepslate ore set ----------------------
    BlockProperties::of(Block::CopperOre, "copper_ore", "Copper Ore")
        .texture("copper_ore").strength(3.0F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateCoalOre, "deepslate_coal_ore", "Deepslate Coal Ore")
        .texture("deepslate_coal_ore").strength(4.5F, 3.0F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateIronOre, "deepslate_iron_ore", "Deepslate Iron Ore")
        .texture("deepslate_iron_ore").strength(4.5F, 3.0F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateCopperOre, "deepslate_copper_ore", "Deepslate Copper Ore")
        .texture("deepslate_copper_ore").strength(4.5F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateGoldOre, "deepslate_gold_ore", "Deepslate Gold Ore")
        .texture("deepslate_gold_ore").strength(4.5F, 3.0F).creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateRedstoneOre, "deepslate_redstone_ore",
                        "Deepslate Redstone Ore")
        .texture("deepslate_redstone_ore").strength(4.5F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateEmeraldOre, "deepslate_emerald_ore", "Deepslate Emerald Ore")
        .texture("deepslate_emerald_ore").strength(4.5F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateLapisOre, "deepslate_lapis_ore", "Deepslate Lapis Lazuli Ore")
        .texture("deepslate_lapis_ore").strength(4.5F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    BlockProperties::of(Block::DeepslateDiamondOre, "deepslate_diamond_ore", "Deepslate Diamond Ore")
        .texture("deepslate_diamond_ore").strength(4.5F, 3.0F)
        .creative(CreativeCategory::NaturalBlocks),
    // ENCH-2: EnchantingTableBlock. A 16x12x16 box (its VoxelShape is literally
    // `Block.column(16, 0, 12)`), so it is an ElementModel rather than a Cube —
    // which also keeps it out of isFullCube(), matching vanilla's
    // useShapeForLightOcclusion: a table does not seal a hole in a roof. Strength
    // 5/1200 is BlockBehaviour.Properties' `.strength(5.0F, 1200.0F)`. The book
    // that hovers above it is an entity-model block-entity renderer in vanilla
    // (EnchantTableRenderer) and is deliberately NOT faked with a static quad
    // here — registered as RN debt instead.
    BlockProperties::of(Block::EnchantingTable, "enchanting_table", "Enchanting Table")
        .texture("enchanting_table_top", "enchanting_table_side", "enchanting_table_bottom")
        .elementModel("enchanting_table_top", "enchanting_table_side",
                      "enchanting_table_bottom")
        .strength(5.0F, 1'200.0F)
        .container(ContainerType::EnchantingTable)
        .creative(CreativeCategory::Functional),
    // ENCH-3: the anvil, in its three wear states. All three share one model
    // (template_anvil's four boxes) and differ only in the top texture, so the
    // roster entries differ only there. FallingBlock in vanilla; this build has
    // no falling-anvil damage yet (registered as debt, not faked).
    BlockProperties::of(Block::Anvil, "anvil", "Anvil")
        .texture("anvil_top", "anvil", "anvil")
        .elementModel("anvil", "anvil_top")
        .strength(5.0F, 1'200.0F)
        .horizontalFacing()
        .container(ContainerType::Anvil)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::ChippedAnvil, "chipped_anvil", "Chipped Anvil")
        .texture("chipped_anvil_top", "anvil", "anvil")
        .elementModel("anvil", "chipped_anvil_top")
        .strength(5.0F, 1'200.0F)
        .horizontalFacing()
        .container(ContainerType::Anvil)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::DamagedAnvil, "damaged_anvil", "Damaged Anvil")
        .texture("damaged_anvil_top", "anvil", "anvil")
        .elementModel("anvil", "damaged_anvil_top")
        .strength(5.0F, 1'200.0F)
        .horizontalFacing()
        .container(ContainerType::Anvil)
        .creative(CreativeCategory::Functional),
    BlockProperties::of(Block::IronBlock, "iron_block", "Block of Iron")
        .texture("iron_block")
        .strength(5.0F, 6.0F)
        .creative(CreativeCategory::BuildingBlocks),
};

[[nodiscard]] constexpr bool isValidBlock(Block block) {
    return static_cast<std::uint16_t>(block) < static_cast<std::uint16_t>(Block::Count);
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

// RN-8a: whether this block occludes a neighbour's face at all — 26.1's
// `BlockBehaviour.Properties.canOcclude` (the flag `noOcclusion()` clears), the
// gate in front of `Block.shouldRenderFace`'s shape test. It is a *separate*
// axis from the render bucket and from light opacity; those three are one field
// here today, which is precisely the conflation RN-8 exists to unpick.
//
// The initial value is deliberately `renderLayer == Opaque` and nothing else.
// Writing it as `isFullCube` would make glass (a Cube model) start occluding its
// neighbours — a bigger regression than the bug being fixed — so this stays the
// conservative reading of today's behaviour, and the only behaviour change RN-8a
// makes comes from the *shape* half of the criterion. Giving Cutout blocks that
// are geometrically solid (stairs, walls, double slabs) their own `occludes`
// bit is RN-8e's, gated on FrameTrace evidence.
[[nodiscard]] constexpr bool canOcclude(Block block) {
    return isRenderable(block) && blockDefinition(block).renderLayer == BlockRenderLayer::Opaque;
}

[[nodiscard]] constexpr std::uint8_t skyLightOpacity(Block block) {
    if (isOpaque(block))
        return 15U;
    return blockDefinition(block).lightFilter;
}

[[nodiscard]] constexpr bool hasCollision(Block block) { return blockDefinition(block).collision; }

// The light a block emits in its *default* state. Blocks whose emission depends
// on a state — a furnace only glows while it burns — must be asked through
// BlockState::emittedLight() instead; this returns their unlit level.
[[nodiscard]] constexpr std::uint8_t emittedLight(Block block) {
    return blockDefinition(block).light;
}

[[nodiscard]] constexpr bool isTorch(Block block) { return blockDefinition(block).torch; }

// A wall-mounted torch (WallTorch, RedstoneWallTorch): a torch whose support is
// a wall rather than the ground. This is the one trait every torch consumer
// keys on to choose the leaning wall geometry over the upright floor one — the
// mesh model, the pick/collision/outline shape (BlockShape::shapeTorch) and any
// future torch behaviour — so a second wall-torch species (the redstone one)
// never needs another `== Block::WallTorch` identity line that could be missed.
// The floor torches (Torch, RedstoneTorch) are `isTorch && !isWallTorch`.
[[nodiscard]] constexpr bool isWallTorch(Block block) {
    return isTorch(block) && blockDefinition(block).support == BlockSupport::Wall;
}

// A redstone torch (standing or wall): a torch that carries a LIT state (the
// plain torches do not). This is what the mesher keys on to swap in the unlit
// `redstone_torch_off` sprite and to read the state's emitted light, so the two
// redstone torch species share one path instead of two identity checks.
[[nodiscard]] constexpr bool isRedstoneTorch(Block block) {
    return isTorch(block) && blockDefinition(block).lit;
}

// Wall torches sit flush against their wall, the way vanilla's WallTorchBlock
// AABB runs all the way to the block face (a north-facing torch spans z 11..16
// of 16). This is the inset of the model's root from the cell centre toward the
// wall; the mesh and the selection box share it so clicking matches the look.
inline constexpr float kWallTorchInset = 0.5F;

[[nodiscard]] constexpr bool isLog(Block block) { return blockDefinition(block).pillar; }

[[nodiscard]] constexpr bool isLeaves(Block block) { return blockDefinition(block).leaves; }

// A block's SoundType — the 26.1 BlockBehaviour.Properties.sound(...) group each
// block was registered with, transcribed from Blocks.java. Everything not named
// here falls to Stone, exactly as BlockBehaviour.Properties defaults to
// SoundType.STONE. Logs (isLog) and leaves (isLeaves) come from their traits;
// the rest are the small named lists vanilla's sound groups hold. Kept a
// constexpr switch over the closed Block enum (the compiler lowers it to a jump
// table) so the audio path never touches a string or a hash — the DOD floor the
// old ad-hoc audio-layer whitelist was reaching for, but complete and correct
// (dirt/podzol are GRAVEL not GRASS; wool is all 16 colours not 3; netherrack,
// nylium, basalt, the nether ores and soul blocks each get their own group
// instead of silently defaulting to stone).
[[nodiscard]] constexpr SoundType blockSoundType(Block block) {
    using enum Block;
    switch (block) {
    // WOOD — planks, wooden slabs/stairs/door/gate/trapdoor, and the wood-sound
    // decorations (torches, chests, crafting table, bookshelf, melon, pumpkin).
    case OakPlanks: case SprucePlanks: case BirchPlanks: case JunglePlanks:
    case AcaciaPlanks: case DarkOakPlanks:
    case OakSlab: case SpruceSlab: case BirchSlab: case JungleSlab: case AcaciaSlab:
    case DarkOakSlab:
    case OakStairs: case OakDoor: case OakFenceGate: case OakTrapdoor:
    case Bookshelf: case CraftingTable: case Chest: case TrappedChest:
    case Melon: case Pumpkin:
    case Torch: case WallTorch: case RedstoneTorch: case RedstoneWallTorch:
        return SoundType::Wood;
    // GRASS — the grass block, plants, saplings, sugar cane, and (per vanilla)
    // TNT.
    case Grass: case GrassPlant: case Dandelion: case SugarCane: case Tnt:
    case OakSapling: case SpruceSapling: case BirchSapling: case JungleSapling:
    case AcaciaSapling: case DarkOakSapling:
        return SoundType::Grass;
    // GRAVEL — dirt and its variants, clay, farmland, gravel.
    case Dirt: case CoarseDirt: case Podzol: case Clay: case Farmland: case Gravel:
        return SoundType::Gravel;
    // SAND.
    case Sand: case RedSand:
        return SoundType::Sand;
    // WOOL — all sixteen colours, and (per vanilla) fire.
    case WhiteWool: case OrangeWool: case MagentaWool: case LightBlueWool:
    case YellowWool: case LimeWool: case PinkWool: case GrayWool:
    case LightGrayWool: case CyanWool: case PurpleWool: case BlueWool:
    case BrownWool: case GreenWool: case RedWool: case BlackWool:
    case Fire:
        return SoundType::Wool;
    // GLASS — glass, glowstone and sea lantern.
    case Glass: case Glowstone: case SeaLantern:
        return SoundType::Glass;
    // CROP — wheat, carrots, potatoes.
    case WheatCrops: case Carrots: case Potatoes:
        return SoundType::Crop;
    case RedstoneBlock:
        return SoundType::Metal;
    case SnowBlock:
        return SoundType::Snow;
    case Basalt:
        return SoundType::Basalt;
    case CrimsonNylium: case WarpedNylium:
        return SoundType::Nylium;
    case NetherBricks:
        return SoundType::NetherBricks;
    case NetherQuartzOre:
        return SoundType::NetherOre;
    case NetherWartBlock:
        return SoundType::WartBlock;
    case Netherrack:
        return SoundType::Netherrack;
    case SoulSand:
        return SoundType::SoulSand;
    case SoulSoil:
        return SoundType::SoulSoil;
    // Fluids make no block sound.
    case Water: case Lava: case Air:
        return SoundType::Empty;
    default:
        break;
    }
    // Trait fallbacks for the regular wood/leaves families: the six leaves and
    // the six *logs* (a non-log pillar such as Basalt is handled in the switch
    // above, so it never reaches here). Everything else is Stone, vanilla's
    // Properties default.
    if (isLeaves(block)) return SoundType::Grass;
    if (isLog(block)) return SoundType::Wood;
    return SoundType::Stone;
}

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

// The deepslate form of a stone ore, for the y<0 deepslate band. Vanilla 26.1's
// ore configurations carry both a stone target and a deepslate target, so an ore
// vein crossing into the deepslate layer becomes its deepslate variant. Returns
// the block unchanged when it has no deepslate form, so the worldgen deepslate
// pass can convert any cell unconditionally and only ores/stone actually change.
[[nodiscard]] constexpr Block deepslateOreVariant(Block ore) {
    switch (ore) {
    case Block::CoalOre:
        return Block::DeepslateCoalOre;
    case Block::IronOre:
        return Block::DeepslateIronOre;
    case Block::CopperOre:
        return Block::DeepslateCopperOre;
    case Block::GoldOre:
        return Block::DeepslateGoldOre;
    case Block::RedstoneOre:
        return Block::DeepslateRedstoneOre;
    case Block::EmeraldOre:
        return Block::DeepslateEmeraldOre;
    case Block::LapisOre:
        return Block::DeepslateLapisOre;
    case Block::DiamondOre:
        return Block::DeepslateDiamondOre;
    default:
        return ore;
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
    // A DirectionalCube (observer) fills its cell exactly like a Cube — it occludes
    // and is face-sturdy — so it counts here; only its per-face texturing differs.
    const auto model = blockDefinition(block).model;
    return isRenderable(block) && !isFluid(block) &&
           (model == BlockModel::Cube || model == BlockModel::DirectionalCube);
}

// Java's BlockBehaviour#getLightDampening, the amount a block "shields" the cell
// above it in the eyes of the spreadable-block checks. This is *not* the light
// filter used for propagation (that is skyLightOpacity/lightFilter): vanilla
// reports 15 only when `isSolidRender` — i.e. when the block's OCCLUSION SHAPE
// is a full cube — and 0 otherwise, plus 3 here for water and lava so a fluid
// above a grass block still reverts it (vanilla spells that case out separately,
// as `getFluidState().isFull()`).
//
// The criterion is deliberately the SHAPE and not the render layer. Keying on
// isOpaque() made every Opaque-layer block that does not fill its cell report
// 15: the 26 slabs, the enchanting table and the three anvils, all of which
// vanilla lets grass live under. A slab is still a special case in the other
// direction — a DOUBLE slab *does* fill its cell — which is why the
// state-aware overload in BlockState.hpp is what the grass tick actually calls;
// this block-level one answers for the default state.
[[nodiscard]] constexpr int opacity(Block block) {
    if (block == Block::Water || block == Block::Lava)
        return 3;
    return isFullCube(block) && canOcclude(block) ? 15 : 0;
}

// Whether this block, drawn as an ITEM, uses the cube geometry rather than the
// extruded 2.5D icon sheet. The three item surfaces — the dropped ItemEntity,
// the first-person held item, and the HUD/inventory icon — all ask this one
// question, so they must ask it in one place.
//
// They used to each spell it out inline as `model == Cube || model == Chest ||
// ...`, and the three spellings disagreed: DirectionalCube (the observer) was
// added to the held and icon paths but missed on the drop path, so an observer
// lying on the ground rendered as a flat sprite while the same item in the hand
// and in the inventory rendered as a cube. Keying on the model set here is what
// makes a new BlockModel impossible to half-register.
//
// A Slab is in the set: it takes the same cube path, only with its Y extent
// halved — the caller reads isSlab() to decide that, exactly as before.
// Deliberately NOT the same set as isFullCube(): that one answers "does it fill
// its cell" (occlusion, face-sturdiness) and so excludes a chest and a slab,
// both of which nevertheless draw as boxes when held.
[[nodiscard]] constexpr bool rendersAsCubeItem(Block block) {
    const auto model = blockDefinition(block).model;
    return model == BlockModel::Cube || model == BlockModel::DirectionalCube ||
           model == BlockModel::Chest || model == BlockModel::Slab;
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

// Whether the block darkens a smooth-lighting AO corner (vanilla
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
[[nodiscard]] constexpr bool isSugarCane(Block block) { return block == Block::SugarCane; }
[[nodiscard]] constexpr bool isFire(Block block) { return block == Block::Fire; }

// AR-CX4-b: whether fire catches on this block. 26.1 keeps per-block burn odds
// on FireBlock; rebedrock derives the boolean from block traits so the answer
// stays a single constexpr source both the world-layer FireBlock#canSurvive rule
// and the gameplay random-tick (spread/burn-out) read, with no parallel list to
// drift. The flammable set is the wooden family: leaves, logs, any `_planks` or
// `_wool` block, and the bookshelf — exactly the subset of the current registry
// FireBlock.registerFlammable would touch. A data-pack-widened set is a later
// concern; this is the built-in floor.
[[nodiscard]] constexpr bool isFlammable(Block block) {
    const auto& definition = blockDefinition(block);
    if (definition.leaves || definition.pillar) {
        return true; // leaves and logs (pillar == RotatedPillarBlock)
    }
    if (block == Block::Bookshelf) {
        return true;
    }
    const std::string_view path = definition.identifier.path;
    return path.ends_with("_planks") || path.ends_with("_wool");
}
[[nodiscard]] constexpr bool isDestroyedByFluid(Block block) {
    // Sugar cane grows beside water on purpose, so — like a crop — it is not a
    // REPLACEABLE_PLANT and flowing water does not wash it away.
    return isRenderable(block) && !isFluid(block) && !isCrop(block) &&
           !isSugarCane(block) && !blockDefinition(block).collision;
}

[[nodiscard]] constexpr BlockSupport blockSupport(Block block) {
    return blockDefinition(block).support;
}

// VegetationBlock#mayPlaceOn in 26.1 (BushBlock in earlier versions).
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

// DirectionalBlock: does this block take a full six-way FACING from the placer's
// nearest looking direction? Observer and the piston family; everything else is
// false.
[[nodiscard]] constexpr bool hasDirectionalFacing(Block block) {
    return blockDefinition(block).directionalFacing;
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

// RN-4a: which of a DirectionalCube's texture faces a given world face shows, for
// a block whose FACING is `facing`. `faceDir` is the world face expressed as a
// BlockOrientation (North=-Z … Up=+Y, the same one-to-one map the mesher's
// faceMatchesOrientation uses). This mirrors vanilla ObserverBlock: the whole
// model rotates with FACING, so front stays on the FACING face, back on its
// opposite, and — for the remaining four faces — the top/bottom pair follows the
// world's vertical axis when FACING is horizontal, but rotates onto the world Z
// axis when FACING is vertical (up/down), exactly as observer.json's x/y
// blockstate rotations carry the top sprite around. `back` vs its powered variant
// is chosen by the caller, which knows the POWERED state.
enum class DirectionalSlot : std::uint8_t { Front, Back, Top, Bottom, Side };

[[nodiscard]] constexpr DirectionalSlot directionalCubeSlot(BlockOrientation facing,
                                                            BlockOrientation faceDir) {
    if (faceDir == facing) {
        return DirectionalSlot::Front;
    }
    if (faceDir == oppositeOrientation(facing)) {
        return DirectionalSlot::Back;
    }
    if (isHorizontal(facing)) {
        // FACING horizontal: the world vertical faces carry top/bottom, the two
        // horizontal faces perpendicular to FACING carry side.
        if (faceDir == BlockOrientation::Up) return DirectionalSlot::Top;
        if (faceDir == BlockOrientation::Down) return DirectionalSlot::Bottom;
        return DirectionalSlot::Side;
    }
    // FACING vertical (up/down): the top sprite has rotated onto the world Z axis,
    // leaving the X axis as side. (observer's top/bottom sprite are identical, so
    // both Z faces map to Top.)
    return (faceDir == BlockOrientation::North || faceDir == BlockOrientation::South)
        ? DirectionalSlot::Top
        : DirectionalSlot::Side;
}

// Observer's six-way faithfulness, pinned at compile time (vanilla
// blockstates/observer.json + models/block/observer.json).
static_assert(directionalCubeSlot(BlockOrientation::North, BlockOrientation::North) ==
              DirectionalSlot::Front);
static_assert(directionalCubeSlot(BlockOrientation::North, BlockOrientation::South) ==
              DirectionalSlot::Back);
static_assert(directionalCubeSlot(BlockOrientation::North, BlockOrientation::Up) ==
              DirectionalSlot::Top);
static_assert(directionalCubeSlot(BlockOrientation::North, BlockOrientation::East) ==
              DirectionalSlot::Side);
static_assert(directionalCubeSlot(BlockOrientation::East, BlockOrientation::East) ==
              DirectionalSlot::Front);
static_assert(directionalCubeSlot(BlockOrientation::East, BlockOrientation::West) ==
              DirectionalSlot::Back);
static_assert(directionalCubeSlot(BlockOrientation::East, BlockOrientation::Down) ==
              DirectionalSlot::Bottom);
static_assert(directionalCubeSlot(BlockOrientation::Up, BlockOrientation::Up) ==
              DirectionalSlot::Front);
static_assert(directionalCubeSlot(BlockOrientation::Up, BlockOrientation::Down) ==
              DirectionalSlot::Back);
static_assert(directionalCubeSlot(BlockOrientation::Up, BlockOrientation::North) ==
              DirectionalSlot::Top);
static_assert(directionalCubeSlot(BlockOrientation::Up, BlockOrientation::East) ==
              DirectionalSlot::Side);

// RN-4a: observer is now a full cube (occludes / face-sturdy). Regression guard
// against ever regressing it onto a non-cube placeholder that leaks light.
static_assert(isFullCube(Block::Observer));

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

// RN-4a: the resolved six-face layers of every DirectionalCube, filled by the
// atlas builder alongside kBlockTextureLayers. Non-directional blocks leave their
// entry zeroed; only the mesher's DirectionalCube branch reads this.
inline std::array<DirectionalTextureLayers, static_cast<std::size_t>(Block::Count)>
    kBlockDirectionalLayers{};

[[nodiscard]] inline const DirectionalTextureLayers& directionalLayers(Block block) {
    const auto index = static_cast<std::size_t>(block);
    return kBlockDirectionalLayers[index < kBlockDirectionalLayers.size() ? index : 0U];
}

// RN-8c-D: the five distinct texture layers a block ITEM needs. A dropped or held
// block is the block's own model with no blockstate rotation (vanilla's `ground`
// display transform is rotation [0,0,0]), so its front sits on the model's NORTH
// face and its back on the south one — the same faces the inventory icon reads.
//
// A plain cube has no front or back of its own, so both fall back to `side`,
// which is exactly what the item surfaces were doing for every block before this
// existed: a dropped piston was a uniform piston_side cube because the drop path
// only ever had top/side/bottom to give the shader.
//
// `front`, not `frontActive`: a block item has no state, and vanilla's item model
// is the unlit variant.
struct CubeItemLayers final {
    float top = 0.0F;
    float bottom = 0.0F;
    float side = 0.0F;
    float front = 0.0F;
    float back = 0.0F;
};

[[nodiscard]] inline bool cubeItemUsesBlockModel(Block block) {
    const auto& definition = blockDefinition(block);
    return definition.model == BlockModel::DirectionalCube &&
           definition.cubeItemModel == CubeItemModel::BlockModel;
}

[[nodiscard]] inline CubeItemLayers cubeItemLayers(Block block) {
    if (!cubeItemUsesBlockModel(block)) {
        // A plain cube item: the block's own texture triple, which is exactly what
        // a `cube_bottom_top` inventory model's top/side/bottom are. No front or
        // back of its own, so both answer side.
        const auto flat = textureLayers(block);
        return {flat.top, flat.bottom, flat.side, flat.side, flat.side};
    }
    const auto& faces = directionalLayers(block);
    return {faces.top, faces.bottom, faces.side, faces.front, faces.back};
}

// The UV model a block ITEM is drawn with. A plain cube item is a
// `cube_bottom_top`, which declares no face rotations, so it is Default whatever
// the block's own model does — the piston's world model rotates three of its
// faces, its inventory model rotates none.
[[nodiscard]] inline CubeUvModel cubeItemUvModel(Block block) {
    return cubeItemUsesBlockModel(block) ? blockDefinition(block).cubeUvModel
                                         : CubeUvModel::Default;
}

// The item shader has one spare float for everything `textureLayersRotation` has
// no room for: the front and back layers, and which cube UV model to sample. They
// travel packed into it as
//
//     1 + front + back * kItemLayerPackStride + uvModel * kItemUvModelPackStride
//
// The +1 keeps zero meaning "not supplied", which is what every item-cube draw
// that is not a block (the block-breaking overlay, a falling block) leaves it at,
// and what makes those keep their old side-on-every-face behaviour untouched.
//
// The stride bounds the atlas at 2047 layers, against a sprite section that starts
// at 203 and grows with the block roster; BlockAtlasBaker checks it. Everything
// stays exact in a float32 — every integer up to 2^24 is, and the widest value
// here is 2 * 2^22 + (2^22 - 1) = 12582911.
inline constexpr float kItemLayerPackStride = 2048.0F;
inline constexpr float kItemUvModelPackStride =
    kItemLayerPackStride * kItemLayerPackStride;

[[nodiscard]] inline float packItemCubeFaces(float front, float back, CubeUvModel uvModel) {
    return 1.0F + front + back * kItemLayerPackStride +
           static_cast<float>(uvModel) * kItemUvModelPackStride;
}

inline void setBlockDirectionalLayers(Block block, DirectionalTextureLayers layers) {
    kBlockDirectionalLayers[static_cast<std::size_t>(block)] = layers;
}

// RN-4a-2: the resolved atlas layers of every ElementModel block's texture slots,
// filled by the atlas builder from BlockDefinition::modelTextures. Only the
// mesher's per-block ElementModel transcription reads this.
inline std::array<std::array<float, kMaxModelTextureSlots>,
                  static_cast<std::size_t>(Block::Count)>
    kBlockModelSlotLayers{};

[[nodiscard]] inline float modelSlotLayer(Block block, std::size_t slot) {
    const auto index = static_cast<std::size_t>(block);
    const auto& slots = kBlockModelSlotLayers[index < kBlockModelSlotLayers.size() ? index : 0U];
    return slots[slot < slots.size() ? slot : 0U];
}

inline void setBlockModelSlotLayers(Block block,
                                    std::array<float, kMaxModelTextureSlots> layers) {
    kBlockModelSlotLayers[static_cast<std::size_t>(block)] = layers;
}

// The atlas layer of `redstone_torch_off`, the sprite an unlit redstone torch
// shows. A redstone torch's own `.texture()` is the *lit* sprite (its side
// layer); the off sprite is a second texture the mesher swaps in when the LIT
// state is false, the same on/off swap the furnace front makes. Set once by the
// atlas baker, like kBlockTextureLayers.
inline float kRedstoneTorchOffLayer = 0.0F;
inline void setRedstoneTorchOffLayer(float layer) { kRedstoneTorchOffLayer = layer; }
[[nodiscard]] inline float redstoneTorchOffLayer() { return kRedstoneTorchOffLayer; }

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
