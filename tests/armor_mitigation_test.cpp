// EQ-2: filling Damage.hpp's armor/toughness stage. Three layers: the pure
// formula in isolation (damageAfterArmor), the pipeline stage wired into
// applyDamage (bypass respected, sequencing against the other named stages),
// and the end-to-end player path through GameSession::hurtPlayer (armor
// summed from EquipmentSlots, durability spent on a landed non-bypassed hit).

#include "gameplay/Damage.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/Item.hpp"

#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

using namespace mc::gameplay;

[[nodiscard]] bool nearly(float value, float expected, float epsilon = 0.001F) {
    return std::fabs(value - expected) < epsilon;
}

struct TestHost final : SimulationHost {
    bool playerDied = false;

    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override { playerDied = true; }
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// --- Layer 1: the pure formula, DamageUtil#getDamageLeft transcribed exactly. ---

// Sabotage① target: drop the toughness term or the clamp and this breaks.
void testDamageAfterArmorWorkedExamples() {
    // The card's own worked example: 10 damage vs 20 armor / 0 toughness.
    // f = 2 + 0/4 = 2; g = clamp(20 - 10/2, 4, 20) = clamp(15, 4, 20) = 15;
    // 10 * (1 - 15/25) = 10 * 0.6 = 4.0.
    assert(nearly(damageAfterArmor(10.0F, 20.0F, 0.0F), 4.0F));

    // Zero armor changes nothing: g = clamp(0 - d/f, 0, 20) = 0 for any
    // positive damage, so the multiplier is exactly 1.
    assert(nearly(damageAfterArmor(10.0F, 0.0F, 0.0F), 10.0F));
    assert(nearly(damageAfterArmor(4.0F, 0.0F, 0.0F), 4.0F));

    // Full iron (15 armor, 0 toughness) against 10 damage: f = 2;
    // g = clamp(15 - 5, 3, 20) = 10; 10 * (1 - 10/25) = 6.0.
    assert(nearly(damageAfterArmor(10.0F, 15.0F, 0.0F), 6.0F));

    // Full diamond (20 armor, 8 toughness) against the same 10 damage reduces
    // further than iron because of the toughness term shrinking the
    // subtracted d/f: f = 2 + 8/4 = 4; g = clamp(20 - 2.5, 4, 20) = 17.5;
    // 10 * (1 - 17.5/25) = 3.0.
    assert(nearly(damageAfterArmor(10.0F, 20.0F, 8.0F), 3.0F));

    // The toughness term matters most on big hits: diamond vs a 20-damage
    // blow reduces to 8.0, strictly better than the same 20 armor with no
    // toughness would (12.0) — the sabotage① regression this pins.
    assert(nearly(damageAfterArmor(20.0F, 20.0F, 8.0F), 8.0F));
    assert(nearly(damageAfterArmor(20.0F, 20.0F, 0.0F), 12.0F));

    std::cout << "testDamageAfterArmorWorkedExamples OK\n";
}

// The armor*0.2 floor of the clamp: enormous damage cannot push the
// reduction below one fifth of the armor value.
void testDamageAfterArmorClampFloor() {
    // f = 2; g = clamp(20 - 500, 4, 20) = 4 (the floor, not the negative raw
    // value); 500 * (1 - 4/25) = 500 * 0.84 = 420.
    assert(nearly(damageAfterArmor(500.0F, 20.0F, 0.0F), 420.0F));
    std::cout << "testDamageAfterArmorClampFloor OK\n";
}

// --- Layer 2: the pipeline stage, applyDamage() wired to the formula. ---

// Sabotage② target: BypassesArmor must short-circuit the stage entirely.
void testPipelineRespectsBypassesArmor() {
    // OutOfWorld carries BypassesArmor — full armor does not reduce the void.
    DamageState voided{20.0F, 20.0F};
    const auto outcome = applyDamage(
        voided, DamageContext{DamageType::OutOfWorld, 10.0F, Difficulty::Normal, false, 20.0F, 8.0F});
    assert(outcome.landed);
    assert(nearly(outcome.appliedDamage, 10.0F));  // unreduced
    assert(!outcome.armorApplied);
    assert(nearly(voided.health, 10.0F));

    // Fall and Drown carry the same tag.
    DamageState fallen{20.0F, 20.0F};
    const auto fallOutcome = applyDamage(
        fallen, DamageContext{DamageType::Fall, 6.0F, Difficulty::Normal, false, 20.0F, 8.0F});
    assert(nearly(fallOutcome.appliedDamage, 6.0F));
    assert(!fallOutcome.armorApplied);

    std::cout << "testPipelineRespectsBypassesArmor OK\n";
}

// A type that does NOT carry BypassesArmor (a mob's swing, an arrow) is
// reduced by the formula, matching the earlier pure-function example exactly.
void testPipelineAppliesArmorWhenNotBypassed() {
    DamageState hit{20.0F, 20.0F};
    const auto outcome = applyDamage(
        hit,
        DamageContext{DamageType::EntityAttack, 10.0F, Difficulty::Normal, true, 20.0F, 0.0F});
    assert(outcome.landed);
    assert(outcome.armorApplied);
    assert(nearly(outcome.appliedDamage, 4.0F));
    assert(nearly(hit.health, 16.0F));

    // Zero armor (the naked case) is a no-op reduction — the whole scaled
    // amount lands, same as before EQ-2 existed.
    DamageState naked{20.0F, 20.0F};
    const auto nakedOutcome = applyDamage(
        naked, DamageContext{DamageType::EntityAttack, 10.0F, Difficulty::Normal, true});
    assert(nearly(nakedOutcome.appliedDamage, 10.0F));
    assert(!nakedOutcome.armorApplied);  // context.armor == 0.0F, stage skipped

    std::cout << "testPipelineAppliesArmorWhenNotBypassed OK\n";
}

// Sabotage③ target: the stage must sit where the pipeline names it — after
// difficulty scaling, before the (still-empty) effects/absorption/shield
// stages, never after health is applied. Difficulty scaling and armor
// reduction must compose in the documented order (armor reduces the
// *difficulty-scaled* amount, not the other way around).
void testPipelineStageOrderMatchesDifficultyThenArmor() {
    // Hard multiplies an EntityAttack swing by 1.5 first; THEN armor reduces
    // the scaled amount. 4.0 * 1.5 = 6.0 pre-armor; against 15 armor/0
    // toughness: f=2, g=clamp(15-3,3,20)=12, 6*(1-12/25)=6*0.52=3.12.
    DamageState hard{20.0F, 20.0F};
    const auto outcome = applyDamage(
        hard,
        DamageContext{DamageType::EntityAttack, 4.0F, Difficulty::Hard, true, 15.0F, 0.0F});
    assert(outcome.armorApplied);
    assert(nearly(outcome.appliedDamage, 3.12F, 0.01F));
    // preArmorDamage reports the post-difficulty, pre-reduction value (6.0),
    // the number durability spends off of — proof the stage runs after
    // difficulty scaling, not before it or standalone.
    assert(nearly(outcome.preArmorDamage, 6.0F));

    std::cout << "testPipelineStageOrderMatchesDifficultyThenArmor OK\n";
}

// --- Layer 3: end to end through GameSession — armor summed from
// EquipmentSlots, and armor durability spent on the pieces that absorbed. ---

[[nodiscard]] ItemStack armorStack(const Item* item) {
    return ItemStack{mc::world::Block::Air, 1U, item};
}

// A naked player and a full-iron player take a mob's swing differently, and
// the iron pieces lose durability while the naked player has nothing to wear.
void testEndToEndIronReducesMeleeDamage() {
    mc::world::World world;

    GameSession naked;
    naked.setGameMode(GameMode::Survival);
    TestHost nakedHost;
    const bool nakedLanded =
        naked.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 10.0F, nakedHost, true);
    assert(nakedLanded);
    assert(nearly(naked.vitals().health(), 10.0F));  // 20 - 10, unreduced

