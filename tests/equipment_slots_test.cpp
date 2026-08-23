// EQ-0: the equipment slot system's storage layer — the five fixed slots
// (armor x4 + offhand) on EquipmentSlots, their persistence through the PLYR
// save block (version 4), backward compatibility with pre-EQ-0 saves, and the
// net/snapshot wire codec round trip. No armor items, no damage-reduction
// formula, no equip interaction/HUD/rendering — those are later EQ nodes;
// this file covers exactly the storage + query seam EQ-0 built.

#include "gameplay/Enchantment.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/StreamCodec.hpp"
#include "persistence/SaveRepository.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <type_traits>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

[[nodiscard]] constexpr std::uint32_t fourCC(const char* text) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

// Builds a world.dat tail by hand so the loader can be pointed at a PLYR
// block written the way a pre-EQ-0 build would have (mirrors
// save_repository_test.cpp's LegacyWriter).
struct LegacyWriter final {
    std::vector<std::uint8_t> bytes;

    template <typename Value>
    void integer(Value value) {
        using Unsigned = std::make_unsigned_t<Value>;
        const auto converted = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Value); ++index) {
            bytes.push_back(static_cast<std::uint8_t>(converted >> (index * 8U)));
        }
    }

    void floating(float value) { integer(std::bit_cast<std::uint32_t>(value)); }

    template <typename WriteBody>
    void block(std::uint32_t tag, std::uint16_t version, WriteBody writeBody) {
        const std::size_t start = bytes.size();
        integer<std::uint32_t>(tag);
        integer<std::uint32_t>(0U);  // size placeholder
        integer<std::uint16_t>(version);
        writeBody();
        const auto size = static_cast<std::uint32_t>(bytes.size() - start);
        for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
            bytes[start + sizeof(std::uint32_t) + offset] =
                static_cast<std::uint8_t>(size >> (offset * 8U));
        }
    }

    void finish() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        integer<std::uint64_t>(hash);
    }
};

void testTriviallyCopyable() {
    static_assert(std::is_trivially_copyable_v<EquipmentSlots>);
    static_assert(std::is_trivially_copyable_v<ItemStack>);
    std::cout << "testTriviallyCopyable OK\n";
}

// --- Sabotage③ target: get/set/equippedArmor must address the exact slot
// asked for, not an off-by-one neighbour. ---
void testSetGetEachSlotIndependently() {
    EquipmentSlots equipment;
    const ItemStack head{world::Block::Air, 1U, &items::IronPickaxe};
    const ItemStack chest{world::Block::Air, 1U, &items::DiamondSword};
    const ItemStack legs{world::Block::Air, 1U, &items::Apple};
    const ItemStack feet{world::Block::Air, 1U, &items::Bucket};
    const ItemStack offhand{world::Block::Stone, 3U};

    equipment.set(EquipmentSlot::Head, head);
    equipment.set(EquipmentSlot::Chest, chest);
    equipment.set(EquipmentSlot::Legs, legs);
    equipment.set(EquipmentSlot::Feet, feet);
    equipment.set(EquipmentSlot::Offhand, offhand);

    // Every slot returns exactly what was put into IT, not a neighbour.
    assert(equipment.get(EquipmentSlot::Head) == head);
    assert(equipment.get(EquipmentSlot::Chest) == chest);
    assert(equipment.get(EquipmentSlot::Legs) == legs);
    assert(equipment.get(EquipmentSlot::Feet) == feet);
    assert(equipment.get(EquipmentSlot::Offhand) == offhand);

    // equippedArmor is the same read, exercised on all four armor slots.
    assert(equipment.equippedArmor(EquipmentSlot::Head) == head);
    assert(equipment.equippedArmor(EquipmentSlot::Chest) == chest);
    assert(equipment.equippedArmor(EquipmentSlot::Legs) == legs);
    assert(equipment.equippedArmor(EquipmentSlot::Feet) == feet);

    // Cross-check every pair is distinguishable — catches a slot alias/
    // off-by-one that happens to read back a *different* wrong slot too.
    assert(!(equipment.get(EquipmentSlot::Head) == equipment.get(EquipmentSlot::Chest)));
    assert(!(equipment.get(EquipmentSlot::Chest) == equipment.get(EquipmentSlot::Legs)));
    assert(!(equipment.get(EquipmentSlot::Legs) == equipment.get(EquipmentSlot::Feet)));
    assert(!(equipment.get(EquipmentSlot::Feet) == equipment.get(EquipmentSlot::Offhand)));
    std::cout << "testSetGetEachSlotIndependently OK\n";
}

void testDefaultEmpty() {
    EquipmentSlots equipment;
    for (std::size_t i = 0; i < kEquipmentSlotCount; ++i) {
        assert(equipment.get(static_cast<EquipmentSlot>(i)).empty());
    }
    std::cout << "testDefaultEmpty OK\n";
}

