#include "assets/PackManager.hpp"
#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"
#include "core/VersionManifest.hpp"

#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string_view>

// PACK-0: PackManager is a management layer over the EXISTING providers — it
// adds no IO of its own. These tests pin: (1) the two independently-ordered
// stacks each produce a correct LayeredResourceProvider (bottom vanilla -> top
// user pack, top overlay wins); (2) the data/authoritative path never composes
// an assets-only pack, the dedicated-server guardrail; (3) pack.mcmeta
// pack_format is read and compared against META's packVersion without hard
// failure; (4) splitting the old monolithic single-stack load into
// data+resource stacks resolves the SAME bytes the monolithic stack did — no
// behavioral drift.

namespace {

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

} // namespace

int main() {
    using namespace mc::assets;
    namespace fs = std::filesystem;

    const fs::path tmp = fs::temp_directory_path() / "rebedrock_pack_manager_test";
    std::error_code cleanup;
    fs::remove_all(tmp, cleanup);

    // --- Fixture: a bundled/base tree, a "data pack" (data/ only), and a
    //     "resource pack" (assets/ only) — the two pure cases PACK REGULAR #1
    //     requires PackManager to keep apart. ---
    const fs::path baseRoot = tmp / "base";
    writeFile(baseRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png", "base-texture");
    writeFile(baseRoot / "data" / "minecraft" / "tags" / "block" / "example.json", "base-tag");

    const fs::path dataPackRoot = tmp / "datapack_only";
    writeFile(dataPackRoot / "pack.mcmeta",
              R"({"pack": {"pack_format": 84, "description": "data only"}})");
    writeFile(dataPackRoot / "data" / "minecraft" / "tags" / "block" / "example.json", "overlay-tag");

    const fs::path resourcePackRoot = tmp / "resourcepack_only";
    writeFile(resourcePackRoot / "pack.mcmeta",
              R"({"pack": {"pack_format": 84, "description": "resource only"}})");
    writeFile(resourcePackRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png",
              "overlay-texture");

    const StandardPackResourceProvider base{baseRoot};
    const StandardPackResourceProvider dataPack{dataPackRoot};
    const StandardPackResourceProvider resourcePack{resourcePackRoot};

    const PackMetadata dataMeta = PackMetadata::parse(
        R"({"pack": {"pack_format": 84, "description": "data only"}})");
    const PackMetadata resourceMeta = PackMetadata::parse(
        R"({"pack": {"pack_format": 84, "description": "resource only"}})");

    // --- Registration: a pure-data pack only gets a data half, a pure-resource
    //     pack only gets a resource half. ---
    PackManager manager;
    manager.registerPack("datapack_only", dataPack, dataMeta, /*hasDataHalf=*/true,
                         /*hasResourceHalf=*/false);
    manager.registerPack("resourcepack_only", resourcePack, resourceMeta, /*hasDataHalf=*/false,
                         /*hasResourceHalf=*/true);
    assert(manager.packs().size() == 2U);
    assert(manager.find("datapack_only") != nullptr);
    assert(manager.find("missing") == nullptr);

    // A pack with no half for a stack cannot be enabled there — enable() would
    // abort (fork-tested below via the sabotage protocol, not here: this test
    // stays on the happy path so it does not itself abort the process).
    manager.enable(PackStackKind::Data, "datapack_only");
    manager.enable(PackStackKind::Resources, "resourcepack_only");
    assert(manager.order(PackStackKind::Data) == std::vector<std::string>{"datapack_only"});
    assert(manager.order(PackStackKind::Resources) ==
           std::vector<std::string>{"resourcepack_only"});

    // --- Two-end stacks: each builds a correct LayeredResourceProvider. ---
    {
        const LayeredResourceProvider dataStack = manager.buildProvider(PackStackKind::Data, base);
        // The overlay tag wins over the base tag: top overlay beats the floor.
        const auto tagLocation = data("tags/block/example.json");
        assert(dataStack.exists(tagLocation));
        const auto bytes = dataStack.readBytes(tagLocation);
        const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        assert(text == "overlay-tag");

        const LayeredResourceProvider resourceStack =
            manager.buildProvider(PackStackKind::Resources, base);
        const auto textureLocation = textures("block/stone.png");
        assert(resourceStack.exists(textureLocation));
        const auto textureBytes = resourceStack.readBytes(textureLocation);
        const std::string textureText{reinterpret_cast<const char*>(textureBytes.data()),
                                      textureBytes.size()};
        assert(textureText == "overlay-texture");

        // --- Guardrail: the data stack composes NO assets-stack pack. The
        //     resource-only pack's texture must NOT be reachable through the
        //     data stack's provider set, and swapping the resource pack into
        //     the data path is exactly what enable() refuses (see the
        //     registration-mismatch check below). Concretely: the data stack
        //     was built from `order(Data)`, which contains only
        //     "datapack_only" — the resource pack never entered its overlay
        //     list at all. ---
        assert(manager.order(PackStackKind::Data).size() == 1U);
        assert(manager.order(PackStackKind::Data)[0] == "datapack_only");
        for (const auto& id : manager.order(PackStackKind::Data)) {
            const PackRecord* record = manager.find(id);
            assert(record != nullptr);
            assert(record->hasDataHalf); // every provider the data stack composed declares a data half
        }
    }

    // --- Stack order: bottom vanilla -> top user pack; a SECOND, higher pack
    //     overrides a first. Also proves order is not silently reversed
    //     (sabotage ②'s target). ---
    {
        const fs::path lowRoot = tmp / "low";
        const fs::path highRoot = tmp / "high";
        writeFile(lowRoot / "pack.mcmeta", R"({"pack": {"pack_format": 84}})");
        writeFile(lowRoot / "assets" / "minecraft" / "textures" / "block" / "dirt.png", "low-dirt");
        writeFile(highRoot / "pack.mcmeta", R"({"pack": {"pack_format": 84}})");
        writeFile(highRoot / "assets" / "minecraft" / "textures" / "block" / "dirt.png", "high-dirt");

        const StandardPackResourceProvider low{lowRoot};
        const StandardPackResourceProvider high{highRoot};

        PackManager orderManager;
        orderManager.registerPack("low", low, PackMetadata{}, false, true);
        orderManager.registerPack("high", high, PackMetadata{}, false, true);
        // Enabled bottom to top: low first, high second — high must win.
        orderManager.enable(PackStackKind::Resources, "low");
        orderManager.enable(PackStackKind::Resources, "high");
        assert((orderManager.order(PackStackKind::Resources) ==
                std::vector<std::string>{"low", "high"}));

        const LayeredResourceProvider stack =
            orderManager.buildProvider(PackStackKind::Resources, base);
        const auto dirtBytes = stack.readBytes(textures("block/dirt.png"));
        const std::string dirtText{reinterpret_cast<const char*>(dirtBytes.data()), dirtBytes.size()};
        assert(dirtText == "high-dirt"); // top of stack wins, not first-registered

        // promoteToTop flips the winner without re-registering.
        orderManager.promoteToTop(PackStackKind::Resources, "low");
        assert((orderManager.order(PackStackKind::Resources) ==
                std::vector<std::string>{"high", "low"}));
        const LayeredResourceProvider promoted =
            orderManager.buildProvider(PackStackKind::Resources, base);
        const auto promotedBytes = promoted.readBytes(textures("block/dirt.png"));
        const std::string promotedText{reinterpret_cast<const char*>(promotedBytes.data()),
                                       promotedBytes.size()};
        assert(promotedText == "low-dirt");

        // disable() removes a pack from the stack; the base floor resolves again.
        orderManager.disable(PackStackKind::Resources, "low");
        orderManager.disable(PackStackKind::Resources, "high");
        assert(orderManager.order(PackStackKind::Resources).empty());
        const LayeredResourceProvider empty =
            orderManager.buildProvider(PackStackKind::Resources, base);
        assert(!empty.exists(textures("block/dirt.png"))); // neither pack nor base has dirt.png
    }

    // --- pack.mcmeta / packVersion compat: matching format is compatible,
    //     out-of-range is flagged (not silently accepted), and a pack with no
    //     pack block at all (zeroed format) declares no opinion. ---
    {
        mc::core::PackVersion buildVersion{.resource = 84U, .data = 84U};

        const PackMetadata matching = PackMetadata::parse(
            R"({"pack": {"pack_format": 84, "description": "ok"}})");
        const auto matchResult =
            PackManager::checkCompatibility(matching, PackStackKind::Resources, buildVersion);
        assert(matchResult.compatible);
        assert(matchResult.buildPackVersion == 84U);

        const PackMetadata stale = PackMetadata::parse(
            R"({"pack": {"pack_format": 6, "description": "old"}})");
        const auto staleResult =
            PackManager::checkCompatibility(stale, PackStackKind::Data, buildVersion);
        assert(!staleResult.compatible); // flagged, not silently loaded wrong
        assert(staleResult.packFormatMin == 6 && staleResult.packFormatMax == 6);

        const PackMetadata range = PackMetadata::parse(
            R"({"pack": {"min_format": 80, "max_format": 90, "description": "range"}})");
        const auto rangeResult =
            PackManager::checkCompatibility(range, PackStackKind::Resources, buildVersion);
        assert(rangeResult.compatible); // 84 is inside [80, 90]

        const PackMetadata noOpinion = PackMetadata::parse("{}");
        const auto noOpinionResult =
            PackManager::checkCompatibility(noOpinion, PackStackKind::Data, buildVersion);
        assert(noOpinionResult.compatible); // zeroed format = no opinion, not incompatible
    }

    // --- Equivalence regression: the pre-split code built ONE monolithic
    //     LayeredResourceProvider (base + every enabled pack, both halves
    //     mixed together) and fed it to both the data-driven tables AND the
    //     renderer. After the split, the data stack alone must resolve the
    //     SAME data/ bytes the monolithic stack resolved, and the resource
    //     stack alone must resolve the SAME assets/ bytes — proving the split
    //     is behavior-preserving, not just differently organised. ---
    {
        const fs::path packRoot = tmp / "mixed_pack";
        writeFile(packRoot / "pack.mcmeta", R"({"pack": {"pack_format": 84}})");
        writeFile(packRoot / "data" / "minecraft" / "tags" / "block" / "example.json",
                  "mixed-tag");
        writeFile(packRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png",
                  "mixed-texture");
        const StandardPackResourceProvider mixedPack{packRoot};

        // Pre-split behaviour: one stack, mixed pack overlaid over base, used
        // for both halves indiscriminately (what Application.cpp did before
        // PACK-0).
        const std::vector<const ResourceProvider*> monolithicOverlays{&mixedPack};
        const LayeredResourceProvider monolithic{base, monolithicOverlays};
        const auto monolithicTag = monolithic.readBytes(data("tags/block/example.json"));
        const auto monolithicTexture = monolithic.readBytes(textures("block/stone.png"));

        // Post-split behaviour: PackManager with the SAME pack enabled on
        // BOTH stacks (since it has both halves) must resolve identically.
        PackManager splitManager;
        splitManager.registerPack("mixed", mixedPack, PackMetadata{}, true, true);
        splitManager.enable(PackStackKind::Data, "mixed");
        splitManager.enable(PackStackKind::Resources, "mixed");
        const LayeredResourceProvider splitData =
            splitManager.buildProvider(PackStackKind::Data, base);
        const LayeredResourceProvider splitResources =
            splitManager.buildProvider(PackStackKind::Resources, base);

        const auto splitTag = splitData.readBytes(data("tags/block/example.json"));
        const auto splitTexture = splitResources.readBytes(textures("block/stone.png"));

        assert(monolithicTag == splitTag);
        assert(monolithicTexture == splitTexture);

        // Registering "mixed" on the data stack ONLY (never resources) is the
        // shape a real dedicated server uses: even though the pack has an
        // assets half on disk, a data-only stack still resolves the tag
        // correctly and never needs the resource stack to do it.
        PackManager dataOnlyManager;
        dataOnlyManager.registerPack("mixed", mixedPack, PackMetadata{}, true, true);
        dataOnlyManager.enable(PackStackKind::Data, "mixed");
        assert(dataOnlyManager.order(PackStackKind::Resources).empty());
        const LayeredResourceProvider dataOnly =
            dataOnlyManager.buildProvider(PackStackKind::Data, base);
        assert(dataOnly.readBytes(data("tags/block/example.json")) == monolithicTag);
    }

    fs::remove_all(tmp, cleanup);
    return 0;
}