    GameSession armored;
    armored.setGameMode(GameMode::Survival);
    armored.equipment().set(EquipmentSlot::Head, armorStack(&items::IronHelmet));
    armored.equipment().set(EquipmentSlot::Chest, armorStack(&items::IronChestplate));
    armored.equipment().set(EquipmentSlot::Legs, armorStack(&items::IronLeggings));
    armored.equipment().set(EquipmentSlot::Feet, armorStack(&items::IronBoots));
    TestHost armoredHost;
    const bool armoredLanded =
        armored.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 10.0F, armoredHost, true);
    assert(armoredLanded);
    // Full iron is 15 armor / 0 toughness: damageAfterArmor(10, 15, 0) = 6.0.
    assert(nearly(armored.vitals().health(), 14.0F));  // 20 - 6

    // Every equipped piece lost durability: max(1, preArmorDamage/4) with
    // preArmorDamage == 10 (Normal difficulty, no scaling) -> floor(10/4) = 2.
    for (const EquipmentSlot slot : kArmorSlots) {
        assert(armored.equipment().get(slot).damage == 2U);
    }
    // The offhand (not armor) is untouched.
    assert(armored.equipment().get(EquipmentSlot::Offhand).empty());

    std::cout << "testEndToEndIronReducesMeleeDamage OK\n";
}

