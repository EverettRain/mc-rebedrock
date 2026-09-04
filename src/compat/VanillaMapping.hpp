#pragma once

// JC1: the vanilla (Java Edition) <-> rebedrock mapping framework.
//
// What this is: a declarative registry that turns a JE blockstate — a
// `minecraft:` block name plus its `{propertyName: valueString}` map — into a
// rebedrock world::BlockState, for the eventual JE-save importer (JC3). It is
// deliberately *only* the mapping: no NBT, no Anvil `.mca`, no I/O (that is
// JC2/JC3 — see docs/content-dev/JC-je-compat/JC-DESIGN.md).
//
// Three layers, cheapest first:
//
//   1. Identity (block/item/entity name -> rebedrock id). Almost entirely
//      free: R0 already files a `minecraft:` alias beside every built-in's own
//      name (world::blockFromIdentifier, itemFromIdentifier,
//      EntityTypeRegistry::byId all already accept either). This file adds
//      nothing here beyond a name for the JC layer to call.
//
//   2. State property/value (vanilla property name + value string -> rebedrock
//      StateProperty + integer value). `StateSchema`'s property names already
//      mirror vanilla (StateSchema.hpp), so the default for a same-named
//      property is: resolve the name, then take the value verbatim — *except*
//      rebedrock's own save format stores a property's value as a small dense
//      integer while JE's blockstate values are strings ("true"/"false",
//      "north", "bottom", ...). So "default identity" for a *value* means "the
//      obvious 1:1 numbering" (bool: false=0/true=1; a small closed vanilla
//      enum: declaration order), which mapVanillaState below applies through
//      defaultValueLookup() when no override claims the property.
//
//      A block that has picked a representation that does not mirror vanilla
//      at all (the deviation-registration duty JC-je-compat/README.md and
//      REGULAR.md lay down) instead gets an explicit override entry: a
//      (vanillaProperty[, vanillaBlock]) -> rebedrock StateProperty, plus a
//      value-string -> integer function. The first and so far only such
//      deviation is F's `SubmergedFluid` axis, which does not exist as a
//      StateProperty yet (F2 is not landed) — see kOverrides below for why
//      that is fine to register today anyway.
//
//   3. Deviation ledger (docs, not code): every subtree that picks a
//      non-vanilla representation must show up in kDeviationLedger so the
//      obligation is visible from source, not only from the markdown that
//      first raised it.
//
// Everything here is read-only, rodata-shaped lookup: the override table is a
// fixed-size array built once at namespace scope (no heap, no static-init
// order dependency beyond StateSchema's own constexpr tables), and the default
// path — the overwhelming majority of properties, being same-named — never
// touches it at all.

#include "core/Identifier.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/StateSchema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::compat {

// ---------------------------------------------------------------------------
// Layer 1: identity. Thin, named wrappers over the R0 lookups that already
// accept a vanilla `minecraft:` name (or the bare path) — see
// world/Block.hpp:blockFromIdentifier, gameplay/ItemRegistry.hpp:
// itemFromIdentifier, gameplay/entities/EntityRegistry.hpp:
// EntityTypeRegistry::byId. JC1 does not reimplement any of these; it only
// gives the "vanilla name in, rebedrock identity out (or 'unknown, skip')"
// shape one name apiece for the importer to call, matching BlockIdRemap's
// "unknown maps to invalid/null, never abort" rule (gameplay/BlockIdRemap.hpp).
// ---------------------------------------------------------------------------

// vanilla block name -> Block, or nullopt when this build has no such block
// (vanilla content this build does not implement — skip, do not abort).
[[nodiscard]] constexpr std::optional<world::Block> mapVanillaBlockName(std::string_view vanillaName) {
    return world::blockFromIdentifier(vanillaName);
}

// vanilla item name -> Item*, or nullptr when unknown. Mirrors
// itemFromIdentifier's own null-for-unknown contract so a caller never has to
// special-case this wrapper away from the registry it wraps.
[[nodiscard]] inline const gameplay::Item* mapVanillaItemName(std::string_view vanillaName) {
    return gameplay::itemFromIdentifier(vanillaName);
}

// vanilla entity type name -> EntityType*, or nullptr when unknown.
[[nodiscard]] inline const gameplay::entities::EntityType*
mapVanillaEntityTypeName(std::string_view vanillaName) {
    return gameplay::entities::entityTypeRegistry().byId(vanillaName);
}

