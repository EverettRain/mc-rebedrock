// Temporary V0 diagnostic: proves the format-10 -> 18 migration path works on a
// real, played world by loading a copy of it, saving (which rewrites to the
// current format), reloading and asserting nothing was lost. Never touches the
// source directory: the world is copied into a scratch root first.
//
// Usage: migration_diag <path-to-world-directory>   (e.g. .../saves/aaa-1786122369)
//
// This is the automated half of PENDING_WORK:server-client-split.md §1 (V0
// 存档格式 18 真实旧世界迁移验收). The visual half stays manual: load the same
// world in the release build and eyeball crops/farmland/leaves/lit furnace/
// containers/drops/mobs.

#include "persistence/SaveRepository.hpp"

#include "world/DayNightCycle.hpp"
#include "world/WorldClock.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>

using namespace mc;

namespace {

[[nodiscard]] int countNonEmpty(
    const std::array<gameplay::ItemStack, gameplay::Inventory::kSlotCount>& inv) {
    int count = 0;
    for (const auto& stack : inv) {
        if (!stack.empty()) ++count;
    }
    return count;
}

// Format 18 groups edits by owning chunk on save, so the edit vector is
// reordered relative to a legacy flat load. Content is what must survive: for
// every cell, the ordered list of states applied to it (later entries win) must
// be identical. This checks both the multiset and the per-cell ordering.
[[nodiscard]] bool editsEqual(
    const std::vector<world::PersistentBlockEdit>& a,
    const std::vector<world::PersistentBlockEdit>& b) {
    if (a.size() != b.size()) return false;
    auto sequenceFor = [](const std::vector<world::PersistentBlockEdit>& edits) {
        std::map<std::tuple<int, int, int>, std::vector<std::uint32_t>> byCell;
        for (const auto& edit : edits) {
            byCell[{edit.x, edit.y, edit.z}].push_back(edit.state.rawId());
        }
        return byCell;
    };
    return sequenceFor(a) == sequenceFor(b);
}

[[nodiscard]] bool chestsEqual(
    const std::vector<gameplay::ChestBlockEntity>& a,
    const std::vector<gameplay::ChestBlockEntity>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].position == b[i].position)) return false;
        if (a[i].items != b[i].items) return false;
        // lid/open are transient animation state; position + contents are the
        // persisted identity.
    }
    return true;
}

[[nodiscard]] bool furnacesEqual(
    const std::vector<gameplay::FurnaceBlockEntity>& a,
    const std::vector<gameplay::FurnaceBlockEntity>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].position == b[i].position)) return false;
        if (!(a[i].input == b[i].input)) return false;
        if (!(a[i].fuel == b[i].fuel)) return false;
        if (!(a[i].output == b[i].output)) return false;
        if (a[i].burnTicks != b[i].burnTicks) return false;
        if (a[i].initialBurnTicks != b[i].initialBurnTicks) return false;
        if (a[i].cookTicks != b[i].cookTicks) return false;
        if (a[i].cookDurationTicks != b[i].cookDurationTicks) return false;
        // activeRecipe is a cached view re-pointed by restore(); not persisted.
    }
    return true;
}

[[nodiscard]] bool entitiesEqual(
    const std::vector<persistence::PersistentEntity>& a,
    const std::vector<persistence::PersistentEntity>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].species != b[i].species) return false;
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
        if (a[i].yaw != b[i].yaw) return false;
        if (a[i].vx != b[i].vx || a[i].vy != b[i].vy || a[i].vz != b[i].vz) return false;
        if (a[i].health != b[i].health) return false;
        if (a[i].angerTicks != b[i].angerTicks) return false;
        if (a[i].ageTicks != b[i].ageTicks) return false;
        if (a[i].rngState != b[i].rngState) return false;
    }
    return true;
}

[[nodiscard]] bool itemDropsEqual(
    const std::vector<persistence::PersistentItemDrop>& a,
    const std::vector<persistence::PersistentItemDrop>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
        if (a[i].vx != b[i].vx || a[i].vy != b[i].vy || a[i].vz != b[i].vz) return false;
        if (!(a[i].stack == b[i].stack)) return false;
        if (a[i].ageTicks != b[i].ageTicks) return false;
    }
    return true;
}

