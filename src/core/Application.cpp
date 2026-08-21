#include "core/Application.hpp"

#include "assets/PackMetadata.hpp"
#include "assets/ResourceProvider.hpp"
#include "gameplay/BlockTags.hpp"
#include "gameplay/LootTable.hpp"
#include "gameplay/RecipeTable.hpp"
#include "gameplay/MobSpawnSettings.hpp"
#include "gameplay/entities/EntityAttributeOverlay.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "assets/ZipResourcePack.hpp"
#include "config/GameOptions.hpp"
#include "render/vulkan/VulkanRenderer.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>
#include <utility>

namespace mc {
namespace {

[[nodiscard]] int developmentLoadRadius(int configuredRadius) {
    constexpr int kMaximumRadius = 36;
    const char* value = std::getenv("MC_REBEDROCK_VIEW_DISTANCE");
    if (value == nullptr) {
        return configuredRadius;
    }

    const std::string_view text{value};
    int radius = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), radius);
    if (error != std::errc{} || end != text.data() + text.size() ||
        radius < 0 || radius > kMaximumRadius) {
        throw std::invalid_argument(
            "MC_REBEDROCK_VIEW_DISTANCE must be an integer between 0 and 36");
    }
    return radius;
}

} // namespace

Application::Application(
    std::filesystem::path resourceRoot,
    std::filesystem::path shaderRoot,
    std::filesystem::path configRoot,
    std::optional<render::TestSceneOptions> testScene)
    : resourceRoot_(std::move(resourceRoot)),
      shaderRoot_(std::move(shaderRoot)),
      configRoot_(std::move(configRoot)), testScene_(testScene) {}