// ---------------------------------------------------------------------------
// Layer 2: state property/value.
// ---------------------------------------------------------------------------

// The result of mapping one vanilla `(propertyName, valueString)` pair.
struct MappedStateValue final {
    world::StateProperty property = world::StateProperty::Count;
    std::uint8_t value = 0U;

    // Count is StateSchema's own "no such property" sentinel
    // (statePropertyFromName), reused here so a caller tests one thing.
    [[nodiscard]] constexpr bool valid() const { return property != world::StateProperty::Count; }
};

// A value-string -> integer function for one property's override. Returns
// nullopt for a value string the override does not recognise (still "skip",
// never abort) so an override can cover a subset of a vanilla property's
// values without a wildcard fallthrough silently mapping the rest to 0.
using OverrideValueFn = std::optional<std::uint8_t> (*)(std::string_view vanillaValue);

// One entry in the deviation override table: a vanilla property name maps to
// a rebedrock StateProperty whose *meaning* differs enough from vanilla's that
// the default identity value mapping (defaultValueLookup, below) would be
// wrong — so this entry supplies the value function instead.
//
// `vanillaBlock` is reserved for a future override that only applies under one
// specific block (empty string = applies whenever the property name matches,
// which is every override registered so far; JC-DESIGN.md's three-layer model
// allows a block-scoped override without changing this shape — a widened
// lookup, not a format change).
struct StateOverride final {
    std::string_view vanillaBlock;     // empty = applies regardless of block
    std::string_view vanillaProperty;  // JE's blockstate property name
    world::StateProperty rebedrockProperty;
    OverrideValueFn valueFn;
};