void testArmorSlotOrderAndMembership() {
    // kArmorSlots names exactly the four armor slots (not offhand), in
    // feet->head order.
    assert(kArmorSlots.size() == 4U);
    assert(kArmorSlots[0] == EquipmentSlot::Feet);
    assert(kArmorSlots[1] == EquipmentSlot::Legs);
    assert(kArmorSlots[2] == EquipmentSlot::Chest);
    assert(kArmorSlots[3] == EquipmentSlot::Head);
    assert(isArmorSlot(EquipmentSlot::Head));
    assert(isArmorSlot(EquipmentSlot::Chest));
    assert(isArmorSlot(EquipmentSlot::Legs));
    assert(isArmorSlot(EquipmentSlot::Feet));
    assert(!isArmorSlot(EquipmentSlot::Offhand));
    std::cout << "testArmorSlotOrderAndMembership OK\n";
}

// --- Net/snapshot wire codec round trip (StreamCodec.hpp), the vehicle
// GameSnapshotCodec.cpp's WorldSnapshot equipmentSlots array uses. ---
void testNetCodecRoundTrip() {
    EquipmentSlots equipment;
    ItemStack chest{world::Block::Air, 1U, &items::DiamondPickaxe};
    setEnchantmentLevel(chest, EnchantmentId::Unbreaking, 3U);
    equipment.set(EquipmentSlot::Chest, chest);
    equipment.set(EquipmentSlot::Feet, ItemStack{world::Block::Stone, 1U});
    // Head/Legs/Offhand left empty on purpose — the codec must round-trip a
    // mixed empty/occupied slot set correctly.

    std::vector<std::uint8_t> bytes;
    for (const auto& stack : equipment.slots()) {
        codec::appendItemStack(bytes, stack);
    }
    std::size_t cursor = 0;
    std::array<ItemStack, kEquipmentSlotCount> decoded{};
    for (auto& stack : decoded) {
        const auto value = codec::readItemStack(bytes, cursor);
        assert(value.has_value());
        stack = *value;
    }
    assert(cursor == bytes.size());

    EquipmentSlots restored;
    restored.restore(decoded);
    assert(restored.get(EquipmentSlot::Chest) == chest);
    assert(enchantmentLevel(restored.get(EquipmentSlot::Chest), EnchantmentId::Unbreaking) == 3U);
    assert(restored.get(EquipmentSlot::Feet).block == world::Block::Stone);
    assert(restored.get(EquipmentSlot::Head).empty());
    assert(restored.get(EquipmentSlot::Legs).empty());
    assert(restored.get(EquipmentSlot::Offhand).empty());
    std::cout << "testNetCodecRoundTrip OK\n";
}

// --- Sabotage① target: save -> load must preserve every equipped slot. ---
void testSaveLoadRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() / "eq0_save_roundtrip_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    persistence::SaveRepository repository{root};

    auto game = repository.create("EquipRoundTrip", 42ULL);
    game.equipment[static_cast<std::size_t>(EquipmentSlot::Head)] =
        ItemStack{world::Block::Air, 1U, &items::IronPickaxe};
    game.equipment[static_cast<std::size_t>(EquipmentSlot::Chest)] =
        ItemStack{world::Block::Air, 1U, &items::DiamondSword};
    game.equipment[static_cast<std::size_t>(EquipmentSlot::Legs)] =
        ItemStack{world::Block::Air, 1U, &items::Apple};
    game.equipment[static_cast<std::size_t>(EquipmentSlot::Feet)] =
        ItemStack{world::Block::Air, 1U, &items::Bucket};
    ItemStack offhand{world::Block::Air, 1U, &items::DiamondPickaxe};
    setEnchantmentLevel(offhand, EnchantmentId::Efficiency, 5U);
    game.equipment[static_cast<std::size_t>(EquipmentSlot::Offhand)] = offhand;

    repository.save(game);
    const auto loaded = repository.load(game.summary.identifier);

    assert(loaded.equipment == game.equipment);
    assert(loaded.equipment[static_cast<std::size_t>(EquipmentSlot::Head)].item ==
           &items::IronPickaxe);
    assert(loaded.equipment[static_cast<std::size_t>(EquipmentSlot::Chest)].item ==
           &items::DiamondSword);
    assert(loaded.equipment[static_cast<std::size_t>(EquipmentSlot::Legs)].item == &items::Apple);
    assert(loaded.equipment[static_cast<std::size_t>(EquipmentSlot::Feet)].item == &items::Bucket);
    assert(enchantmentLevel(loaded.equipment[static_cast<std::size_t>(EquipmentSlot::Offhand)],
                            EnchantmentId::Efficiency) == 5U);

    std::filesystem::remove_all(root);
    std::cout << "testSaveLoadRoundTrip OK\n";
}

