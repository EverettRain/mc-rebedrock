#include "persistence/SaveRepository.hpp"

#include "world/DayNightCycle.hpp"
#include "world/WorldClock.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// Builds a world.dat by hand so the loader can be pointed at a payload written
// the way an older build wrote it.
struct LegacyWriter final {
    std::vector<std::uint8_t> bytes;

    void magic() {
        for (const char character : std::string_view{"MCRBSAVE"}) {
            bytes.push_back(static_cast<std::uint8_t>(character));
        }
    }

    template <typename Value>
    void integer(Value value) {
        using Unsigned = std::make_unsigned_t<Value>;
        const auto converted = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Value); ++index) {
            bytes.push_back(static_cast<std::uint8_t>(converted >> (index * 8U)));
        }
    }

    void floating(float value) { integer(std::bit_cast<std::uint32_t>(value)); }
    void doubleValue(double value) { integer(std::bit_cast<std::uint64_t>(value)); }

    void stringValue(std::string_view text) {
        integer<std::uint16_t>(static_cast<std::uint16_t>(text.size()));
        for (const char character : text) {
            bytes.push_back(static_cast<std::uint8_t>(character));
        }
    }

    void finish() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        integer(hash);
    }
};

} // namespace

int main() {
    using namespace mc;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mc-rebedrock-save-test-" + std::to_string(unique));
    persistence::SaveRepository repository{root};
    auto save = repository.create("  Test=World  ", 0x12345678ULL);
    assert(save.summary.displayName == "TestWorld");
    save.hasPlayerPosition = true;
    save.playerX = -12.5F;
    save.playerY = 64.0F;
    save.playerZ = 7.25F;
    // The /spawnpoint result rides in its own format-10 block.
    save.hasSpawnPoint = true;
    save.spawnX = -30.5F;
    save.spawnY = 66.0F;
    save.spawnZ = 12.0F;
    save.spawnYaw = 0.0F;
    save.gameTimeSeconds = 42.5;
    save.gameMode = gameplay::GameMode::Survival;
    save.difficulty = gameplay::Difficulty::Hard;
    static_cast<void>(
        save.gameRules.set<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed, 7));
    gameplay::Inventory inventory;
    inventory.selectHotbar(4U);
    save.inventory = inventory.slots();
    // A half-worn tool has to come back with the same damage on it.
    save.inventory[4] = {world::Block::Air, 1U, &gameplay::items::IronPickaxe, 137U};
    save.selectedHotbarSlot = inventory.selectedHotbarSlot();
    save.edits = {
        // A burning furnace is the same block with LIT set; format 14 carries
        // the flag, so a world saved mid-smelt reopens still alight instead of
        // silently going cold.
        {-1, 63, 2, world::Block::Furnace, 0U, world::BlockOrientation::East, true},
        {4, 62, -8, world::Block::Water, 3U, world::BlockOrientation::North},
        // Farmland carries its moisture (0-7) in the per-cell orientation byte
        // (farmlandMoisture), so the saved state can exceed the six enumerated
        // BlockOrientation facings. Regression: a save with well-watered
        // farmland used to fail to load as an "invalid block edit".
        {5, 62, -8, world::Block::Farmland, 0U, static_cast<world::BlockOrientation>(7)},
    };
    gameplay::ChestBlockEntity chest;
    chest.position = {8, 65, -4};
    chest.items[0] = {world::Block::Chest, 1U};
    chest.items[8] = {world::Block::Air, 3U, &gameplay::items::Book};
    save.chests.push_back(chest);
    // Format 15's furnace block entities: each furnace's three slots and its
    // burn/cook counters travel with the world, so a furnace reopens holding
    // what it held and resumes the smelt it was partway through.
    gameplay::FurnaceBlockEntity furnace;
    furnace.position = {8, 65, -6};
    furnace.input = {world::Block::IronOre, 2U, gameplay::blockItemFor(world::Block::IronOre)};
    furnace.fuel = {world::Block::Air, 1U, &gameplay::items::Coal};
    furnace.output = {world::Block::Air, 3U, &gameplay::items::IronIngot};
    furnace.burnTicks = 640;
    furnace.initialBurnTicks = 1600;
    furnace.cookTicks = 75;
    furnace.cookDurationTicks = 200;
    save.furnaces.push_back(furnace);
    // Format 12's ENTITY block: creatures travel by species name and come back
    // with their pose and state intact.
    save.entities = {
        {"pig", 10.0F, 64.0F, 8.0F, 0.5F, 0.0F, 0.0F, 0.0F, 10.0F, 0, 120U, 0x1234U},
        {"zombie", 20.0F, 64.0F, -5.0F, 1.2F, 0.1F, 0.0F, 0.0F, 20.0F, 40, 5U, 0xABCDU},
    };
    repository.save(save);
    const auto listed = repository.list();
    assert(listed.size() == 1U);
    assert(listed.front().identifier == save.summary.identifier);
    const auto loaded = repository.load(save.summary.identifier);
    assert(loaded.summary.displayName == "TestWorld");
    assert(loaded.summary.seed == 0x12345678ULL);
    assert(loaded.hasPlayerPosition && loaded.playerX == -12.5F);
    assert(loaded.hasSpawnPoint && loaded.spawnX == -30.5F && loaded.spawnY == 66.0F &&
           loaded.spawnZ == 12.0F);
    assert(loaded.gameMode == gameplay::GameMode::Survival);
    assert(loaded.difficulty == gameplay::Difficulty::Hard);
    assert(loaded.gameRules.get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed) == 7);
    assert(loaded.selectedHotbarSlot == 4U);
    assert(loaded.inventory == save.inventory);
    assert(loaded.inventory[4].damage == 137U);
    assert(loaded.edits == save.edits);
    assert(loaded.edits[0].lit);
    // The palette names the block, not the state: `lit_furnace` is gone.
    {
        std::ifstream data{root / save.summary.identifier / "world.dat", std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{data},
                                std::istreambuf_iterator<char>{}};
        assert(bytes.find("lit_furnace") == std::string::npos);
    }
    assert(loaded.chests.size() == 1U);
    assert(loaded.chests.front().position == chest.position);
    assert(loaded.chests.front().items == chest.items);
    // The furnace block entity survives the round trip whole: contents and the
    // burn/cook counters that let it resume its smelt.
    assert(loaded.furnaces.size() == 1U);
    {
        const auto& reloaded = loaded.furnaces.front();
        assert(reloaded.position == furnace.position);
        assert(reloaded.input.block == world::Block::IronOre && reloaded.input.count == 2U);
        assert(reloaded.fuel.item == &gameplay::items::Coal);
        assert(reloaded.output.item == &gameplay::items::IronIngot && reloaded.output.count == 3U);
        assert(reloaded.burnTicks == 640 && reloaded.initialBurnTicks == 1600);
        assert(reloaded.cookTicks == 75 && reloaded.cookDurationTicks == 200);
    }
    // The ENTITY block round-trips the herd, species resolved by name later.
    assert(loaded.entities.size() == 2U);
    assert(loaded.entities[0].species == "pig");
    assert(loaded.entities[0].x == 10.0F && loaded.entities[0].y == 64.0F &&
           loaded.entities[0].z == 8.0F);
    assert(loaded.entities[0].yaw == 0.5F);
    assert(loaded.entities[0].health == 10.0F);
    assert(loaded.entities[0].ageTicks == 120U);
    assert(loaded.entities[0].rngState == 0x1234U);
    assert(loaded.entities[1].species == "zombie");
    assert(loaded.entities[1].angerTicks == 40);
    assert(loaded.entities[1].rngState == 0xABCDU);

    // Blocks travel as namespaced identifiers now, so the world.dat payload
    // literally contains them and a renumbered enum cannot silently reinterpret
    // an old save.
    {
        std::ifstream data{root / save.summary.identifier / "world.dat", std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{data},
                                std::istreambuf_iterator<char>{}};
        assert(bytes.find("rebedrock:furnace") != std::string::npos);
        assert(bytes.find("rebedrock:water") != std::string::npos);
        assert(bytes.find("rebedrock:chest") != std::string::npos);
        // Items travel the same way.
        assert(bytes.find("rebedrock:book") != std::string::npos);
        // Only the content the save mentions is written, not the whole registry.
        assert(bytes.find("rebedrock:granite") == std::string::npos);
        assert(bytes.find("rebedrock:feather") == std::string::npos);
        // The non-default randomTickSpeed travels as a named entry in the
        // GameRules block.
        assert(bytes.find("randomTickSpeed") != std::string::npos);
    }

    // Game rule storage is sparse: a brand-new world, whose rules are all at
    // their defaults, writes a GameRules block with zero entries and no rule
    // name appears anywhere in world.dat.
    {
        auto fresh = repository.create("Fresh", 1ULL);
        repository.save(fresh);
        std::ifstream data{root / fresh.summary.identifier / "world.dat", std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{data},
                                std::istreambuf_iterator<char>{}};
        assert(bytes.find("randomTickSpeed") == std::string::npos);
        assert(bytes.find("doDaylightCycle") == std::string::npos);
        assert(bytes.find("keepInventory") == std::string::npos);
        const auto reloaded = repository.load(fresh.summary.identifier);
        assert(reloaded.gameRules.get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed) == 3);
    }

    // A format-4 world.dat, written when blocks were raw enum ordinals, still
    // loads: the frozen legacy order maps those ordinals onto identifiers.
    {
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(4U);
        writer.integer<std::uint64_t>(0xABCDEFULL);
        writer.integer<std::uint8_t>(1U);
        writer.floating(1.0F);
        writer.floating(64.0F);
        writer.floating(2.0F);
        // 300 s of world time, so the format-13 backfill has something to
        // convert: the server tick from the elapsed seconds, the sun from the
        // same day-cycle conversion every read site used to redo by hand.
        writer.doubleValue(300.0);
        writer.integer<std::uint8_t>(static_cast<std::uint8_t>(gameplay::GameMode::Survival));
        writer.integer<std::uint8_t>(0U);
        writer.floating(gameplay::PlayerVitals::kMaximumHealth);
        writer.integer<std::int32_t>(gameplay::PlayerVitals::kMaximumFood);
        writer.floating(5.0F);
        writer.integer<std::int32_t>(gameplay::PlayerVitals::kMaximumAirTicks);
        for (std::size_t slot = 0; slot < gameplay::Inventory::kSlotCount; ++slot) {
            // Slot 0 holds ordinal 14 of the format-4 item order, a diamond
            // pickaxe; the rest are empty.
            writer.integer<std::uint8_t>(0U);  // air
            writer.integer<std::uint8_t>(slot == 0U ? 1U : 0U);
            writer.integer<std::uint8_t>(slot == 0U ? 14U : 0U);
        }
        writer.integer<std::uint64_t>(2U);
        // Ordinals 3 and 49 in the format-4 enum: stone and the north wall torch.
        for (const auto& [position, ordinal] :
             std::array<std::pair<std::array<std::int32_t, 3>, std::uint8_t>, 2>{
                 {{{0, 70, 0}, 3U}, {{1, 71, 2}, 49U}}}) {
            writer.integer<std::int32_t>(position[0]);
            writer.integer<std::int32_t>(position[1]);
            writer.integer<std::int32_t>(position[2]);
            writer.integer<std::uint8_t>(ordinal);
            writer.integer<std::uint8_t>(0U);
            writer.integer<std::uint8_t>(
                static_cast<std::uint8_t>(world::BlockOrientation::North));
        }
        writer.integer<std::uint64_t>(0U);
        writer.finish();

        const std::string legacyIdentifier = "legacy-world";
        const auto legacyDirectory = root / legacyIdentifier;
        std::filesystem::create_directories(legacyDirectory);
        {
            std::ofstream metadata{legacyDirectory / "level.properties"};
            metadata << "format=4\nid=" << legacyIdentifier
                     << "\nname=Legacy\nseed=11259375\nlast_played=1\n";
        }
        {
            std::ofstream data{legacyDirectory / "world.dat", std::ios::binary};
            data.write(reinterpret_cast<const char*>(writer.bytes.data()),
                       static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto legacy = repository.load(legacyIdentifier);
        assert(legacy.edits.size() == 2U);
        assert(legacy.edits[0].block == world::Block::Stone);
        // Ordinal 49 was `wall_torch_north`, which is no longer a block of its
        // own: format 14 turns it back into the one wall torch plus a facing.
        // Losing that mapping would silently rotate every wall torch in an old
        // world to the default north face.
        assert(legacy.edits[1].block == world::Block::WallTorch);
        assert(legacy.edits[1].orientation == world::BlockOrientation::North);
        assert(!legacy.edits[1].lit);
        assert(legacy.inventory[0] ==
               (gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::DiamondPickaxe}));
        assert(legacy.gameMode == gameplay::GameMode::Survival);
        // Format 13 split the single gameTimeSeconds into a world tick and the
        // named clocks; a save older than that seeds both from it rather than
        // reopening at tick zero.
        assert(legacy.serverTick == 6'000U);  // 300 s x 20 TPS
        assert(legacy.clocks[static_cast<std::size_t>(world::ClockId::Overworld)].totalTicks ==
               static_cast<std::uint64_t>(world::DayNightCycle::worldTick(300.0)));
        std::filesystem::remove_all(legacyDirectory);
    }

    // The clock block round-trips: a world saved with the sun frozen partway
    // through the night reopens with the sun exactly there and still frozen,
    // while the world tick continues from where it stopped. Before format 13
    // both lived in one double and the pause state was not saved at all.
    {
        auto clocked = repository.create("Clocked", 4321U);
        clocked.serverTick = 123'456U;
        auto& overworld = clocked.clocks[static_cast<std::size_t>(world::ClockId::Overworld)];
        overworld.totalTicks = 17'250U;
        overworld.partialTick = 0.25F;
        overworld.rate = 0.5F;
        overworld.paused = true;
        repository.save(clocked);

        const auto reloaded = repository.load(clocked.summary.identifier);
        assert(reloaded.serverTick == 123'456U);
        const auto& restored =
            reloaded.clocks[static_cast<std::size_t>(world::ClockId::Overworld)];
        assert(restored.totalTicks == 17'250U);
        assert(restored.partialTick == 0.25F);
        assert(restored.rate == 0.5F);
        assert(restored.paused);
    }

    // A format-8 world.dat still carries randomTickSpeed at a fixed header
    // offset; format 9 moved the game rules into a trailing block, so the new
    // loader must migrate the old value into the rules registry and rewrite it
    // as a block on the next save.
    {
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(8U);
        writer.integer<std::uint64_t>(0xDEADBEEFULL);
        writer.integer<std::uint8_t>(1U);
        writer.floating(0.0F);
        writer.floating(64.0F);
        writer.floating(0.0F);
        writer.doubleValue(0.0);
        writer.integer<std::uint8_t>(static_cast<std::uint8_t>(gameplay::GameMode::Creative));
        writer.integer<std::uint8_t>(0U);
        writer.integer<std::uint8_t>(static_cast<std::uint8_t>(gameplay::Difficulty::Hard));
        writer.integer<std::int32_t>(7);  // the format-8 randomTickSpeed header field
        writer.floating(gameplay::PlayerVitals::kMaximumHealth);
        writer.integer<std::int32_t>(gameplay::PlayerVitals::kMaximumFood);
        writer.floating(5.0F);
        writer.integer<std::int32_t>(gameplay::PlayerVitals::kMaximumAirTicks);
        // Format 5+ resolves blocks through an identifier palette; this empty
        // world mentions only air.
        writer.integer<std::uint16_t>(1U);
        writer.stringValue("air");
        // Format 6+ does the same for items; the empty string is the block
        // sentinel, resolving back to no item.
        writer.integer<std::uint16_t>(1U);
        writer.stringValue("");
        for (std::size_t slot = 0; slot < gameplay::Inventory::kSlotCount; ++slot) {
            writer.integer<std::uint16_t>(0U);  // block palette index: air
            writer.integer<std::uint8_t>(0U);
            writer.integer<std::uint16_t>(0U);  // item palette index: empty
            writer.integer<std::uint16_t>(0U);  // damage (format 7+)
        }
        writer.integer<std::uint64_t>(0U);  // edits
        writer.integer<std::uint64_t>(0U);  // chests
        writer.finish();

        const std::string v8Identifier = "v8-world";
        const auto v8Directory = root / v8Identifier;
        std::filesystem::create_directories(v8Directory);
        {
            std::ofstream metadata{v8Directory / "level.properties"};
            metadata << "format=8\nid=" << v8Identifier
                     << "\nname=V8\nseed=42\nlast_played=1\n";
        }
        {
            std::ofstream data{v8Directory / "world.dat", std::ios::binary};
            data.write(reinterpret_cast<const char*>(writer.bytes.data()),
                       static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto migrated = repository.load(v8Identifier);
        assert(migrated.difficulty == gameplay::Difficulty::Hard);
        assert(migrated.gameRules.get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed) == 7);
        // A pre-format-10 save has no spawn point block; the field stays unset.
        assert(!migrated.hasSpawnPoint);
        // Saving the migrated world rewrites it as format 10, where the value
        // lives in the GameRules block and the absent spawn point is preserved.
        repository.save(migrated);
        const auto roundTrip = repository.load(v8Identifier);
        assert(roundTrip.gameRules.get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed) == 7);
        assert(!roundTrip.hasSpawnPoint);
        std::filesystem::remove_all(v8Directory);
    }

    bool rejectedTraversal = false;
    try {
        static_cast<void>(repository.load("../outside"));
    } catch (const std::invalid_argument&) {
        rejectedTraversal = true;
    }
    assert(rejectedTraversal);

    // A partial/corrupted payload must fail closed instead of loading a
    // plausible-looking but damaged world.
    const auto dataPath = root / save.summary.identifier / "world.dat";
    {
        std::fstream data{dataPath, std::ios::binary | std::ios::in | std::ios::out};
        assert(data);
        data.seekg(12);
        char byte = 0;
        data.read(&byte, 1);
        data.seekp(12);
        byte ^= 0x5A;
        data.write(&byte, 1);
    }
    bool rejectedCorruption = false;
    try {
        static_cast<void>(repository.load(save.summary.identifier));
    } catch (const std::runtime_error&) {
        rejectedCorruption = true;
    }
    assert(rejectedCorruption);
    std::filesystem::remove_all(root);
    return 0;
}