[[nodiscard]] bool fallingBlocksEqual(
    const std::vector<persistence::PersistentFallingBlock>& a,
    const std::vector<persistence::PersistentFallingBlock>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
        if (a[i].verticalVelocity != b[i].verticalVelocity) return false;
        if (a[i].block != b[i].block) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: migration_diag <path-to-world-directory>\n";
        return 2;
    }
    const std::filesystem::path source = argv[1];
    if (!std::filesystem::exists(source / "world.dat")) {
        std::cerr << "no world.dat in " << source.string() << "\n";
        return 2;
    }

    // Copy into a scratch root so the source world is never modified.
    const auto scratch = std::filesystem::temp_directory_path() / "v0-migration-diag";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::filesystem::copy(source, scratch / source.filename(),
                          std::filesystem::copy_options::recursive);
    const std::string identifier = source.filename().string();
    persistence::SaveRepository repository{scratch};

    // 1. Load the old-format world (exercises the version-gated legacy readers).
    const auto before = repository.load(identifier);
    std::cout << "LOADED " << before.summary.displayName << " ("
              << before.summary.seed << ")\n";
    std::cout << "  player at (" << before.playerX << ", " << before.playerY << ", "
              << before.playerZ << ")\n";
    std::cout << "  edits=" << before.edits.size()
              << " chests=" << before.chests.size()
              << " furnaces=" << before.furnaces.size()
              << " entities=" << before.entities.size()
              << " itemDrops=" << before.itemDrops.size()
              << " fallingBlocks=" << before.fallingBlocks.size() << "\n";
    std::cout << "  inventory non-empty=" << countNonEmpty(before.inventory)
              << " gameTime=" << before.gameTimeSeconds
              << " serverTick=" << before.serverTick
              << " raining=" << (before.weather.raining ? "yes" : "no")
              << " gameMode="
              << (before.gameMode == gameplay::GameMode::Survival ? "survival"
                                                                  : "creative")
              << "\n";
    if (before.edits.empty() && countNonEmpty(before.inventory) == 0) {
        std::cerr << "FAIL: world loaded with no edits and an empty inventory — "
                     "the migration did not reproduce real content\n";
        return 1;
    }

    // 2. Save — rewrites the world to the current format 18.
    repository.save(before);
    {
        std::ifstream data{scratch / identifier / "world.dat", std::ios::binary};
        char magic[8];
        data.read(magic, 8);
        std::uint32_t format = 0;
        data.read(reinterpret_cast<char*>(&format), 4);
        if (format != 18U) {
            std::cerr << "FAIL: after save, world.dat header format=" << format
                      << ", expected 18\n";
            return 1;
        }
        std::cout << "  saved: world.dat header format is now " << format << "\n";
    }

    // 3. Reload and assert the round trip lost nothing.
    const auto after = repository.load(identifier);
    assert(after.hasPlayerPosition == before.hasPlayerPosition);
    assert(after.playerX == before.playerX);
    assert(after.playerY == before.playerY);
    assert(after.playerZ == before.playerZ);
    assert(after.hasSpawnPoint == before.hasSpawnPoint);
    if (before.hasSpawnPoint) {
        assert(after.spawnX == before.spawnX);
        assert(after.spawnY == before.spawnY);
        assert(after.spawnZ == before.spawnZ);
        assert(after.spawnYaw == before.spawnYaw);
    }
    // Format 13 split the single gameTimeSeconds into an integer serverTick plus
    // the named clocks, and format 17 stopped persisting gameTimeSeconds
    // altogether; reload re-derives it as serverTick / 20. So the exact float is
    // not round-tripped — the sun position is (carried by the Overworld clock),
    // which the clocks equality below verifies. Assert the documented recompute
    // and report the magnitude for the §8.2 record.
    std::cout << "  gameTimeSeconds: " << before.gameTimeSeconds << " -> "
              << after.gameTimeSeconds << " (expected " << (double(before.serverTick) /
              world::DayNightCycle::kTicksPerSecond) << ")\n";
    assert(after.gameTimeSeconds ==
           double(before.serverTick) / world::DayNightCycle::kTicksPerSecond);
    assert(after.gameMode == before.gameMode);
    assert(after.difficulty == before.difficulty);
    assert(after.selectedHotbarSlot == before.selectedHotbarSlot);
    assert(after.playerHealth == before.playerHealth);
    assert(after.playerFoodLevel == before.playerFoodLevel);
    assert(after.playerSaturation == before.playerSaturation);
    assert(after.playerAirTicks == before.playerAirTicks);
    assert(after.inventory == before.inventory);
    assert(editsEqual(after.edits, before.edits));
    std::cout << "  edits: " << before.edits.size()
              << " preserved (content + per-cell order)\n";
    assert(chestsEqual(after.chests, before.chests));
    assert(furnacesEqual(after.furnaces, before.furnaces));
    assert(entitiesEqual(after.entities, before.entities));
    assert(itemDropsEqual(after.itemDrops, before.itemDrops));
    assert(fallingBlocksEqual(after.fallingBlocks, before.fallingBlocks));
    assert(after.weather.raining == before.weather.raining);
    assert(after.weather.thundering == before.weather.thundering);
    assert(after.weather.rainTime == before.weather.rainTime);
    assert(after.weather.thunderTime == before.weather.thunderTime);
    assert(after.weather.clearWeatherTime == before.weather.clearWeatherTime);
    assert(after.serverTick == before.serverTick);
    assert(after.clocks == before.clocks);

    std::cout << "PASS: load -> save (format 18) -> reload preserved every field\n";
    return 0;
}