namespace detail {

// waterlogged=true/false -> submerged_in=water/none. The first registered
// deviation (F-fluid/F-DESIGN.md §3D): SubmergedFluid does not mirror
// vanilla's boolean, so it cannot go through defaultValueLookup.
//
// SubmergedFluid is not a StateProperty yet — F2 has not landed (see
// docs/content-dev/F-fluid/README.md's node table). That is fine to register
// now: the override *name*s the rebedrock property through
// world::statePropertyFromName("submerged_in"), which today returns
// StateProperty::Count (StateSchema's own "unknown name" answer) and will
// start resolving for real the moment F2 adds the enumerator — zero change
// needed here. mapVanillaState below already treats an override whose target
// resolves to Count as "unknown, skip", so until F2 lands this entry is inert
// rather than wrong.
[[nodiscard]] constexpr std::optional<std::uint8_t> waterloggedToSubmergedIn(std::string_view value) {
    // Value names chosen to match F-2-submerged-fluid-axis.md's serialisation:
    // submerged_in's own values are named "none"/"water", so this is also the
    // shape a *rebedrock* save's statePropertyName/value pair already takes —
    // the override's job is solely translating vanilla's "true"/"false".
    if (value == "true") return std::uint8_t{1U};  // none=0, water=1 (F-2 §19)
    if (value == "false") return std::uint8_t{0U};
    return std::nullopt;
}

// --- The enum-word properties -----------------------------------------------
//
// defaultValueLookup below reads vanilla's two *shapes* — a boolean and a plain
// integer — and its comment names the gap it deliberately leaves: "a property
// whose vanilla values are enum words (facing's north/south/...) is exactly the
// kind of thing that needs an override". These are those overrides.
//
// They are not a new parser: each is the same "vanilla word -> this build's
// enumerator ordinal" table the enumerator's own declaration already fixes, and
// each cites where that ordinal comes from. Registering them here rather than in
// a caller is what keeps one answer to "what does facing=north mean" for the JC
// save bridge and for RN-15's `--test-scene oak_trapdoor[half=top]` alike.

// BlockOrientation's declaration order (Block.hpp): North, East, South, West,
// Up, Down. Vanilla's Direction names are the same six words.
[[nodiscard]] constexpr std::optional<std::uint8_t> facingWord(std::string_view value) {
    if (value == "north") return std::uint8_t{0U};
    if (value == "east") return std::uint8_t{1U};
    if (value == "south") return std::uint8_t{2U};
    if (value == "west") return std::uint8_t{3U};
    if (value == "up") return std::uint8_t{4U};
    if (value == "down") return std::uint8_t{5U};
    return std::nullopt;
}

// StateProperty::Half carries three vanilla properties at once, and they spell
// their two values differently: a stair and a trapdoor say top/bottom
// (Half.TOP/BOTTOM), a door says upper/lower (DoubleBlockHalf.UPPER/LOWER).
// BlockState reads the same bit for all three (isDoorUpperHalf, trapdoorHalf),
// so all four words resolve here: 0 is the lower one, matching SlabPortion's
// Bottom == 0.
[[nodiscard]] constexpr std::optional<std::uint8_t> halfWord(std::string_view value) {
    if (value == "bottom" || value == "lower") return std::uint8_t{0U};
    if (value == "top" || value == "upper") return std::uint8_t{1U};
    return std::nullopt;
}

// SlabBlock.TYPE -> SlabPortion: Bottom, Top, Double (Block.hpp's declaration).
[[nodiscard]] constexpr std::optional<std::uint8_t> slabTypeWord(std::string_view value) {
    if (value == "bottom") return std::uint8_t{0U};
    if (value == "top") return std::uint8_t{1U};
    if (value == "double") return std::uint8_t{2U};
    return std::nullopt;
}

// DoorBlock.HINGE -> DoorHinge: Left, Right.
[[nodiscard]] constexpr std::optional<std::uint8_t> hingeWord(std::string_view value) {
    if (value == "left") return std::uint8_t{0U};
    if (value == "right") return std::uint8_t{1U};
    return std::nullopt;
}

// StairBlock.SHAPE -> StairShape: Straight, InnerLeft, InnerRight, OuterLeft,
// OuterRight.
[[nodiscard]] constexpr std::optional<std::uint8_t> stairShapeWord(std::string_view value) {
    if (value == "straight") return std::uint8_t{0U};
    if (value == "inner_left") return std::uint8_t{1U};
    if (value == "inner_right") return std::uint8_t{2U};
    if (value == "outer_left") return std::uint8_t{3U};
    if (value == "outer_right") return std::uint8_t{4U};
    return std::nullopt;
}

// RepeaterBlock.DELAY is spelled 1..4 in a vanilla blockstate and stored 0..3
// here (BlockState::repeaterDelay adds the one back). The name and the shape both
// look like an identity integer, which is exactly why this needs a row: without
// it `repeater[delay=3]` maps to the stored 3, i.e. a FOUR-tick repeater, and
// nothing anywhere says so. The only off-by-one of its kind in the schema —
// every other integer property (age, moisture, level, signal) is a true identity.
[[nodiscard]] constexpr std::optional<std::uint8_t> repeaterDelayValue(std::string_view value) {
    if (value == "1") return std::uint8_t{0U};
    if (value == "2") return std::uint8_t{1U};
    if (value == "3") return std::uint8_t{2U};
    if (value == "4") return std::uint8_t{3U};
    return std::nullopt;
}

// ComparatorBlock.MODE -> BlockState::comparatorSubtract's bit: compare is 0.
[[nodiscard]] constexpr std::optional<std::uint8_t> comparatorModeWord(std::string_view value) {
    if (value == "compare") return std::uint8_t{0U};
    if (value == "subtract") return std::uint8_t{1U};
    return std::nullopt;
}

} // namespace detail

// The override table. constexpr array, built once at namespace scope: no
// heap, no runtime construction order to reason about. Grows by appending a
// row, not by restructuring (REGULAR.md rule 5, "override 表 constexpr/静态；
// 导入是查表+下标，量增再迁 D 数据化").
inline constexpr std::array<StateOverride, 8> kOverrides{{
    StateOverride{
        /*vanillaBlock=*/{},
        /*vanillaProperty=*/"waterlogged",
        /*rebedrockProperty=*/world::statePropertyFromName("submerged_in"),
        /*valueFn=*/&detail::waterloggedToSubmergedIn,
    },
    // The six enum-word properties. Same name on both sides, different value
    // *spelling* — which is precisely what an override entry is for.
    StateOverride{{}, "facing", world::StateProperty::Facing, &detail::facingWord},
    StateOverride{{}, "half", world::StateProperty::Half, &detail::halfWord},
    StateOverride{{}, "type", world::StateProperty::SlabType, &detail::slabTypeWord},
    StateOverride{{}, "hinge", world::StateProperty::Hinge, &detail::hingeWord},
    StateOverride{{}, "shape", world::StateProperty::StairShape, &detail::stairShapeWord},
    StateOverride{{}, "mode", world::StateProperty::ComparatorMode, &detail::comparatorModeWord},
    // Not an enum word — an integer whose origin differs. See repeaterDelayValue.
    StateOverride{{}, "delay", world::StateProperty::Delay, &detail::repeaterDelayValue},
}};

