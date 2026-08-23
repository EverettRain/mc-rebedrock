#include "persistence/SaveRepository.hpp"
#include "persistence/UnknownBlockTable.hpp"

#include "core/VersionManifest.hpp"
#include "world/BlockState.hpp"
#include "world/DayNightCycle.hpp"
#include "world/WorldClock.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
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

    // Writes a self-describing block frame (u32 tag, u32 size, u16 version) whose
    // body is produced by `writeBody`, patching the size once the body is known —
    // the same framing SaveBlockWriter emits, so a hand-built block loads exactly
    // as a real one.
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
        integer(hash);
    }
};

// The four-character block tag, matching persistence::blockTag's little-endian
// packing, so the test can address the same owner the writer registered.
[[nodiscard]] constexpr std::uint32_t fourCC(const char (&text)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

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
        {-1, 63, 2,
         world::BlockState{world::Block::Furnace, world::BlockOrientation::East}.withLit(true)},
        {4, 62, -8, world::BlockState{world::Block::Water}.withFluidLevel(3U)},
        // Farmland's moisture is a property of its own now, but the case it
        // guards is the same one: a well-watered farmland used to be a value the
        // orientation byte could not legally hold, and the save refused to load
        // as an "invalid block edit".
        {5, 62, -8, world::BlockState{world::Block::Farmland}.withMoisture(7)},
        // A crop's age likewise, and a hand-placed leaf's PERSISTENT flag: three
        // properties that all used to be the same overloaded byte.
        {6, 62, -8, world::BlockState{world::Block::WheatCrops}.withAge(5)},
        {7, 62, -8, world::BlockState{world::Block::OakLeaves}.withPersistent(true)},
        // A slab's SlabType survives by name too: a top slab and a double slab
        // must reopen in the same half rather than defaulting back to bottom.
        {8, 62, -8,
         world::BlockState{world::Block::OakSlab}.withSlabPortion(world::SlabPortion::Top)},
        {9, 62, -8,
         world::BlockState{world::Block::StoneSlab}.withSlabPortion(world::SlabPortion::Double)},
    };
    gameplay::ChestBlockEntity chest;
    chest.position = {8, 65, -4};
    chest.items[0] = {world::Block::Chest, 1U};
    chest.items[8] = {world::Block::Air, 3U, &gameplay::items::Book};
    save.chests.push_back(chest);
    // BE3's trapped chest block entities: the same ChestBlockEntity record in
    // their own section (TCST), so a trapped chest reopens holding its own
    // contents and is never confused with a chest at load.
    gameplay::ChestBlockEntity trappedChest;
    trappedChest.position = {8, 65, -10};
    trappedChest.items[3] = {world::Block::Air, 7U, &gameplay::items::Diamond};
    trappedChest.items[26] = {world::Block::Stone, 12U};
    save.trappedChests.push_back(trappedChest);
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
        // The zombie is saved mid-burn (entity block/region version 2's
        // fireTicks): it must reopen still ablaze.
        {"zombie", 20.0F, 64.0F, -5.0F, 1.2F, 0.1F, 0.0F, 0.0F, 20.0F, 40, 5U, 0xABCDU, 60},
    };
    // The zombie also carries active MobEffects (entity block/region version 3):
    // they travel by name and must reopen with their remaining duration intact.
    save.entities[1].effects = {
        {"poison", 80, 0},
        {"speed", 200, 1},
    };
    // AgeableMob age/love (entity block/region version 4): the pig is a baby with
    // growth left, the zombie an adult on a breed cooldown and in love.
    save.entities[0].age = -12000;   // pig: baby, halfway grown
    save.entities[1].age = 4000;     // zombie: breed cooldown
    save.entities[1].loveTicks = 300;
    repository.save(save);
    const auto listed = repository.list();
    assert(listed.size() == 1U);
    assert(listed.front().identifier == save.summary.identifier);
    const auto loaded = repository.load(save.summary.identifier);
    assert(loaded.summary.displayName == "TestWorld");
    assert(loaded.summary.seed == 0x12345678ULL);
    // META-1: a world written by this build carries a real (non-derived)
    // self-description that round-trips the write-time VersionManifest snapshot.
    {
        const auto& header = loaded.versionHeader;
        assert(!header.derived);
        assert(header.versionName == std::string{core::kVersion.name});
        assert(header.protocolVersion == core::kVersion.protocolVersion);
        assert(header.buildRef == std::string{core::kVersion.buildRef});
        assert(header.buildTime == std::string{core::kVersion.buildTime});
        assert(header.stable == core::kVersion.stable);
        // Single source: the header's worldVersion is the save's own top-level
        // format number, not a second independent value. It equals the manifest's
        // worldVersion because both come from the one place.
        assert(header.worldVersion == core::kVersion.worldVersion);
    }
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
    assert(loaded.edits[0].state.lit());
    // Every property survives the round trip by name, not by column.
    assert(loaded.edits[0].state.orientation() == world::BlockOrientation::East);
    assert(loaded.edits[1].state.fluidLevel() == 3U);
    assert(loaded.edits[2].state.moisture() == 7);
    assert(loaded.edits[3].state.age() == 5);
    assert(loaded.edits[4].state.persistent());
    assert(loaded.edits[5].state.block() == world::Block::OakSlab);
    assert(loaded.edits[5].state.slabPortion() == world::SlabPortion::Top);
    assert(loaded.edits[6].state.block() == world::Block::StoneSlab);
    assert(loaded.edits[6].state.slabPortion() == world::SlabPortion::Double);
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
    // The trapped chest round-trips in its own section, distinct from the chest.
    assert(loaded.trappedChests.size() == 1U);
    assert(loaded.trappedChests.front().position == trappedChest.position);
    assert(loaded.trappedChests.front().items == trappedChest.items);
    assert(loaded.trappedChests.front().items[3].item == &gameplay::items::Diamond);
    assert(loaded.trappedChests.front().items[3].count == 7U);
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
    // The herd round-trips per chunk now — entities ride in their chunk's region
    // record — so the order they come back in is the region file order, not the
    // save order. Look each one up by species instead of by index.
    assert(loaded.entities.size() == 2U);
    const auto entityBySpecies = [&](const char* species) -> const persistence::PersistentEntity* {
        for (const auto& entity : loaded.entities) {
            if (entity.species == species) {
                return &entity;
            }
        }
        return nullptr;
    };
    const auto* pig = entityBySpecies("pig");
    const auto* zombie = entityBySpecies("zombie");
    assert(pig != nullptr);
    assert(pig->x == 10.0F && pig->y == 64.0F && pig->z == 8.0F);
    assert(pig->yaw == 0.5F);
    assert(pig->health == 10.0F);
    assert(pig->ageTicks == 120U);
    assert(pig->rngState == 0x1234U);
    // The pig was never lit, so it reopens not on fire.
    assert(pig->fireTicks == 0);
    assert(zombie != nullptr);
    assert(zombie->angerTicks == 40);
    assert(zombie->rngState == 0xABCDU);
    // The burning zombie kept its fireTicks across the round trip.
    assert(zombie->fireTicks == 60);
    // Its active effects survived by name, with their durations and amplifiers.
    assert(zombie->effects.size() == 2U);
    {
        const auto effectByName = [&](const char* name) -> const persistence::PersistentEffect* {
            for (const auto& effect : zombie->effects) {
                if (effect.name == name) {
                    return &effect;
                }
            }
            return nullptr;
        };
        const auto* poison = effectByName("poison");
        const auto* speed = effectByName("speed");
        assert(poison != nullptr && poison->durationTicks == 80 && poison->amplifier == 0);
        assert(speed != nullptr && speed->durationTicks == 200 && speed->amplifier == 1);
    }
    // The pig carried no effects.
    assert(pig->effects.empty());
    // AgeableMob age/love survived: the pig is still a baby with its remaining
    // growth, the zombie still on cooldown and in love.
    assert(pig->age == -12000);
    assert(pig->loveTicks == 0);
    assert(zombie->age == 4000);
    assert(zombie->loveTicks == 300);

    // Blocks travel as namespaced identifiers now, so the payload literally
    // contains them and a renumbered enum cannot silently reinterpret an old
    // save. Block edits live in region/ files since M-3; block entities and the
    // non-default rule stay in world.dat.
    const auto worldDat = [&] {
        std::ifstream data{root / save.summary.identifier / "world.dat", std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{data},
                           std::istreambuf_iterator<char>{}};
    }();
    const auto regionBytes = [&] {
        std::string all;
        for (const auto& entry :
             std::filesystem::directory_iterator(root / save.summary.identifier / "region")) {
            std::ifstream data{entry.path(), std::ios::binary};
            all += std::string{std::istreambuf_iterator<char>{data},
                               std::istreambuf_iterator<char>{}};
        }
        return all;
    }();
    // The edit states (furnace, water) ride in the region files.
    assert(regionBytes.find("rebedrock:furnace") != std::string::npos);
    assert(regionBytes.find("rebedrock:water") != std::string::npos);
    // The chest block entity and its item palette stay in world.dat.
    assert(worldDat.find("rebedrock:chest") != std::string::npos);
    assert(worldDat.find("rebedrock:book") != std::string::npos);
    // Only the content the save mentions is written, not the whole registry.
    assert(regionBytes.find("rebedrock:granite") == std::string::npos);
    assert(worldDat.find("rebedrock:granite") == std::string::npos);
    assert(worldDat.find("rebedrock:feather") == std::string::npos);
    // The non-default randomTickSpeed travels as a named entry in the GameRules
    // block.
    assert(worldDat.find("randomTickSpeed") != std::string::npos);

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
        assert(legacy.edits[0].state.block() == world::Block::Stone);
        // Ordinal 49 was `wall_torch_north`, which is no longer a block of its
        // own: format 14 turns it back into the one wall torch plus a facing.
        // Losing that mapping would silently rotate every wall torch in an old
        // world to the default north face.
        assert(legacy.edits[1].state.block() == world::Block::WallTorch);
        assert(legacy.edits[1].state.orientation() == world::BlockOrientation::North);
        assert(!legacy.edits[1].state.lit());
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
        // META-1: a real old world has no VERS block, so its version header is
        // reconstructed from the format number — worldVersion == 8, name unknown,
        // `derived` set. The load neither crashed nor rejected the save.
        assert(migrated.versionHeader.derived);
        assert(migrated.versionHeader.worldVersion == 8U);
        assert(migrated.versionHeader.versionName.empty());
        // Saving the migrated world rewrites it as format 10, where the value
        // lives in the GameRules block and the absent spawn point is preserved.
        repository.save(migrated);
        const auto roundTrip = repository.load(v8Identifier);
        assert(roundTrip.gameRules.get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed) == 7);
        assert(!roundTrip.hasSpawnPoint);
        // Re-saving stamped the current build's identity: the rewritten world now
        // self-describes with a real (non-derived) header at the current version.
        assert(!roundTrip.versionHeader.derived);
        assert(roundTrip.versionHeader.worldVersion == core::kVersion.worldVersion);
        assert(roundTrip.versionHeader.versionName == std::string{core::kVersion.name});
        std::filesystem::remove_all(v8Directory);
    }

    // META-1: a save's version header records the version at *write* time, not the
    // one reading it. A world hand-written with a VERS block naming a different
    // build ("25.0", protocol 3) must read back that build, proving the header is
    // the writer's snapshot — self-description would be worthless otherwise.
    {
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(core::kVersion.worldVersion);  // current format
        writer.integer<std::uint64_t>(0x2500ULL);                    // seed
        writer.integer<std::uint16_t>(1U);  // block palette: air only
        writer.stringValue("air");
        writer.integer<std::uint16_t>(1U);  // item palette: the empty sentinel
        writer.stringValue("");
        writer.block(fourCC("VERS"), 1U, [&] {
            writer.integer<std::uint32_t>(core::kVersion.worldVersion);  // worldVersion
            writer.integer<std::uint32_t>(3U);                           // protocolVersion
            writer.stringValue("25.0");                                  // versionName
            writer.stringValue("abc1234");                               // buildRef
            writer.stringValue("2025-01-02T03:04:05Z");                  // buildTime
            writer.integer<std::uint8_t>(0U);                           // stable = false
        });
        writer.finish();

        const std::string otherIdentifier = "other-build-world";
        const auto otherDirectory = root / otherIdentifier;
        std::filesystem::create_directories(otherDirectory);
        {
            std::ofstream metadata{otherDirectory / "level.properties"};
            metadata << "format=" << core::kVersion.worldVersion << "\nid=" << otherIdentifier
                     << "\nname=Other\nseed=9472\nlast_played=1\n";
        }
        {
            std::ofstream data{otherDirectory / "world.dat", std::ios::binary};
            data.write(reinterpret_cast<const char*>(writer.bytes.data()),
                       static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto other = repository.load(otherIdentifier);
        assert(!other.versionHeader.derived);
        assert(other.versionHeader.versionName == "25.0");
        assert(other.versionHeader.protocolVersion == 3U);
        assert(other.versionHeader.buildRef == "abc1234");
        assert(other.versionHeader.buildTime == "2025-01-02T03:04:05Z");
        assert(!other.versionHeader.stable);
        // Not the current build's name — the write-time snapshot was preserved.
        assert(other.versionHeader.versionName != std::string{core::kVersion.name});
        std::filesystem::remove_all(otherDirectory);
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
    // --- Format 16: dropped items and blocks mid-fall survive a round trip.
    // Before it both simply vanished on reload — everything thrown or mined but
    // not yet picked up, and any block caught partway through a collapse. ---
    {
        auto game = repository.create("Drops", 7ULL);

        persistence::PersistentItemDrop stoneDrop;
        stoneDrop.x = 12.5F;
        stoneDrop.y = 65.0F;
        stoneDrop.z = -3.25F;
        stoneDrop.vx = 0.05F;
        stoneDrop.vy = -0.2F;
        stoneDrop.vz = -0.05F;
        stoneDrop.stack = {world::Block::Stone, 7U};
        stoneDrop.ageTicks = 1234U;
        game.itemDrops.push_back(stoneDrop);

        // An item-backed stack too: the block and item palettes are separate.
        persistence::PersistentItemDrop diamondDrop;
        diamondDrop.x = 1.0F;
        diamondDrop.y = 70.0F;
        diamondDrop.z = 1.0F;
        diamondDrop.stack = {world::Block::Air, 3U, &gameplay::items::Diamond};
        diamondDrop.ageTicks = 5U;
        game.itemDrops.push_back(diamondDrop);

        persistence::PersistentFallingBlock sand;
        sand.x = 4.5F;
        sand.y = 80.5F;
        sand.z = 4.5F;
        sand.verticalVelocity = -0.42F;
        sand.block = world::Block::Sand;
        game.fallingBlocks.push_back(sand);

        repository.save(game);
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.itemDrops.size() == 2U);
        assert(loaded.itemDrops[0].stack.block == world::Block::Stone);
        assert(loaded.itemDrops[0].stack.count == 7U);
        // The age matters: it drives both the despawn timer and the pickup delay.
        assert(loaded.itemDrops[0].ageTicks == 1234U);
        assert(std::fabs(loaded.itemDrops[0].x - 12.5F) < 1e-4F);
        assert(std::fabs(loaded.itemDrops[0].vy + 0.2F) < 1e-4F);
        assert(loaded.itemDrops[1].stack.item == &gameplay::items::Diamond);
        assert(loaded.itemDrops[1].stack.count == 3U);

        assert(loaded.fallingBlocks.size() == 1U);
        assert(loaded.fallingBlocks[0].block == world::Block::Sand);
        assert(std::fabs(loaded.fallingBlocks[0].verticalVelocity + 0.42F) < 1e-4F);
        assert(std::fabs(loaded.fallingBlocks[0].y - 80.5F) < 1e-4F);
    }

    // --- A world with nothing dropped still round-trips: the block is written
    // empty rather than omitted, so the reader's cursor stays in step. ---
    {
        auto game = repository.create("NoDrops", 8ULL);
        repository.save(game);
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.itemDrops.empty());
        assert(loaded.fallingBlocks.empty());
    }

    // --- Format 17: the edits are grouped by the chunk that owns them, and the
    // coordinates inside a group are chunk-local. Negative coordinates are where
    // that goes wrong: -1 belongs to chunk -1 at local 15, and a truncating
    // division would put it in chunk 0 at local -1. ---
    {
        auto game = repository.create("ChunkGrouped", 21ULL);
        const std::array<std::pair<std::int32_t, std::int32_t>, 7> positions{{
            {0, 0}, {15, 15}, {16, 0}, {-1, -1}, {-16, -17}, {-33, 40}, {1000, -1000},
        }};
        for (const auto& [x, z] : positions) {
            world::PersistentBlockEdit edit;
            edit.x = x;
            edit.y = 70;
            edit.z = z;
            edit.state = world::BlockState{world::Block::Stone};
            game.edits.push_back(edit);
        }
        // Two edits on the same cell: the later one is the cell's final state, so
        // the grouping sort has to be stable or the world reloads wrong.
        world::PersistentBlockEdit first;
        first.x = 5;
        first.y = 71;
        first.z = 5;
        first.state = world::BlockState{world::Block::Dirt};
        game.edits.push_back(first);
        auto second = first;
        second.state = world::BlockState{world::Block::Cobblestone};
        game.edits.push_back(second);
        // A state-carrying edit, so the state palette is exercised rather than
        // just the block one: a lit furnace facing west. The old packed byte
        // spent four bits on the fluid level, three on the orientation and one
        // on lit, and had nothing left; this goes through named properties.
        world::PersistentBlockEdit stateful;
        stateful.x = -20;
        stateful.y = 64;
        stateful.z = 7;
        stateful.state =
            world::BlockState{world::Block::Furnace, world::BlockOrientation::West}.withLit(true);
        game.edits.push_back(stateful);
        // And a deep-water edit, whose level 8 no longer shares a byte with
        // anything.
        world::PersistentBlockEdit deep;
        deep.x = -21;
        deep.y = 64;
        deep.z = 7;
        deep.state = world::BlockState{world::Block::Water}.withFluidLevel(8U);
        game.edits.push_back(deep);
        repository.save(game);
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.edits.size() == game.edits.size());
        // The order changes — the edits come back grouped — so compare as sets of
        // positions, then check the two that carry meaning.
        for (const auto& original : game.edits) {
            const auto found = std::ranges::find_if(
                loaded.edits, [&](const world::PersistentBlockEdit& candidate) {
                    return candidate.x == original.x && candidate.y == original.y &&
                           candidate.z == original.z && candidate.state == original.state;
                });
            assert(found != loaded.edits.end());
        }
        const auto* winner = static_cast<const world::PersistentBlockEdit*>(nullptr);
        for (const auto& edit : loaded.edits) {
            if (edit.x == 5 && edit.y == 71 && edit.z == 5) {
                winner = &edit;
            }
        }
        assert(winner != nullptr && winner->state.block() == world::Block::Cobblestone);
        const auto reloadedState = std::ranges::find_if(
            loaded.edits,
            [](const world::PersistentBlockEdit& edit) { return edit.x == -20; });
        assert(reloadedState != loaded.edits.end());
        assert(reloadedState->state.lit());
        assert(reloadedState->state.orientation() == world::BlockOrientation::West);
        const auto reloadedDeep = std::ranges::find_if(
            loaded.edits,
            [](const world::PersistentBlockEdit& edit) { return edit.x == -21; });
        assert(reloadedDeep != loaded.edits.end());
        assert(reloadedDeep->state.fluidLevel() == 8U);
    }

    // --- The packing is the point of the grouping: a thousand edits in one
    // chunk must cost about five bytes each, not the nineteen format 16 spent on
    // three absolute coordinates plus four loose bytes. Format 18's state
    // palette took the sixth byte back as well: a thousand stone edits name one
    // state once instead of repeating a block index and a packed byte. ---
    {
        auto game = repository.create("EditDensity", 22ULL);
        for (int index = 0; index < 1000; ++index) {
            world::PersistentBlockEdit edit;
            edit.x = index % 16;
            edit.y = 64 + (index / 256);
            edit.z = (index / 16) % 16;
            edit.state = world::BlockState{world::Block::Stone};
            game.edits.push_back(edit);
        }
        repository.save(game);
        // M-3: the edits live in the chunk's region file now, not world.dat —
        // so measure the region file the thousand edits land in (all in chunk
        // (0,0), region (0,0)). Five bytes a record plus the palettes and the
        // chunk frames; format 16's 17-byte records could not have fitted this
        // under 17'000 bytes for the edits alone.
        const auto path = repository.root() / game.summary.identifier / "region" / "r.0.0.cache";
        const auto size = std::filesystem::file_size(path);
        assert(size < 7'000U);
        // And world.dat is small now that the edit list is out of it.
        const auto worldDatSize = std::filesystem::file_size(
            repository.root() / game.summary.identifier / "world.dat");
        assert(worldDatSize < 7'000U);
        assert(repository.load(game.summary.identifier).edits.size() == 1000U);
    }

    // --- Container slots travel sparsely: only the occupied ones are written,
    // each behind its index. A chest holding one item costs a handful of bytes
    // rather than 27 dense stack records. ---
    {
        auto game = repository.create("SparseChest", 23ULL);
        gameplay::ChestBlockEntity chest;
        chest.position = {4, 65, 4};
        chest.items[26] = {world::Block::Air, 5U, &gameplay::items::Diamond, 0U};
        game.chests.push_back(chest);
        repository.save(game);
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.chests.size() == 1U);
        assert(loaded.chests.front().items[26].item == &gameplay::items::Diamond);
        assert(loaded.chests.front().items[26].count == 5U);
        assert(loaded.chests.front().items[0].empty());
        const auto size = std::filesystem::file_size(
            repository.root() / game.summary.identifier / "world.dat");
        // A dense chest alone would be 27 x 7 bytes; the whole file stays under
        // that plus the fixed blocks.
        assert(size < 1'200U);
    }

    // --- Forward compatibility: a block written by a build this one has never
    // met is skipped by its own size, and everything around it still loads. That
    // is the whole reason the reader dispatches on the tag instead of counting
    // offsets from the start of the file. ---
    {
        auto game = repository.create("UnknownOwner", 24ULL);
        game.playerHealth = 12.5F;
        game.hasPlayerPosition = true;
        game.playerX = 3.5F;
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
        // Drop the trailing checksum, append a block nothing knows, re-checksum.
        bytes.resize(bytes.size() - sizeof(std::uint64_t));
        LegacyWriter writer;
        writer.bytes = std::move(bytes);
        writer.integer<std::uint32_t>('Z' | ('Z' << 8) | ('Z' << 16) | ('Z' << 24));
        writer.integer<std::uint32_t>(10U + 5U);  // header + five body bytes
        writer.integer<std::uint16_t>(1U);
        for (int index = 0; index < 5; ++index) {
            writer.bytes.push_back(0xABU);
        }
        writer.finish();
        {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(writer.bytes.data()),
                         static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.playerHealth == 12.5F);
        assert(loaded.hasPlayerPosition && loaded.playerX == 3.5F);
    }

    // --- Nothing is positional any more: a payload carrying only the world block
    // loads, and every owner that said nothing keeps its default. Under format 16
    // a missing section desynchronised everything after it. ---
    {
        auto game = repository.create("SparseOwners", 25ULL);
        repository.save(game);
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(17U);
        writer.integer<std::uint64_t>(25ULL);
        writer.integer<std::uint16_t>(1U);  // block palette: air only
        writer.stringValue("rebedrock:air");
        writer.integer<std::uint16_t>(1U);  // item palette: the block sentinel
        writer.stringValue("");
        writer.integer<std::uint32_t>('W' | ('R' << 8) | ('L' << 16) | ('D' << 24));
        writer.integer<std::uint32_t>(10U + 2U);
        writer.integer<std::uint16_t>(1U);
        writer.integer<std::uint8_t>(static_cast<std::uint8_t>(gameplay::GameMode::Survival));
        writer.integer<std::uint8_t>(static_cast<std::uint8_t>(gameplay::Difficulty::Hard));
        writer.finish();
        {
            std::ofstream output{
                repository.root() / game.summary.identifier / "world.dat",
                std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(writer.bytes.data()),
                         static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.gameMode == gameplay::GameMode::Survival);
        assert(loaded.difficulty == gameplay::Difficulty::Hard);
        assert(loaded.edits.empty());
        assert(loaded.chests.empty());
        assert(!loaded.hasPlayerPosition);
        assert(loaded.playerHealth == gameplay::PlayerVitals::kMaximumHealth);
    }

    // --- Format 17 -> 18: the overloaded orientation byte migrates into named
    // properties. This is the upgrade path every existing world takes, and the
    // one place where getting it wrong is silent: a crop whose age was read as a
    // facing comes back as a seedling, a well-watered field comes back dry, and
    // hand-placed leaves start decaying. ---
    {
        auto game = repository.create("StateMigration", 26ULL);
        repository.save(game);
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(17U);
        writer.integer<std::uint64_t>(26ULL);
        // Block palette: air, wheat, farmland, oak leaves, furnace.
        writer.integer<std::uint16_t>(5U);
        writer.stringValue("rebedrock:air");
        writer.stringValue("rebedrock:wheat");
        writer.stringValue("rebedrock:farmland");
        writer.stringValue("rebedrock:oak_leaves");
        writer.stringValue("rebedrock:furnace");
        writer.integer<std::uint16_t>(1U);  // item palette: the block sentinel
        writer.stringValue("");

        // One CHNK block at version 1: four edits in chunk (0, 0), each six
        // bytes, with the state packed as fluid | orientation << 4 | lit << 7.
        writer.integer<std::uint32_t>('C' | ('H' << 8) | ('N' << 16) | ('K' << 24));
        // The size field covers the whole frame, header included: 10 bytes of
        // tag/size/version, then the block's own payload.
        writer.integer<std::uint32_t>(10U + 4U + 4U + 12U + 4U * 6U);
        writer.integer<std::uint16_t>(1U);  // CHNK version 1
        writer.integer<std::uint32_t>(1U);  // one chunk
        writer.integer<std::uint32_t>(4U);  // four edits in total
        writer.integer<std::int32_t>(0);
        writer.integer<std::int32_t>(0);
        writer.integer<std::uint32_t>(4U);
        const auto legacyEdit = [&](std::uint8_t packedXZ, std::int16_t y,
                                    std::uint16_t blockIndex, std::uint8_t packedState) {
            writer.integer<std::uint8_t>(packedXZ);
            writer.integer<std::int16_t>(y);
            writer.integer<std::uint16_t>(blockIndex);
            writer.integer<std::uint8_t>(packedState);
        };
        // Wheat with the age 5 that used to live in the orientation nibble.
        legacyEdit(0x00U, 64, 1U, static_cast<std::uint8_t>(5U << 4U));
        // Farmland at moisture 7, the value that could not be a legal facing.
        legacyEdit(0x01U, 64, 2U, static_cast<std::uint8_t>(7U << 4U));
        // Leaves flagged persistent by the magic East orientation, ordinal 1.
        legacyEdit(0x02U, 64, 3U, static_cast<std::uint8_t>(1U << 4U));
        // A lit furnace facing west (ordinal 3), the top bit plus a real facing.
        legacyEdit(0x03U, 64, 4U, static_cast<std::uint8_t>((3U << 4U) | 0x80U));
        writer.finish();
        {
            std::ofstream output{
                repository.root() / game.summary.identifier / "world.dat",
                std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(writer.bytes.data()),
                         static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.edits.size() == 4U);
        assert(loaded.edits[0].state.block() == world::Block::WheatCrops);
        assert(loaded.edits[0].state.age() == 5);
        assert(loaded.edits[1].state.block() == world::Block::Farmland);
        assert(loaded.edits[1].state.moisture() == 7);
        assert(loaded.edits[2].state.block() == world::Block::OakLeaves);
        assert(loaded.edits[2].state.persistent());
        assert(loaded.edits[3].state.block() == world::Block::Furnace);
        assert(loaded.edits[3].state.lit());
        assert(loaded.edits[3].state.orientation() == world::BlockOrientation::West);
        // And a crop's age must not have been mistaken for a facing on the way
        // in: wheat has no facing at all.
        assert(loaded.edits[0].state.orientation() == world::BlockOrientation::North);

        // Saving it again writes format 18, and the properties survive that too.
        auto migrated = loaded;
        repository.save(migrated);
        const auto reloaded = repository.load(game.summary.identifier);
        assert(reloaded.edits.size() == 4U);
        for (const auto& original : loaded.edits) {
            const auto found = std::ranges::find_if(
                reloaded.edits, [&](const world::PersistentBlockEdit& candidate) {
                    return candidate.x == original.x && candidate.y == original.y &&
                           candidate.z == original.z;
                });
            assert(found != reloaded.edits.end());
            assert(found->state == original.state);
        }
    }

    // --- A property this build has never heard of is skipped, not fatal. That is
    // the forward half of writing names instead of columns: a world saved by a
    // build that knows about stairs still opens here, minus the stair shape. ---
    {
        auto game = repository.create("UnknownProperty", 27ULL);
        repository.save(game);
        LegacyWriter writer;
        writer.magic();
        writer.integer<std::uint32_t>(18U);
        writer.integer<std::uint64_t>(27ULL);
        writer.integer<std::uint16_t>(2U);
        writer.stringValue("rebedrock:air");
        writer.stringValue("rebedrock:wheat");
        writer.integer<std::uint16_t>(1U);
        writer.stringValue("");
        writer.integer<std::uint32_t>('C' | ('H' << 8) | ('N' << 16) | ('K' << 24));
        // chunkCount + total + palette(count, entry) + chunk header + one record
        // count, then one entry: block index, property count, and two
        // length-prefixed names each followed by a value byte.
        const std::uint32_t paletteBytes = 2U + (2U + 1U + (2U + 3U + 1U) + (2U + 5U + 1U));
        writer.integer<std::uint32_t>(10U + 4U + 4U + paletteBytes + 12U + 5U);
        writer.integer<std::uint16_t>(2U);  // CHNK version 2
        writer.integer<std::uint32_t>(1U);
        writer.integer<std::uint32_t>(1U);
        writer.integer<std::uint16_t>(1U);  // one palette entry
        writer.integer<std::uint16_t>(1U);  // block index: wheat
        writer.integer<std::uint8_t>(2U);   // two properties
        writer.stringValue("age");
        writer.integer<std::uint8_t>(6U);
        writer.stringValue("shape");  // no such property in this build
        writer.integer<std::uint8_t>(3U);
        writer.integer<std::int32_t>(0);
        writer.integer<std::int32_t>(0);
        writer.integer<std::uint32_t>(1U);
        writer.integer<std::uint8_t>(0x00U);
        writer.integer<std::int16_t>(70);
        writer.integer<std::uint16_t>(0U);
        writer.finish();
        {
            std::ofstream output{
                repository.root() / game.summary.identifier / "world.dat",
                std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(writer.bytes.data()),
                         static_cast<std::streamsize>(writer.bytes.size()));
        }
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.edits.size() == 1U);
        assert(loaded.edits[0].state.block() == world::Block::WheatCrops);
        // The property it does know still arrived.
        assert(loaded.edits[0].state.age() == 6);
    }

    // --- M-3 region files: edits and creatures ride in the chunk's region
    // record. A world saved now writes region/r.<rx>.<rz>.cache files and keeps
    // CHNK and ENTY out of world.dat; loading pulls both sources back. ---
    {
        auto game = repository.create("Regioned", 28ULL);
        // Two edits in different chunks of different regions (negative too).
        game.edits.push_back({-1, 64, -1, world::BlockState{world::Block::Stone}});
        game.edits.push_back({40, 64, 40, world::BlockState{world::Block::Dirt}});
        // A creature in each of the same two regions.
        game.entities.push_back(
            {"pig", -8.5F, 64.0F, -8.5F, 0.0F, 0.0F, 0.0F, 0.0F, 10.0F, 0, 0U, 0U});
        game.entities.push_back(
            {"zombie", 40.5F, 64.0F, 40.5F, 0.0F, 0.0F, 0.0F, 0.0F, 20.0F, 0, 0U, 0U});
        repository.save(game);
        const auto saveDirectory = repository.root() / game.summary.identifier;
        // Chunk (-1,-1) is region (-1,-1); chunk (2,2) is region (0,0).
        assert(std::filesystem::is_regular_file(saveDirectory / "region" / "r.-1.-1.cache"));
        assert(std::filesystem::is_regular_file(saveDirectory / "region" / "r.0.0.cache"));
        // world.dat no longer carries the edit/entity blocks.
        {
            std::ifstream data{saveDirectory / "world.dat", std::ios::binary};
            const std::string bytes{std::istreambuf_iterator<char>{data},
                                    std::istreambuf_iterator<char>{}};
            assert(bytes.find("CHNK") == std::string::npos);
            assert(bytes.find("ENTY") == std::string::npos);
        }
        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.edits.size() == 2U);
        assert(loaded.entities.size() == 2U);
        const auto pig = std::ranges::find_if(loaded.entities, [](const auto& entity) {
            return entity.species == "pig";
        });
        assert(pig != loaded.entities.end());
        assert(pig->x == -8.5F && pig->z == -8.5F);

        // Reverting every edit and removing every creature prunes the region
        // file: content that no longer exists must not linger in an old file and
        // resurrect on the next load.
        auto reverted = loaded;
        reverted.edits.clear();
        reverted.entities.clear();
        repository.save(reverted);
        assert(!std::filesystem::exists(saveDirectory / "region" / "r.-1.-1.cache"));
        assert(!std::filesystem::exists(saveDirectory / "region" / "r.0.0.cache"));
        const auto reloaded = repository.load(game.summary.identifier);
        assert(reloaded.edits.empty());
        assert(reloaded.entities.empty());
    }

    // --- A torn region file regenerates from seed rather than refusing the
    // world: a chunk's edits are the regenerable part, unlike world.dat, whose
    // checksum mismatch refuses to open. ---
    {
        auto game = repository.create("TornRegion", 29ULL);
        game.edits.push_back({0, 64, 0, world::BlockState{world::Block::Stone}});
        repository.save(game);
        const auto saveDirectory = repository.root() / game.summary.identifier;
        const auto regionPath = saveDirectory / "region" / "r.0.0.cache";
        assert(std::filesystem::is_regular_file(regionPath));
        {
            std::fstream data{regionPath, std::ios::binary | std::ios::in | std::ios::out};
            assert(data);
            char byte = 0;
            data.seekg(12);
            data.read(&byte, 1);
            data.seekp(12);
            byte ^= 0x5A;
            data.write(&byte, 1);
        }
        const auto loaded = repository.load(game.summary.identifier);
        // The corrupted region is skipped; nothing else is lost.
        assert(loaded.edits.empty());
        assert(loaded.entities.empty());
    }

    // A block a removed datapack/mod placed is kept as an UnknownBlock
    // placeholder rather than dropped to air: its identifier and property blob
    // ride the save unchanged, and re-adding the content restores the real block.
    {
        // Stand in for content this build's registry does not know. The blob mixes
        // a property this build happens to have a name for ("age") with one it does
        // not ("custom"), to prove the bytes survive without interpretation.
        const auto placeholder = persistence::unknownBlockTable().intern(
            "moddedmc:gadget",
            {{"age", 5U}, {"custom", 2U}});
        assert(persistence::unknownBlockTable().isUnknown(placeholder));
        // A second placeholder whose name is one this build *does* know, standing
        // in for the same on-disk bytes read by a build where the content is
        // (re)registered: it must resolve to the real block, not a placeholder.
        const auto reregistered = persistence::unknownBlockTable().intern(
            "rebedrock:furnace",
            {{"lit", 1U}});
        assert(persistence::unknownBlockTable().isUnknown(reregistered));

        auto game = repository.create("Unknown", 7ULL);
        game.edits = {
            {100, 70, 100, placeholder},
            {101, 70, 100, world::BlockState{world::Block::Stone}},
            {102, 70, 100, reregistered},
        };
        repository.save(game);

        // The original identifier is on disk verbatim — no data was lost to air.
        const auto regionBytes = [&] {
            std::string all;
            for (const auto& entry : std::filesystem::directory_iterator(
                     root / game.summary.identifier / "region")) {
                std::ifstream data{entry.path(), std::ios::binary};
                all += std::string{std::istreambuf_iterator<char>{data},
                                   std::istreambuf_iterator<char>{}};
            }
            return all;
        }();
        assert(regionBytes.find("moddedmc:gadget") != std::string::npos);

        const auto loaded = repository.load(game.summary.identifier);
        assert(loaded.edits.size() == 3U);
        const auto editAt = [&](int x) -> const world::PersistentBlockEdit& {
            for (const auto& edit : loaded.edits) {
                if (edit.x == x) return edit;
            }
            assert(false && "edit missing after round-trip");
            std::abort();
        };
        // The unknown block came back as the same placeholder (interning dedupes
        // by name+blob), and its stored identity is intact — not air.
        const auto& unknown = editAt(100);
        assert(persistence::unknownBlockTable().isUnknown(unknown.state));
        assert(unknown.state != world::BlockState{world::Block::Air});
        const auto record = persistence::unknownBlockTable().record(unknown.state);
        assert(record.name == "moddedmc:gadget");
        assert((record.properties ==
                std::vector<persistence::UnknownStateProperty>{{"age", 5U}, {"custom", 2U}}));
        // The known block beside it is unaffected.
        assert(editAt(101).state.block() == world::Block::Stone);
        // The placeholder whose name this build knows resolves to the real block:
        // this is the "re-register the content and it restores" guarantee.
        const auto& restored = editAt(102);
        assert(!persistence::unknownBlockTable().isUnknown(restored.state));
        assert(restored.state.block() == world::Block::Furnace);
        assert(restored.state.lit());
    }

    return 0;
}
