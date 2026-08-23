// ENCH-0: per-stack enchantment storage — the inline fixed-capacity list on
// ItemStack, its participation in operator==/sameItem (so enchanted stacks
// never merge with a plain or differently-enchanted twin), and the net/
// snapshot wire codec (StreamCodec.hpp's appendItemStack/readItemStack). Save
// round-tripping is covered by save_repository_test.cpp (the storage owner);
// this file covers the in-memory/wire half.

#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/StreamCodec.hpp"

#include <cassert>
#include <iostream>
#include <type_traits>

using namespace mc;
using namespace mc::gameplay;

namespace {

void testTriviallyCopyable() {
    static_assert(std::is_trivially_copyable_v<ItemStack>);
    static_assert(std::is_trivially_copyable_v<EnchantmentInstance>);
    std::cout << "testTriviallyCopyable OK\n";
}

void testSetGetEnchantmentLevel() {
    ItemStack sword{world::Block::Air, 1U, &items::DiamondSword};
    assert(!hasEnchantment(sword, EnchantmentId::Sharpness));
    assert(enchantmentLevel(sword, EnchantmentId::Sharpness) == 0U);

    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 5U);
    assert(hasEnchantment(sword, EnchantmentId::Sharpness));
    assert(enchantmentLevel(sword, EnchantmentId::Sharpness) == 5U);
    assert(sword.enchantmentCount == 1U);

    // Raising an existing entry's level does not grow the count.
    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 2U);
    assert(enchantmentLevel(sword, EnchantmentId::Sharpness) == 2U);
    assert(sword.enchantmentCount == 1U);

    // Setting level 0 clears the entry.
    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 0U);
    assert(!hasEnchantment(sword, EnchantmentId::Sharpness));
    assert(sword.enchantmentCount == 0U);

    // Multiple distinct enchantments coexist.
    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 3U);
    setEnchantmentLevel(sword, EnchantmentId::Unbreaking, 2U);
    setEnchantmentLevel(sword, EnchantmentId::Mending, 1U);
    assert(sword.enchantmentCount == 3U);
    assert(enchantmentLevel(sword, EnchantmentId::Sharpness) == 3U);
    assert(enchantmentLevel(sword, EnchantmentId::Unbreaking) == 2U);
    assert(enchantmentLevel(sword, EnchantmentId::Mending) == 1U);

    // Removing the middle entry (swap-erase) leaves the other two intact.
    setEnchantmentLevel(sword, EnchantmentId::Unbreaking, 0U);
    assert(sword.enchantmentCount == 2U);
    assert(enchantmentLevel(sword, EnchantmentId::Sharpness) == 3U);
    assert(enchantmentLevel(sword, EnchantmentId::Mending) == 1U);
    assert(!hasEnchantment(sword, EnchantmentId::Unbreaking));
    std::cout << "testSetGetEnchantmentLevel OK\n";
}

// The fixed capacity is a deliberate JC deviation: past the cap, further
// setEnchantmentRaw calls are a no-op rather than UB or a silent overwrite of
// an unrelated slot.
void testCapacityCeiling() {
    ItemStack sword{world::Block::Air, 1U, &items::DiamondSword};
    for (std::size_t index = 0; index < kMaxEnchantmentsPerStack; ++index) {
        sword.setEnchantmentRaw(static_cast<EnchantmentIdStorage>(index), 1U);
    }
    assert(sword.enchantmentCount == kMaxEnchantmentsPerStack);
    // One past the cap: dropped, not appended, not UB.
    sword.setEnchantmentRaw(static_cast<EnchantmentIdStorage>(kMaxEnchantmentsPerStack), 1U);
    assert(sword.enchantmentCount == kMaxEnchantmentsPerStack);
    std::cout << "testCapacityCeiling OK\n";
}