// Finds the override for a vanilla property name (optionally scoped to a
// vanilla block name), or nullptr when this property has no deviation and
// should go through the default identity path instead. Linear scan over
// kOverrides: the table is small by design (rule 5 above), so this is a
// handful of string comparisons, not a hash lookup — and the hot import path
// (JC3, not yet built) is expected to call this once per non-default
// property, not once per block in a chunk.
[[nodiscard]] constexpr const StateOverride* findOverride(std::string_view vanillaProperty,
                                                           std::string_view vanillaBlock = {}) {
    for (const StateOverride& entry : kOverrides) {
        if (entry.vanillaProperty != vanillaProperty) continue;
        if (!entry.vanillaBlock.empty() && entry.vanillaBlock != vanillaBlock) continue;
        return &entry;
    }
    return nullptr;
}

// The default (no-override) value mapping: the obvious 1:1 numbering for a
// same-named, same-shaped vanilla property. This is deliberately narrow — it
// only knows the two vanilla value shapes this project's schema currently
// uses (a boolean, or a plain non-negative integer already spelled as
// digits) — because *every* property this build declares that is not in
// kOverrides is, by the "names mirror vanilla" rule (StateSchema.hpp), one of
// those two shapes. A property whose vanilla values are enum words (facing's
// "north"/"south"/...) is exactly the kind of thing that needs an override
// (or a future addition here) rather than silently defaulting to 0; this
// function returns nullopt for anything it does not recognise so the caller's
// "unknown, skip" rule catches it instead of guessing.
[[nodiscard]] constexpr std::optional<std::uint8_t> defaultValueLookup(std::string_view vanillaValue) {
    if (vanillaValue == "true") return std::uint8_t{1U};
    if (vanillaValue == "false") return std::uint8_t{0U};
    // A plain small integer, as vanilla spells age/level/moisture/etc:
    // hand-rolled so this stays constexpr without <charconv>'s runtime-only
    // from_chars on some standard libraries.
    if (!vanillaValue.empty()) {
        std::uint32_t parsed = 0U;
        bool allDigits = true;
        for (const char c : vanillaValue) {
            if (c < '0' || c > '9') { allDigits = false; break; }
            parsed = parsed * 10U + static_cast<std::uint32_t>(c - '0');
        }
        if (allDigits && parsed <= 255U) {
            return static_cast<std::uint8_t>(parsed);
        }
    }
    return std::nullopt;
}

// Maps one vanilla `(propertyName, valueString)` pair to a rebedrock
// StateProperty + value, optionally scoped to the vanilla block name (for a
// future block-scoped override; every override registered today is
// block-agnostic). Three outcomes, all "valid()==false, caller skips" except
// the first:
//
//   * override claims the property, value resolves       -> valid, mapped
//   * override claims the property, value does not resolve -> invalid (skip:
//     an override that recognises the *property* but not this particular
//     *value* is exactly the "JE has it, rebedrock doesn't (yet)" case)
//   * no override, name does not resolve (statePropertyFromName -> Count)
//     -> invalid (skip: unrecognised property name, same as an old/new save
//     naming something this build has no notion of, StateSchema.hpp's own
//     rule)
//   * no override, name resolves, value does not parse    -> invalid (skip)
//
// Never aborts on unknown input — REGULAR.md rule 3 ("未知内容跳过不 abort").
[[nodiscard]] constexpr MappedStateValue mapVanillaState(std::string_view vanillaProperty,
                                                          std::string_view vanillaValue,
                                                          std::string_view vanillaBlock = {}) {
    if (const StateOverride* override_ = findOverride(vanillaProperty, vanillaBlock)) {
        if (override_->rebedrockProperty == world::StateProperty::Count) {
            // Named a rebedrock property that does not exist in this build yet
            // (see detail::waterloggedToSubmergedIn's comment) — inert, not an
            // error.
            return {};
        }
        if (const auto mapped = override_->valueFn(vanillaValue); mapped.has_value()) {
            return MappedStateValue{override_->rebedrockProperty, *mapped};
        }
        return {};
    }
    const world::StateProperty property = world::statePropertyFromName(vanillaProperty);
    if (property == world::StateProperty::Count) {
        return {};  // JE has it, this build's schema does not: skip.
    }
    if (const auto mapped = defaultValueLookup(vanillaValue); mapped.has_value()) {
        return MappedStateValue{property, *mapped};
    }
    return {};  // named property, unrecognised value shape: skip.
}

