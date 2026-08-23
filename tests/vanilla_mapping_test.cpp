// JC1: the vanilla <-> rebedrock mapping framework (compat/VanillaMapping.hpp).
//
// Covers what the header's own static_asserts cannot (registry-backed lookups
// that need registerBuiltinEntities() to have run, and the sabotage-friendly
// end-to-end BlockState assembly through applyMappedState): identity
// round-trip for block/item/entity, default-identity for same-named
// properties, the waterlogged<->submerged_in override in both directions,
// unknown-content skip (property, value and block), and the reverse-mapping
// placeholder's existence (JC4 seam, not behaviour).

#include "compat/VanillaMapping.hpp"

#include "gameplay/ItemRegistry.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/StateSchema.hpp"

#include <cassert>
#include <cstdint>

namespace {

namespace compat = mc::compat;
namespace world = mc::world;
namespace gameplay = mc::gameplay;

// --- Layer 1: identity --------------------------------------------------

void testBlockIdentity() {
    // A real vanilla name resolves to the block it names.
    const auto stone = compat::mapVanillaBlockName("minecraft:stone");
    assert(stone.has_value());
    assert(*stone == world::Block::Stone);

    // The bare path (no namespace) also resolves — the same convenience
    // world::blockFromIdentifier already gives every caller.
    assert(compat::mapVanillaBlockName("stone") == world::Block::Stone);

    // A vanilla-only block this build does not implement: skip (nullopt),
    // never abort. BlockIdRemap's "unknown -> invalid, not a crash" rule,
    // layer 1's version of it.
    assert(!compat::mapVanillaBlockName("minecraft:this_block_does_not_exist").has_value());
}

void testItemIdentity() {
    const auto* stick = compat::mapVanillaItemName("minecraft:stick");
    assert(stick != nullptr);
    assert(gameplay::itemId(stick) == gameplay::itemId(gameplay::itemFromIdentifier("stick")));

    assert(compat::mapVanillaItemName("minecraft:this_item_does_not_exist") == nullptr);
}

void testEntityIdentity() {
    mc::gameplay::entities::registerBuiltinEntities();

    const auto* cow = compat::mapVanillaEntityTypeName("minecraft:cow");
    assert(cow != nullptr);
    assert(cow == &mc::gameplay::entities::CowEntity::type());

    assert(compat::mapVanillaEntityTypeName("minecraft:this_mob_does_not_exist") == nullptr);
}

// --- Layer 2: state property/value, default identity ---------------------

void testDefaultIdentityBoolean() {
    // "lit" mirrors vanilla's AbstractFurnaceBlock.LIT by name (StateSchema.hpp
    // comment) and is boolean in both — the ordinary case defaultValueLookup
    // covers with no override entry at all.
    const auto mappedTrue = compat::mapVanillaState("lit", "true");
    assert(mappedTrue.valid());
    assert(mappedTrue.property == world::StateProperty::Lit);
    assert(mappedTrue.value == 1U);

    const auto mappedFalse = compat::mapVanillaState("lit", "false");
    assert(mappedFalse.valid());
    assert(mappedFalse.value == 0U);

    // End to end: applying it to a real furnace BlockState lights it, exactly
    // as BlockState::withLit(true) would.
    const auto furnace =
        compat::applyMappedState(world::BlockState{world::Block::Furnace}, mappedTrue);
    assert(furnace.lit());
}

void testDefaultIdentitySmallInteger() {
    // "age" mirrors CropBlock.AGE (0-7), a plain digit string in vanilla NBT.
    const auto mapped = compat::mapVanillaState("age", "5");
    assert(mapped.valid());
    assert(mapped.property == world::StateProperty::Age);
    assert(mapped.value == 5U);

    const auto crops =
        compat::applyMappedState(world::BlockState{world::Block::WheatCrops}, mapped);
    assert(crops.age() == 5);
}

// --- Layer 2: the registered deviation override (waterlogged) ------------

void testWaterloggedOverrideBothDirections() {
    // The core closed-loop assertion the task calls out: true -> water(1),
    // false -> none(0).
    const auto* override_ = compat::findOverride("waterlogged");
    assert(override_ != nullptr);
    const auto trueValue = override_->valueFn("true");
    assert(trueValue.has_value());
    assert(*trueValue == 1U);  // water
    const auto falseValue = override_->valueFn("false");
    assert(falseValue.has_value());
    assert(*falseValue == 0U);  // none

    // F2 landed: SubmergedFluid now exists (StateSchema.hpp), so
    // statePropertyFromName("submerged_in") resolves for real and the override
    // is no longer inert — this is JC1's forward registration activating
    // exactly as its own comment promised, with zero changes needed here in
    // VanillaMapping.hpp. The end-to-end assertion below is what proves that:
    // it builds a real BlockState off a real submergible block (a slab) and
    // checks the axis actually moved, not just that the mapped property/value
    // pair looks right in isolation.
    const auto mappedTrue = compat::mapVanillaState("waterlogged", "true");
    assert(mappedTrue.valid());
    assert(mappedTrue.property == world::StateProperty::SubmergedFluid);
    assert(mappedTrue.value == 1U);  // water
    const auto mappedFalse = compat::mapVanillaState("waterlogged", "false");
    assert(mappedFalse.valid());
    assert(mappedFalse.property == world::StateProperty::SubmergedFluid);
    assert(mappedFalse.value == 0U);  // none

    // End to end (the "current JC-1 test only isolates the value function"
    // gap the F-2 card calls out): apply both onto a real slab BlockState and
    // read the axis back through BlockState::submergedFluid(), not just the
    // raw MappedStateValue. none=0/water=1 is confirmed on the wire a real
    // save read/write would use.
    const auto dryOakSlab = compat::applyMappedState(
        world::BlockState{world::Block::OakSlab}, mappedFalse);
    assert(dryOakSlab.submergedFluid() == world::SubmergedFluid::None);
    const auto wetOakSlab = compat::applyMappedState(
        world::BlockState{world::Block::OakSlab}, mappedTrue);
    assert(wetOakSlab.submergedFluid() == world::SubmergedFluid::Water);
    // The slab's own shape axis is untouched by the override (SlabType stays
    // whatever it already was) — applyMappedState only writes the one
    // property the mapping named.
    assert(wetOakSlab.slabPortion() == world::SlabPortion::Bottom);

    // F2 extension (this pass): the same override, exercised end to end on a
    // stair now that OakStairs calls .submerges() too — the mapping itself
    // needed zero changes (it is keyed on the StateProperty, not the block),
    // but this proves that generality actually holds for the new block, not
    // just the slab it was originally proven against.
    const auto dryOakStairs = compat::applyMappedState(
        world::BlockState{world::Block::OakStairs, world::BlockOrientation::East}
            .withStairShape(world::StairShape::InnerRight),
        mappedFalse);
    assert(dryOakStairs.submergedFluid() == world::SubmergedFluid::None);
    const auto wetOakStairs = compat::applyMappedState(
        world::BlockState{world::Block::OakStairs, world::BlockOrientation::East}
            .withStairShape(world::StairShape::InnerRight),
        mappedTrue);
    assert(wetOakStairs.submergedFluid() == world::SubmergedFluid::Water);
    // Facing/StairShape survive the override untouched, same guarantee as the
    // slab's SlabType above.
    assert(wetOakStairs.orientation() == world::BlockOrientation::East);
    assert(wetOakStairs.stairShape() == world::StairShape::InnerRight);

    // Doors and fence gates are the negative case: applying the override onto
    // them is a no-op because neither declared the SubmergedFluid axis (JC-1's
    // own "absent property is a no-op" contract, inherited from StateSchema),
    // exactly matching that vanilla import of a JE waterlogged=true door/gate
    // (which cannot actually happen — neither carries WATERLOGGED in vanilla
    // either) would not silently manufacture a wet door.
    const auto doorMapped =
        compat::applyMappedState(world::BlockState{world::Block::OakDoor}, mappedTrue);
    assert(doorMapped.submergedFluid() == world::SubmergedFluid::None);
    const auto gateMapped =
        compat::applyMappedState(world::BlockState{world::Block::OakFenceGate}, mappedTrue);
    assert(gateMapped.submergedFluid() == world::SubmergedFluid::None);
}

void testWaterloggedOverrideUnknownValueSkips() {
    // An override that recognises the *property* but not this *value* still
    // skips rather than guessing.
    const auto* override_ = compat::findOverride("waterlogged");
    assert(override_ != nullptr);
    assert(!override_->valueFn("maybe").has_value());
}

// --- Unknown content: skip, never abort -----------------------------------

void testUnknownPropertySkips() {
    assert(!compat::mapVanillaState("this_property_does_not_exist", "true").valid());
}

void testUnknownValueShapeSkips() {
    // "facing" mirrors vanilla's DirectionProperty but vanilla spells its
    // values as words ("north"), which defaultValueLookup deliberately does
    // not guess at (see the comment on defaultValueLookup) — a property that
    // needs enum-word values is a future override, not a silent 0.
    assert(!compat::mapVanillaState("facing", "north").valid());
}

// --- Ledger sanity: the override table is exactly the registered deviation,
//     never queried for a same-named property (hot-path discipline: default
//     path costs zero override lookups). ----------------------------------

void testOverrideTableOnlyListsDeviations() {
    assert(compat::kOverrides.size() == 1);
    assert(compat::kOverrides[0].vanillaProperty == "waterlogged");
    // A property with no deviation is simply absent from the table — the
    // table is not an exhaustive property list, only exceptions to identity.
    assert(compat::findOverride("lit") == nullptr);
    assert(compat::findOverride("age") == nullptr);
    assert(compat::findOverride("moisture") == nullptr);
}

// --- Layer 4: reverse-mapping placeholder (JC4 seam), existence only ------

void testReverseSeamShape() {
    // hasVanillaIdentity is real and usable today: every built-in mirrors a
    // vanilla block except deliberately-original content, of which this build
    // currently has none, so every block reports true. The point of this
    // assertion is the *shape* (a caller can ask this before attempting an
    // export), not a specific original block existing yet.
    for (std::size_t i = 0; i < static_cast<std::size_t>(world::Block::Count); ++i) {
        const auto block = static_cast<world::Block>(i);
        // Every built-in registered so far mirrors vanilla (JC-DESIGN.md
        // §1's "vanilla id" row + je_mapping_test's own completeness audit).
        // If this ever goes false it means original content landed, which is
        // expected and fine — it is JC4's job to give it a substitute, not
        // this test's. So this only pins that the predicate runs cleanly over
        // every block without crashing, not a fixed answer.
        static_cast<void>(compat::hasVanillaIdentity(block));
    }
    // vanillaBlockNameForExport (JC4's reverse-identity seam) is declared in
    // VanillaMapping.hpp but deliberately has no definition anywhere in this
    // build (JC1's scope excludes JC4 — see the header's own comment on it).
    // Not called or referenced here on purpose: doing so would force a link
    // error, which is the *correct* outcome for "not implemented yet" but
    // would make this test binary fail to link rather than fail an assertion
    // — the wrong failure mode for ctest. Its declaration compiling (proven
    // when this translation unit includes the header at all) is the whole of
    // what JC1 promises for layer 4.
}

} // namespace

int main() {
    testBlockIdentity();
    testItemIdentity();
    testEntityIdentity();
    testDefaultIdentityBoolean();
    testDefaultIdentitySmallInteger();
    testWaterloggedOverrideBothDirections();
    testWaterloggedOverrideUnknownValueSkips();
    testUnknownPropertySkips();
    testUnknownValueShapeSkips();
    testOverrideTableOnlyListsDeviations();
    testReverseSeamShape();
    return 0;
}
