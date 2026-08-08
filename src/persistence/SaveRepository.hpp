#pragma once

#include "gameplay/GameMode.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/PersistentBlockEdit.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::persistence {

struct SaveSummary final {
    std::string identifier;
    std::string displayName;
    std::uint64_t seed = 0U;
    std::int64_t lastPlayedUnixSeconds = 0;
};

struct SaveGame final {
    SaveSummary summary;
    bool hasPlayerPosition = false;
    float playerX = 0.0F;
    float playerY = 0.0F;
    float playerZ = 0.0F;
    // The player's personal spawn point, set by /spawnpoint the way 1.16.1 keeps
    // SpawnX/Y/Z on the player. Death respawns here before falling back to the
    // world spawn. Format 10 serialises it into its own self-describing block.
    bool hasSpawnPoint = false;
    float spawnX = 0.0F;
    float spawnY = 0.0F;
    float spawnZ = 0.0F;
    float spawnYaw = 0.0F;
    double gameTimeSeconds = 0.0;
    gameplay::GameMode gameMode = gameplay::GameMode::Creative;
    gameplay::Difficulty difficulty = gameplay::Difficulty::Normal;
    // Game rules travel with the world the way 1.16.1 keeps them in level.dat;
    // format 9 serialises them into a sparse, self-describing block.
    gameplay::GameRules gameRules;
    std::size_t selectedHotbarSlot = 0U;
    float playerHealth = gameplay::PlayerVitals::kMaximumHealth;
    std::int32_t playerFoodLevel = gameplay::PlayerVitals::kMaximumFood;
    float playerSaturation = 5.0F;
    std::int32_t playerAirTicks = gameplay::PlayerVitals::kMaximumAirTicks;
    std::array<gameplay::ItemStack, gameplay::Inventory::kSlotCount> inventory{};
    std::vector<world::PersistentBlockEdit> edits;
    std::vector<gameplay::ChestBlockEntity> chests;
    // The weather timers and flags, the way 1.16.1 keeps them in level.dat;
    // format 11 serialises them into their own self-describing block. A fresh
    // world defaults to a clear spell.
    gameplay::WeatherState weather;
};

class SaveRepository final {
  public:
    explicit SaveRepository(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::vector<SaveSummary> list() const;
    [[nodiscard]] SaveGame create(std::string displayName, std::uint64_t seed) const;
    [[nodiscard]] SaveGame load(const std::string& identifier) const;
    void save(SaveGame game) const;
    // Rename keeps the folder/identifier stable and rewrites only the display
    // name stored in level.properties, so an edited world keeps its identity.
    void rename(const std::string& identifier, std::string displayName) const;
    // Permanently removes the world directory and everything inside it.
    void remove(const std::string& identifier) const;

    [[nodiscard]] static std::string sanitizeDisplayName(std::string name);

  private:
    std::filesystem::path root_;
};

} // namespace mc::persistence