// Full diamond reduces more than iron, especially against a bigger hit —
// the toughness term earning its keep end to end, not just in the pure
// function.
void testEndToEndDiamondReducesBigHitsMoreThanIron() {
    GameSession diamondSession;
    diamondSession.setGameMode(GameMode::Survival);
    diamondSession.equipment().set(EquipmentSlot::Head, armorStack(&items::DiamondHelmet));
    diamondSession.equipment().set(EquipmentSlot::Chest, armorStack(&items::DiamondChestplate));
    diamondSession.equipment().set(EquipmentSlot::Legs, armorStack(&items::DiamondLeggings));
    diamondSession.equipment().set(EquipmentSlot::Feet, armorStack(&items::DiamondBoots));
    TestHost diamondHost;
    assert(diamondSession.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 20.0F,
                                     diamondHost, true));
    // Full diamond is 20 armor / 8 toughness: damageAfterArmor(20, 20, 8) = 8.0.
    assert(nearly(diamondSession.vitals().health(), 12.0F));  // 20 - 8

    GameSession ironSession;
    ironSession.setGameMode(GameMode::Survival);
    ironSession.equipment().set(EquipmentSlot::Head, armorStack(&items::IronHelmet));
    ironSession.equipment().set(EquipmentSlot::Chest, armorStack(&items::IronChestplate));
    ironSession.equipment().set(EquipmentSlot::Legs, armorStack(&items::IronLeggings));
    ironSession.equipment().set(EquipmentSlot::Feet, armorStack(&items::IronBoots));
    TestHost ironHost;
    assert(ironSession.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 20.0F, ironHost,
                                  true));
    // Full iron (15 armor, 0 toughness): damageAfterArmor(20, 15, 0) = 20 *
    // (1 - clamp(15-10,3,20)/25) = 20*(1-5/25) = 16.0.
    assert(nearly(ironSession.vitals().health(), 4.0F));  // 20 - 16

    // Diamond's post-hit health is strictly higher: it absorbed more.
    assert(diamondSession.vitals().health() > ironSession.vitals().health());

    std::cout << "testEndToEndDiamondReducesBigHitsMoreThanIron OK\n";
}

// Sabotage② end-to-end: a BypassesArmor type (the void) ignores full diamond
// entirely — same as naked.
void testEndToEndBypassIgnoresArmor() {
    GameSession session;
    session.setGameMode(GameMode::Survival);
    session.equipment().set(EquipmentSlot::Head, armorStack(&items::DiamondHelmet));
    session.equipment().set(EquipmentSlot::Chest, armorStack(&items::DiamondChestplate));
    session.equipment().set(EquipmentSlot::Legs, armorStack(&items::DiamondLeggings));
    session.equipment().set(EquipmentSlot::Feet, armorStack(&items::DiamondBoots));
    TestHost host;
    assert(session.hurtPlayer(kPrimaryPlayerId, DamageType::OutOfWorld, 10.0F, host));
    // The full 10 damage lands, exactly as if unarmored.
    assert(nearly(session.vitals().health(), 10.0F));
    // Armor that never absorbed anything does not wear out either.
    for (const EquipmentSlot slot : kArmorSlots) {
        assert(session.equipment().get(slot).damage == 0U);
    }

    std::cout << "testEndToEndBypassIgnoresArmor OK\n";
}