int Application::run() {
    // One provider owns where every asset lives; the renderer and its texture
    // manager ask it for files instead of rebuilding the layout from path
    // arithmetic. The block-texture and sound roots below are derived from it too
    // rather than being spelled out a second time.
    const assets::DirectoryResourceProvider bundled{resourceRoot_};
    const auto vanillaRoot = bundled.vanillaRoot();

    std::cout << "MC Rebedrock Vulkan milestone\n";
    std::cout << "Chunk: " << world::kChunkWidth << 'x' << world::kChunkDepth << 'x'
              << world::kWorldHeight << "\n";
    std::cout << "Sections per chunk: " << world::kSectionCount << "\n";
    std::cout << "Sea level: " << world::kSeaLevel << "\n";
    std::cout << "Resources: " << vanillaRoot << "\n";

    if (!std::filesystem::is_directory(vanillaRoot)) {
        std::cerr << "Warning: extracted Minecraft 1.16.1 resources were not found.\n";
    }

    const auto optionsPath = configRoot_ / "options.properties";
    config::GameOptions options = config::GameOptions::load(optionsPath);
    const int loadRadius = testScene_.has_value()
        ? 0 : developmentLoadRadius(options.viewDistance);
    // Unload a couple of rings past the load radius so a player sitting on a
    // chunk boundary does not thrash the same ring load/unload (each unload is a
    // region write). See world::kUnloadHysteresisChunks.
    const int unloadRadius = loadRadius + world::kUnloadHysteresisChunks;
    world::ChunkStreamer chunkStreamer{
        0x5EEDULL,
        loadRadius,
        unloadRadius,
    };
    const int diameter = loadRadius * 2 + 1;
    std::cout << "Streaming window: " << diameter << 'x' << diameter
              << " chunks (load radius " << loadRadius
              << ", unload radius " << unloadRadius << ")\n";

    // User-imported standard resource packs sit in <game root>/resourcepacks,
    // next to bin/, config/ and saves/, the way vanilla keeps its folder. The
    // game root is the parent of config/ (saves/ lives there too). The folder is
    // created on first run so it is there to drop packs into, and its full path
    // is printed so it is easy to find. Each pack is a subdirectory holding
    // pack.mcmeta at its root; enabled packs override the bundled assets file by
    // file, and anything they omit still resolves from the built-in resources.
    const auto packDirectory = configRoot_.parent_path() / "resourcepacks";
    std::error_code packError;
    std::filesystem::create_directories(packDirectory, packError);
    std::cout << "Resource packs: " << std::filesystem::weakly_canonical(packDirectory, packError)
              << "\n";
    // A pack is either a directory holding pack.mcmeta at its root or a .zip
    // archive of the same shape. Collect both, sorted by name so the enable order
    // is stable.
    struct PackEntry final {
        std::filesystem::path path;
        bool isZip = false;
    };
    std::vector<PackEntry> found;
    if (std::filesystem::is_directory(packDirectory, packError)) {
        for (const auto& entry : std::filesystem::directory_iterator(packDirectory, packError)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "pack.mcmeta")) {
                found.push_back({entry.path(), false});
            } else if (entry.is_regular_file() && entry.path().extension() == ".zip") {
                found.push_back({entry.path(), true});
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const PackEntry& a, const PackEntry& b) { return a.path < b.path; });

    // Nothing in the game asks a provider for a filesystem path any more —
    // textures, fonts, sounds, tags and biome data all read bytes — so a zip
    // pack is consumed entirely from memory and this cache stays empty. The
    // path is still handed to the provider because `locate()` remains its
    // documented fallback, but a build that populates it has regressed.
    //
    // Older builds did extract here, so a stale cache is swept once at startup
    // rather than left to sit at a few hundred megabytes forever.
    const auto packCacheRoot = configRoot_.parent_path() / ".packcache";
    if (std::filesystem::exists(packCacheRoot)) {
        std::error_code cleanup;
        std::filesystem::remove_all(packCacheRoot, cleanup);
        if (!cleanup) {
            std::cout << "Removed the legacy resource-pack extraction cache at "
                      << packCacheRoot.string() << "\n";
        }
    }
    // A deque so pushing more providers (a pack's overlays) never invalidates the
    // pointers already handed to `enabled`.
    std::deque<assets::StandardPackResourceProvider> directoryPacks;
    std::deque<assets::ZipResourcePackProvider> zipPacks;
    // Enable order, each with the provider that serves it (lowest priority first
    // within a pack: main assets, then its version-gated overlays).
    std::vector<const assets::ResourceProvider*> enabled;
    // ReBedrock targets the 26.1 resource-pack format; overlays gated to it apply.
    constexpr int kTargetPackFormat = 84;
    const auto readMetadata = [](const std::filesystem::path& mcmeta) -> assets::PackMetadata {
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
    };
    for (const auto& entry : found) {
        if (entry.isZip) {
            zipPacks.emplace_back(entry.path, packCacheRoot / entry.path.stem());
            if (!zipPacks.back().valid()) {
                std::cerr << "Skipping unreadable resource pack: " << entry.path.filename().string()
                          << "\n";
                continue;
            }
            enabled.push_back(&zipPacks.back());
            std::cout << "Resource pack: " << entry.path.filename().string() << " (zip)\n";
        } else {
            directoryPacks.emplace_back(entry.path);
            enabled.push_back(&directoryPacks.back());
            std::cout << "Resource pack: " << entry.path.filename().string() << "\n";
            // pack.mcmeta overlays: version-gated subdirectories with their own
            // assets/ that override the main assets, last-declared highest. Each
            // becomes a provider stacked above the pack's main assets.
            const auto metadata = readMetadata(entry.path / "pack.mcmeta");
            for (const auto& overlay : metadata.overlays) {
                if (!overlay.appliesTo(kTargetPackFormat)) {
                    continue;
                }
                const auto overlayRoot = entry.path / overlay.directory;
                if (!std::filesystem::is_directory(overlayRoot, packError)) {
                    continue;
                }
                directoryPacks.emplace_back(overlayRoot);
                enabled.push_back(&directoryPacks.back());
                std::cout << "  overlay: " << overlay.directory << "\n";
            }
        }
    }

    // A resource pack is required: this build ships no Mojang assets of its own,
    // so without one there is nothing to draw or play. The folder was created
    // above; tell the player exactly where to drop a pack and stop rather than
    // launching into a world of missing-texture markers.
    if (enabled.empty()) {
        const auto where = std::filesystem::weakly_canonical(packDirectory, packError);
        std::cerr << "\n启动失败：缺少资源包 (Missing resource pack)\n"
                  << "请将一个标准资源包（目录或 .zip，含 pack.mcmeta 与 assets/）放入：\n  "
                  << where.string() << "\n"
                  << "然后重新启动。\n";
        return 1;
    }

    // Highest priority first for the resolver: the last-listed pack wins, so it
    // is searched before the earlier ones and before the bundled base.
    std::vector<const assets::ResourceProvider*> overlays{enabled.rbegin(), enabled.rend()};
    const assets::LayeredResourceProvider resources{bundled, overlays};

    // Block tags come from the `data/` half of the pack stack, the same way
    // textures come from `assets/`. An ordinary resource pack ships no `data/`
    // at all, so this usually keeps the compiled-in 26.1 defaults; a full data
    // pack overrides them per tag.
    // The species registry has to exist before anything that names species is
    // built. The biome spawn tables below resolve pig/cow/zombie through it, and
    // the renderer's own call comes later — so loading them first produced
    // tables with nothing in them and a world that never spawned a mob.
    // Registration is idempotent, so the renderer's call stays harmless.
    gameplay::entities::registerBuiltinEntities();
    // Entity attribute overrides load the same two-layer way: each species keeps
    // its compiled-in floor, and a datapack that ships
    // `data/<space>/entity_attributes/<species>.json` overrides the attributes it
    // lists. No `data/` keeps every species' built-in numbers.
    gameplay::entities::entityAttributeTable().load(resources);
    gameplay::blockTags().load(resources);
    // Recipes load the same way: the baked built-in floor first, then any recipes
    // a datapack supplies. An ordinary resource pack ships no `data/`, so this
    // usually keeps the compiled-in recipe set rather than emptying the crafting
    // table.
    gameplay::recipeTable().load(resources);
    // Block loot the same way: the baked drop floor, then any block loot tables a
    // datapack supplies; no `data/` keeps the built-in drops.
    gameplay::lootTable().load(resources);
    // The biome spawn tables come from the same `data/` half, and follow the
    // same rule: an ordinary resource pack ships no `data/`, so this usually
    // keeps the compiled-in 26.1 numbers rather than leaving the world empty.
    gameplay::biomeSpawnTables().load(resources);

    render::VulkanRenderer renderer{
        shaderRoot_,
        resources,
        chunkStreamer,
        options,
        optionsPath,
        configRoot_.parent_path() / "saves",
        testScene_};
    renderer.run();
    chunkStreamer.stop();

    return 0;
}

} // namespace mc
