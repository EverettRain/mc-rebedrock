#include "gameplay/DataPackStack.hpp"

#include "gameplay/BlockTags.hpp"
#include "gameplay/LootTable.hpp"
#include "gameplay/MobSpawnSettings.hpp"
#include "gameplay/RecipeTable.hpp"
#include "gameplay/entities/EntityAttributeOverlay.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mc::gameplay {

namespace {

[[nodiscard]] assets::PackMetadata readMetadata(const std::filesystem::path& mcmeta) {
    std::ifstream input{mcmeta, std::ios::binary};
    if (!input) {
        return {};
    }
    std::ostringstream text;
    text << input.rdbuf();
    try {
        return assets::PackMetadata::parse(text.str());
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

void PerSaveDataStack::reset() {
    manager_ = assets::PackManager{};
    providers_.clear();
    discoveredIds_.clear();
}

void PerSaveDataStack::scan(const std::filesystem::path& saveDirectory) {
    const auto datapacksRoot = saveDirectory / "datapacks";
    std::error_code error;
    if (!std::filesystem::is_directory(datapacksRoot, error)) {
        return; // no datapacks/ at all: zero packs discovered, the sparse default case
    }
    // Sorted by directory name so discovery — and therefore the default order a
    // caller with no persisted order would enable in — is stable across runs,
    // the same discipline Application.cpp's resourcepacks scan already uses.
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(datapacksRoot, error)) {
        if (error) {
            break;
        }
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "pack.mcmeta")) {
            entries.push_back(entry.path());
        }
    }
    std::ranges::sort(entries);
    for (const auto& path : entries) {
        const std::string id = path.filename().string();
        const auto metadata = readMetadata(path / "pack.mcmeta");
        providers_.emplace_back(path);
        std::error_code dataProbe;
        const bool hasData = std::filesystem::is_directory(path / "data", dataProbe);
        // A JE-shaped datapack has no assets/ half by convention (that is what
        // makes it a data pack rather than a resource pack), but a directory
        // that happens to ship both is still registered on the data stack only
        // — PerSaveDataStack never touches the resource stack (PACK REGULAR
        // #1: data packs are server-authoritative, per-save; resource packs
        // stay PACK-3's global client concern).
        manager_.registerPack(id, providers_.back(), metadata, hasData,
                              /*hasResourceHalf=*/false);
        discoveredIds_.push_back(id);
    }
}

void PerSaveDataStack::enable(const std::string& id) {
    if (manager_.find(id) == nullptr) {
        return; // stale persisted id whose folder is gone this session: skip, do not crash
    }
    manager_.enable(assets::PackStackKind::Data, id);
}

void PerSaveDataStack::disable(const std::string& id) {
    manager_.disable(assets::PackStackKind::Data, id);
}

std::vector<DiscoveredDataPack> PerSaveDataStack::list() const {
    const auto& order = manager_.order(assets::PackStackKind::Data);
    std::vector<DiscoveredDataPack> result;
    result.reserve(discoveredIds_.size());
    for (const auto& id : discoveredIds_) {
        const auto* record = manager_.find(id);
        DiscoveredDataPack entry;
        entry.id = id;
        entry.metadata = record != nullptr ? record->metadata : assets::PackMetadata{};
        entry.enabled = std::ranges::find(order, id) != order.end();
        result.push_back(std::move(entry));
    }
    return result;
}

const std::vector<std::string>& PerSaveDataStack::enabledOrder() const {
    return manager_.order(assets::PackStackKind::Data);
}

void PerSaveDataStack::rebuild(const assets::ResourceProvider& base) const {
    const assets::LayeredResourceProvider dataStack =
        manager_.buildProvider(assets::PackStackKind::Data, base);
    // Entity types must exist before entity_attributes/biome spawn tables name
    // a species (same ordering rule Application.cpp's loadDataPacks documents);
    // idempotent, so calling it again on a second rebuild() is harmless.
    entities::registerBuiltinEntities();
    entities::entityAttributeTable().load(dataStack);
    blockTags().load(dataStack);
    recipeTable().load(dataStack);
    lootTable().load(dataStack);
    biomeSpawnTables().load(dataStack);
}

void PerSaveDataStack::rebuildBuiltinOnly(const assets::ResourceProvider& base) {
    const assets::LayeredResourceProvider empty{base, {}};
    entities::registerBuiltinEntities();
    entities::entityAttributeTable().load(empty);
    blockTags().load(empty);
    recipeTable().load(empty);
    lootTable().load(empty);
    biomeSpawnTables().load(empty);
}

} // namespace mc::gameplay
