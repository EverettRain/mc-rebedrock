// PACK: a single combined pack (assets/ + data/) feeding both stacks.
//
// A user should be able to drop *one* pack — their own extracted Minecraft, which
// carries both halves — into resourcepacks/ and have it supply textures (client
// assets/) AND server data (data/: structures, loot, recipes). The mechanism this
// pins: a pack registered and enabled in the RESOURCE stack still serves its
// ServerData half when the resulting resource-stack provider is queried for data,
// because a pack provider maps a query to assets/ or data/ by its PackType. That
// resource stack is what the integrated (single-player) runtime hands its data
// tables as their base (VulkanRenderer passes it as GameRuntime's dataBase), so
// the combined pack's data loads without the user importing a second pack.
//
// The counterpart guarantee (kept correct by construction, asserted here too): a
// *pure* base with no packs surfaces no such data — the shape the dedicated server
// uses (DirectoryResourceProvider base, no resource stack), so a client resource
// pack never injects server data into a dedicated server.

#include "assets/PackManager.hpp"
#include "assets/ResourceProvider.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::assets::PackManager;
using mc::assets::PackMetadata;
using mc::assets::PackStackKind;
using mc::assets::PackType;
using mc::assets::ResourceLocation;

// A provider standing in for a combined pack root: it answers ClientResources
// queries out of an "assets" map and ServerData queries out of a "data" map, the
// way StandardPackResourceProvider maps a PackType to assets/ vs data/.
class DualHalfProvider final : public mc::assets::ResourceProvider {
  public:
    void addAsset(std::string path, std::string body) {
        assets_[path] = std::move(body);
    }
    void addData(std::string path, std::string body) {
        data_[path] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const ResourceLocation&) const override { return {}; }
    [[nodiscard]] bool exists(const ResourceLocation& location) const override {
        return map(location.type).count(location.path) != 0;
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte> readBytes(const ResourceLocation& location) const override {
        const auto& m = map(location.type);
        const auto slot = m.find(location.path);
        if (slot == m.end()) return {};
        std::vector<std::byte> bytes(slot->second.size());
        std::memcpy(bytes.data(), slot->second.data(), slot->second.size());
        return bytes;
    }
    [[nodiscard]] std::vector<ResourceLocation>
    list(std::string_view space, std::string_view prefix, PackType type) const override {
        std::vector<ResourceLocation> found;
        for (const auto& [path, body] : map(type)) {
            (void)body;
            if (path.size() >= prefix.size() && std::string_view{path}.substr(0, prefix.size()) == prefix) {
                found.push_back(ResourceLocation{std::string{space}, path, type});
            }
        }
        return found;
    }

  private:
    [[nodiscard]] const std::map<std::string, std::string>& map(PackType type) const {
        return type == PackType::ServerData ? data_ : assets_;
    }
    std::map<std::string, std::string> assets_;
    std::map<std::string, std::string> data_;
};

// An empty base that serves nothing — the "pure base" a dedicated server layers
// per-save datapacks over.
class EmptyProvider final : public mc::assets::ResourceProvider {
  public:
    [[nodiscard]] std::filesystem::path locate(const ResourceLocation&) const override { return {}; }
    [[nodiscard]] bool exists(const ResourceLocation&) const override { return false; }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte> readBytes(const ResourceLocation&) const override { return {}; }
    [[nodiscard]] std::vector<ResourceLocation>
    list(std::string_view, std::string_view, PackType) const override { return {}; }
};

std::string bytesToString(const std::vector<std::byte>& bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

int main() {
    EmptyProvider base;

    DualHalfProvider pack;
    pack.addAsset("textures/block/stone.png", "PNG");
    pack.addData("structures/igloo/top.nbt", "NBT");
    pack.addData("loot_tables/chests/igloo_chest.json", "LOOT");

    // Register + enable the pack in the RESOURCE stack only, exactly as
    // Application does for a resourcepacks/ entry.
    PackManager manager;
    manager.registerPack("pack0", pack, PackMetadata{}, /*hasDataHalf=*/false, /*hasResourceHalf=*/true);
    manager.enable(PackStackKind::Resources, "pack0");
    const auto resourceStack = manager.buildProvider(PackStackKind::Resources, base);

    // The resource-stack provider (what the integrated runtime uses as its data
    // base) surfaces BOTH halves of the combined pack.
    assert(bytesToString(resourceStack.readBytes(
               {"minecraft", "textures/block/stone.png", PackType::ClientResources})) == "PNG");
    assert(bytesToString(resourceStack.readBytes(
               {"minecraft", "structures/igloo/top.nbt", PackType::ServerData})) == "NBT");
    assert(bytesToString(resourceStack.readBytes(
               {"minecraft", "loot_tables/chests/igloo_chest.json", PackType::ServerData})) == "LOOT");

    // list() for the data half reaches the pack too (what structureManager().load
    // and chestLootTable().load enumerate).
    const auto structures = resourceStack.list("minecraft", "structures", PackType::ServerData);
    assert(structures.size() == 1 && structures[0].path == "structures/igloo/top.nbt");

    // Isolation: a pure base with no packs surfaces none of the data — a client
    // resource pack can never inject server data into a dedicated server, which
    // layers per-save datapacks over exactly this kind of bare base.
    assert(base.readBytes({"minecraft", "structures/igloo/top.nbt", PackType::ServerData}).empty());
    assert(base.list("minecraft", "structures", PackType::ServerData).empty());

    return 0;
}