// An empty-equipment player round-trips too — the common case (armor items
// do not exist through ordinary play yet), must not desync or crash.
void testSaveLoadRoundTripEmpty() {
    const auto root = std::filesystem::temp_directory_path() / "eq0_save_roundtrip_empty_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    persistence::SaveRepository repository{root};

    auto game = repository.create("EquipRoundTripEmpty", 43ULL);
    repository.save(game);
    const auto loaded = repository.load(game.summary.identifier);
    for (const auto& stack : loaded.equipment) {
        assert(stack.empty());
    }

    std::filesystem::remove_all(root);
    std::cout << "testSaveLoadRoundTripEmpty OK\n";
}

// --- Sabotage② target: a hand-built PLYR block at a pre-EQ-0 version (3)
// must load back with all five equipment slots empty — no crash, no stream
// misalignment reading a tail that was never written. Mirrors
// save_repository_test.cpp's XP-0/ENCH-0 legacy PLYR tests, one version
// bump later. ---
void testLegacyPreEq0SaveLoadsEmptyEquipment() {
    const auto root = std::filesystem::temp_directory_path() / "eq0_legacy_player_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    persistence::SaveRepository repository{root};

    auto game = repository.create("EquipLegacyPlayer", 44ULL);
    game.playerExperienceLevel = 4;
    game.playerExperiencePoints = 2;
    game.playerTotalExperience = 30;
    game.playerEnchantmentSeed = 555;
    repository.save(game);

    const auto path = repository.root() / game.summary.identifier / "world.dat";
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        assert(input);
        bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    // Drop the trailing checksum, append a version-3 PLYR block (position +
    // hotbar + vitals + an empty inventory with the ENCH-0 tail + experience,
    // but NO equipment tail — exactly what a pre-EQ-0 build wrote), re-checksum.
    bytes.resize(bytes.size() - sizeof(std::uint64_t));

    LegacyWriter writer;
    writer.bytes = std::move(bytes);
    writer.block(fourCC("PLYR"), 3U, [&] {
        writer.integer<std::uint8_t>(0U);  // hasPlayerPosition = false
        writer.floating(0.0F);
        writer.floating(0.0F);
        writer.floating(0.0F);
        writer.integer<std::uint8_t>(0U);   // selectedHotbarSlot
        writer.floating(20.0F);             // playerHealth
        writer.integer<std::int32_t>(20);   // playerFoodLevel
        writer.floating(5.0F);              // playerSaturation
        writer.integer<std::int32_t>(300);  // playerAirTicks
        // appendSlots' sparse format: a zero occupied-count closes the
        // inventory list (version-3 stacks have the ENCH-0 tail, but an
        // empty list never reads any per-stack bytes either way).
        writer.integer<std::uint16_t>(0U);
        // Version 2+'s experience fields, present at version 3.
        writer.integer<std::int32_t>(9);    // playerExperienceLevel
        writer.integer<std::int32_t>(3);    // playerExperiencePoints
        writer.integer<std::int32_t>(200);  // playerTotalExperience
        writer.integer<std::int32_t>(-42);  // playerEnchantmentSeed
        // No equipment tail here — this IS the version-3 (pre-EQ-0) shape.
    });
    writer.finish();
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(writer.bytes.data()),
                     static_cast<std::streamsize>(writer.bytes.size()));
    }

    const auto loaded = repository.load(game.summary.identifier);
    // The version-3 fields still read correctly — proves the reader did NOT
    // try to consume a nonexistent equipment tail and desync.
    assert(loaded.playerHealth == 20.0F);
    assert(loaded.playerFoodLevel == 20);
    assert(loaded.playerAirTicks == 300);
    assert(loaded.playerExperienceLevel == 9);
    assert(loaded.playerExperiencePoints == 3);
    assert(loaded.playerTotalExperience == 200);
    assert(loaded.playerEnchantmentSeed == -42);
    // Every equipment slot — absent from this version-3 block entirely —
    // reads back empty: "no equipment ever existed to lose", not a crash,
    // not a misaligned read of neighbouring bytes.
    for (const auto& stack : loaded.equipment) {
        assert(stack.empty());
    }

    std::filesystem::remove_all(root);
    std::cout << "testLegacyPreEq0SaveLoadsEmptyEquipment OK\n";
}

}  // namespace

int main() {
    testTriviallyCopyable();
    testSetGetEachSlotIndependently();
    testDefaultEmpty();
    testArmorSlotOrderAndMembership();
    testNetCodecRoundTrip();
    testSaveLoadRoundTrip();
    testSaveLoadRoundTripEmpty();
    testLegacyPreEq0SaveLoadsEmptyEquipment();
    std::cout << "equipment_slots_test: all tests passed\n";
    return 0;
}