// Sabotage③ end-to-end: durability accumulates hit over hit and the piece
// empties once it would exceed its maximum (mirrors Inventory::damageSelected's
// break-at-threshold rule, just applied per armor slot).
// Clears the post-hit invulnerability window headlessly (no World/tick(), no
// movement side effects) so a test can land a second hit immediately —
// PlayerVitals::tick decrements it every call regardless of the VitalsInput
// given, and a default-constructed one causes no fall/food/fire side effects.
void clearInvulnerabilityWindow(PlayerVitals& vitals) {
    for (int i = 0; i < kInvulnerableWindowTicks; ++i) {
        static_cast<void>(vitals.tick(VitalsInput{}));
    }
}

void testArmorBreaksAtZeroDurability() {
    GameSession session;
    session.setGameMode(GameMode::Survival);
    // Leather boots: BASE_DURABILITY[feet]=13 * multiplier[leather]=5 -> 65.
    session.equipment().set(EquipmentSlot::Feet, armorStack(&items::LeatherBoots));
    assert(itemMaximumDamage(session.equipment().get(EquipmentSlot::Feet)) == 65U);

    TestHost host;
    // Every landed non-bypassed hit costs max(1, preArmorDamage/4). A 12
    // damage hit costs floor(12/4) = 3 durability; the player is healed back
    // to full between hits (a fresh PlayerVitals::heal call, orthogonal to
    // the pipeline under test) purely so repeated hits keep landing rather
    // than being cut short by death — durability spend does not depend on
    // the player surviving, only on the hit having landed and reached the
    // armor stage. The invulnerability window is cleared the same way.
    for (int hit = 0; hit < 21; ++hit) {
        assert(session.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 12.0F, host, true));
        clearInvulnerabilityWindow(session.vitals());
        session.vitals().heal(20.0F);
    }
    // 21 hits * 3 durability = 63 <= 65: still intact.
    assert(session.equipment().get(EquipmentSlot::Feet).damage == 63U);
    assert(!session.equipment().get(EquipmentSlot::Feet).empty());

    // One more hit: 63 + 3 = 66 > 65 -- the boots break and the slot empties,
    // same rule Inventory::damageSelected uses for a worn-out tool.
    assert(session.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 12.0F, host, true));
    assert(session.equipment().get(EquipmentSlot::Feet).empty());

    std::cout << "testArmorBreaksAtZeroDurability OK\n";
}

// A minimal hit still spends at least one point of durability (the
// max(1, amount/4) floor), matching vanilla's "at least one point" rule.
void testArmorDurabilityFloorIsOnePoint() {
    GameSession session;
    session.setGameMode(GameMode::Survival);
    session.equipment().set(EquipmentSlot::Feet, armorStack(&items::IronBoots));
    TestHost host;
    // A 1-damage hit: preArmorDamage/4 = 0.25, floored under the max(1, ...)
    // rule to exactly 1.
    assert(session.hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, 1.0F, host, true));
    assert(session.equipment().get(EquipmentSlot::Feet).damage == 1U);

    std::cout << "testArmorDurabilityFloorIsOnePoint OK\n";
}

}  // namespace

int main() {
    testDamageAfterArmorWorkedExamples();
    testDamageAfterArmorClampFloor();
    testPipelineRespectsBypassesArmor();
    testPipelineAppliesArmorWhenNotBypassed();
    testPipelineStageOrderMatchesDifficultyThenArmor();
    testEndToEndIronReducesMeleeDamage();
    testEndToEndDiamondReducesBigHitsMoreThanIron();
    testEndToEndBypassIgnoresArmor();
    testArmorBreaksAtZeroDurability();
    testArmorDurabilityFloorIsOnePoint();
    std::cout << "armor_mitigation_test: all tests passed\n";
    return 0;
}