// Applies mapVanillaState's result onto a BlockState, when it mapped to
// something. A no-op MappedStateValue (property==Count) leaves `state`
// untouched — the "rebedrock has no notion of this property, default
// (0)" rule BlockState::with already gives every property the block itself
// does not declare (BlockState.hpp's own comment on `with`/`has`).
[[nodiscard]] constexpr world::BlockState applyMappedState(world::BlockState state,
                                                            MappedStateValue mapped) {
    if (!mapped.valid()) return state;
    return state.with(mapped.property, mapped.value);
}

// ---------------------------------------------------------------------------
// Layer 4 (JC-DESIGN.md's "逆向占位"): the reverse-mapping surface for JC4
// (rebedrock -> JE export), left unimplemented here on purpose. JC1's scope is
// the import direction only (JC-1-mapping-framework.md "Scope" — "本节点只落
// 框架" — and "明确不做…导出实现（JC4）"). The shape below exists so JC4 has a
// declared seam to fill rather than having to invent one, and so a reader can
// see at a glance which layers have no inverse at all (original content has
// no vanilla name to map back to).
// ---------------------------------------------------------------------------

// Whether a rebedrock block has a vanilla name to export back to. False for
// original (non-vanilla-mirroring) content, which JC4 must instead replace
// with a best-effort substitute or skip on export — there is no vanilla
// identity to hand back for something vanilla never had.
[[nodiscard]] constexpr bool hasVanillaIdentity(world::Block block) {
    return !world::blockDefinition(block).vanilla.empty();
}

// JC4's reverse identity mapping: Block -> vanilla name. Declared, not
// defined, here — a real body belongs to JC4 alongside the export writer that
// would call it (REGULAR.md rule 6, "格式层与语义层分离" extends to "import
// framework and export framework are different milestones"). Any translation
// unit that references this without also linking JC4's definition will fail
// at link time, which is the intended "not implemented yet" signal for a
// function whose contract (round-trip every vanilla-mirroring block, and
// name what happens to original content) does not exist until JC4 decides it.
[[nodiscard]] core::Identifier vanillaBlockNameForExport(world::Block block);

// ---------------------------------------------------------------------------
// Compile-time pins. Small enough to keep beside the code they pin rather than
// only in the test binary — same spirit as BlockState.hpp's own static_asserts
// (BlockState.hpp:200-208): a regression here fails the build, not just ctest.
// ---------------------------------------------------------------------------

// Identity: an unknown vanilla block name is "not found", never a crash.
static_assert(!mapVanillaBlockName("minecraft:this_does_not_exist").has_value());
// Identity: a real vanilla block resolves through its `minecraft:` alias.
static_assert(mapVanillaBlockName("minecraft:stone") == world::Block::Stone);

// Default identity: a same-named boolean property round-trips 1:1.
static_assert(mapVanillaState("lit", "true").property == world::StateProperty::Lit);
static_assert(mapVanillaState("lit", "true").value == 1U);
static_assert(mapVanillaState("lit", "false").value == 0U);
// Default identity: a same-named small-integer property round-trips verbatim.
static_assert(mapVanillaState("age", "5").property == world::StateProperty::Age);
static_assert(mapVanillaState("age", "5").value == 5U);

// Unknown property name: skip, do not guess.
static_assert(!mapVanillaState("this_property_does_not_exist", "true").valid());

// The one registered override: waterlogged is caught by name even before F2
// lands SubmergedFluid (findOverride matches on the string, independent of
// whether the target StateProperty exists yet).
static_assert(findOverride("waterlogged") != nullptr);
// The override is scoped to "waterlogged" only — a differently-named property
// goes through the default path and is untouched by the override table
// (REGULAR.md rule 2, "同名属性走恒等、不进 override" — the hot default path
// never even calls findOverride for the properties it need not, but the
// converse also holds: an override never hijacks a property it does not name).
static_assert(findOverride("lit") == nullptr);
static_assert(findOverride("age") == nullptr);

} // namespace mc::compat