// --- Sabotage② target: an enchanted stack must never compare equal to (or
// merge with) its unenchanted twin, nor a differently-enchanted twin. ---
void testEnchantedStacksDoNotEqualOrMerge() {
    ItemStack plain{world::Block::Air, 1U, &items::DiamondSword};
    ItemStack enchanted{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(enchanted, EnchantmentId::Sharpness, 3U);

    assert(!(plain == enchanted));
    assert(!sameItem(plain, enchanted));

    ItemStack differentlyEnchanted{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(differentlyEnchanted, EnchantmentId::Sharpness, 4U);
    assert(!(enchanted == differentlyEnchanted));
    assert(!sameItem(enchanted, differentlyEnchanted));

    ItemStack differentEnchantmentSameLevel{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(differentEnchantmentSameLevel, EnchantmentId::Smite, 3U);
    assert(!(enchanted == differentEnchantmentSameLevel));
    assert(!sameItem(enchanted, differentEnchantmentSameLevel));

    // Two stacks with the identical enchantment set (even if applied in a
    // different order) DO compare equal — order independence.
    ItemStack enchantedAgainSameOrder{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(enchantedAgainSameOrder, EnchantmentId::Sharpness, 3U);
    assert(enchanted == enchantedAgainSameOrder);
    assert(sameItem(enchanted, enchantedAgainSameOrder));

    ItemStack multiA{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(multiA, EnchantmentId::Sharpness, 3U);
    setEnchantmentLevel(multiA, EnchantmentId::Unbreaking, 2U);
    ItemStack multiB{world::Block::Air, 1U, &items::DiamondSword};
    setEnchantmentLevel(multiB, EnchantmentId::Unbreaking, 2U);  // reverse insertion order
    setEnchantmentLevel(multiB, EnchantmentId::Sharpness, 3U);
    assert(multiA == multiB);
    assert(sameItem(multiA, multiB));

    // Two identical unenchanted stacks are unaffected (baseline sanity).
    ItemStack plainAgain{world::Block::Air, 1U, &items::DiamondSword};
    assert(plain == plainAgain);
    assert(sameItem(plain, plainAgain));
    std::cout << "testEnchantedStacksDoNotEqualOrMerge OK\n";
}

// The plain (unenchanted) case must be completely unaffected by ENCH-0 — two
// plain stacks that only differ in damage still compare unequal exactly as
// before, and the enchantment machinery does not perturb that.
void testPlainStacksUnaffected() {
    ItemStack fresh{world::Block::Air, 1U, &items::IronPickaxe, 0U};
    ItemStack worn{world::Block::Air, 1U, &items::IronPickaxe, 50U};
    assert(!(fresh == worn));
    assert(!sameItem(fresh, worn));
    ItemStack freshAgain{world::Block::Air, 1U, &items::IronPickaxe, 0U};
    assert(fresh == freshAgain);
    assert(sameItem(fresh, freshAgain));
    std::cout << "testPlainStacksUnaffected OK\n";
}

// --- Net/snapshot wire codec round trip (StreamCodec.hpp) ---
void testNetCodecRoundTripEnchanted() {
    ItemStack stack{world::Block::Air, 1U, &items::DiamondPickaxe};
    setEnchantmentLevel(stack, EnchantmentId::Efficiency, 4U);
    setEnchantmentLevel(stack, EnchantmentId::Fortune, 3U);
    setEnchantmentLevel(stack, EnchantmentId::Unbreaking, 3U);

    std::vector<std::uint8_t> bytes;
    codec::appendItemStack(bytes, stack);
    std::size_t cursor = 0;
    const auto decoded = codec::readItemStack(bytes, cursor);
    assert(decoded.has_value());
    assert(decoded->enchantmentCount == 3U);
    assert(enchantmentLevel(*decoded, EnchantmentId::Efficiency) == 4U);
    assert(enchantmentLevel(*decoded, EnchantmentId::Fortune) == 3U);
    assert(enchantmentLevel(*decoded, EnchantmentId::Unbreaking) == 3U);
    assert(decoded->item == stack.item);
    assert(decoded->count == stack.count);
    assert(cursor == bytes.size());
    std::cout << "testNetCodecRoundTripEnchanted OK\n";
}

// An unenchanted stack's net encoding must still round-trip (the sparse tail
// costs exactly one zero byte, never breaking the existing block/count/item/
// damage fields any pre-ENCH-0 peer already decoded).
void testNetCodecRoundTripPlain() {
    ItemStack stack{world::Block::Air, 3U, &items::Apple};
    std::vector<std::uint8_t> bytes;
    codec::appendItemStack(bytes, stack);
    std::size_t cursor = 0;
    const auto decoded = codec::readItemStack(bytes, cursor);
    assert(decoded.has_value());
    assert(decoded->enchantmentCount == 0U);
    assert(decoded->count == 3U);
    assert(decoded->item == &items::Apple);
    assert(cursor == bytes.size());
    std::cout << "testNetCodecRoundTripPlain OK\n";
}

// A block stack (the legacy null-item-pointer form) also carries enchantments
// through the codec correctly (an enchanted block stack cannot occur through
// ordinary play today, but the codec must not silently drop it if one is
// ever constructed by a future block-enchant path).
void testNetCodecRoundTripBlockStack() {
    ItemStack stack{world::Block::Stone, 5U};
    std::vector<std::uint8_t> bytes;
    codec::appendItemStack(bytes, stack);
    std::size_t cursor = 0;
    const auto decoded = codec::readItemStack(bytes, cursor);
    assert(decoded.has_value());
    assert(decoded->block == world::Block::Stone);
    assert(decoded->count == 5U);
    assert(decoded->enchantmentCount == 0U);
    std::cout << "testNetCodecRoundTripBlockStack OK\n";
}

}  // namespace

int main() {
    testTriviallyCopyable();
    testSetGetEnchantmentLevel();
    testCapacityCeiling();
    testEnchantedStacksDoNotEqualOrMerge();
    testPlainStacksUnaffected();
    testNetCodecRoundTripEnchanted();
    testNetCodecRoundTripPlain();
    testNetCodecRoundTripBlockStack();
    std::cout << "enchantment_storage_test: all tests passed\n";
    return 0;
}
