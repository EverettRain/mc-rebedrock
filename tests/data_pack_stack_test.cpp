#include "gameplay/DataPackStack.hpp"

#include "assets/ResourceProvider.hpp"
#include "gameplay/BlockTags.hpp"
#include "gameplay/RecipeTable.hpp"
#include "persistence/SaveRepository.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string_view>

// PACK-1: PerSaveDataStack scans <save>/datapacks/*, rebuild() applies the
// stack to the five data-driven gameplay tables, and SaveRepository's DPKS
// block persists which packs are enabled and in what order. These tests pin
// the card's three acceptance points end to end:
//  1. Per-save isolation: a datapack that overrides a recipe/tag is visible
//     only when its own save's stack is rebuilt from — a second save with no
//     such pack (or the same pack disabled) sees the built-in floor.
//  2. Enable/order persistence round-trips through world.dat's DPKS block,
//     sparsely: an old SaveGame with no enabledDataPacks set (the zero value
//     every pre-PACK-1 struct read leaves it at) loads as "nothing enabled",
//     never a crash.
//  3. Rebuild leaves no residue: rebuilding from stack A then stack B (or the
//     built-in floor) produces exactly B's (or the floor's) tables, not a
//     union of A and B.

namespace {

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

// A trivial base with no data/ or assets/ at all — just enough for
// LayeredResourceProvider's `base` slot. Every test pack overrides on top of
// it, so overlay visibility never depends on bundled content being present in
// this test's temp directory.
[[nodiscard]] std::filesystem::path makeEmptyBase(const std::filesystem::path& root) {
    const auto base = root / "base";
    std::filesystem::create_directories(base);
    return base;
}

// A minimal datapack: pack.mcmeta plus a `mineable/pickaxe` tag override
// (proves via BlockTagTable::dataDriven — the same fixture shape
// pack_manager_test.cpp already uses) and a oak_planks recipe override that
// bumps its crafted count from the baked 4 to 9 (proves via
// RecipeTable::crafting(), the card's literal "override a recipe" example).
void writeOverridePack(const std::filesystem::path& packRoot, std::uint8_t plankCount) {
    writeFile(packRoot / "pack.mcmeta",
              R"({"pack": {"pack_format": 84, "description": "test overrides"}})");
    writeFile(packRoot / "data" / "minecraft" / "tags" / "block" / "mineable" / "pickaxe.json",
              R"({"replace": true, "values": []})");
    const std::string recipe = R"({"width":1,"height":1,"shapeless":true,)"
                               R"("ingredients":[{"block":"rebedrock:oak_log"}],)"
                               R"("output":"rebedrock:oak_planks","count":)" +
                               std::to_string(static_cast<int>(plankCount)) + "}";
    writeFile(packRoot / "data" / "minecraft" / "recipes" / "oak_planks.json", recipe);
}

[[nodiscard]] std::uint8_t plankCount() {
    for (const auto& recipe : mc::gameplay::recipeTable().crafting()) {
        if (recipe.identifier == "minecraft:oak_planks") {
            return recipe.output.count;
        }
    }
    return 0U;
}

}  // namespace

int main() {
    using namespace mc;
    namespace fs = std::filesystem;

    const fs::path tmp = fs::temp_directory_path() / "rebedrock_data_pack_stack_test";
    std::error_code cleanup;
    fs::remove_all(tmp, cleanup);

    const auto base = makeEmptyBase(tmp);
    const assets::DirectoryResourceProvider baseProvider{base};

    // --- Fixture: two saves. Save A carries an override pack (enabled); save
    //     B carries none at all. ---
    const auto saveA = tmp / "saves" / "save-a";
    const auto saveB = tmp / "saves" / "save-b";
    writeOverridePack(saveA / "datapacks" / "overrides", /*plankCount=*/9U);
    fs::create_directories(saveB / "datapacks");  // exists but empty: zero packs discovered

    // --- 1. Built-in floor sanity: before any save is scanned, the tables read
    //     as "no pack supplied this tag" and the baked plank count (4). ---
    gameplay::PerSaveDataStack::rebuildBuiltinOnly(baseProvider);
    assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
    assert(plankCount() == 4U);

    // --- 2. Save A, override pack discovered but NOT yet enabled: rebuild
    //     still shows the built-in floor (discovery alone must not activate a
    //     pack — enable() is the only door). ---
    {
        gameplay::PerSaveDataStack stack;
        stack.scan(saveA);
        const auto listing = stack.list();
        assert(listing.size() == 1U);
        assert(listing[0].id == "overrides");
        assert(!listing[0].enabled);
        stack.rebuild(baseProvider);
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(plankCount() == 4U);

        // --- 3. Enabling it and rebuilding shows the override — per-save
        //     isolation's positive case. ---
        stack.enable("overrides");
        assert(stack.enabledOrder().size() == 1U);
        assert(stack.enabledOrder()[0] == "overrides");
        stack.rebuild(baseProvider);
        assert(gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(plankCount() == 9U);

        // --- 4. Save B, scanned and rebuilt from a FRESH stack instance:
        //     shows the built-in floor even though save A (a different
        //     PerSaveDataStack, but the same process-wide table singletons)
        //     just rebuilt with its override active. This is the "no residue
        //     across saves" property GameRuntime's loadWorld/unloadWorld
        //     relies on — each rebuild() call fully replaces the previous
        //     call's contribution because the five tables' own load()
        //     clears before applying. ---
        gameplay::PerSaveDataStack stackB;
        stackB.scan(saveB);
        assert(stackB.list().empty());
        stackB.rebuild(baseProvider);
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(plankCount() == 4U);

        // --- 5. Disabling save A's pack and rebuilding A's stack again also
        //     falls back to the floor — enable/disable is live, not sticky. ---
        stack.disable("overrides");
        assert(stack.enabledOrder().empty());
        stack.rebuild(baseProvider);
        assert(!gameplay::blockTags().dataDriven(gameplay::BlockTag::MineableWithPickaxe));
        assert(plankCount() == 4U);

        // --- 6. reset() drops discovery too (GameRuntime calls this between
        //     loadWorld calls so a stale id from a previous save's now-gone
        //     folder is never dereferenced). ---
        stack.reset();
        assert(stack.list().empty());
        assert(!stack.contains("overrides"));
    }

    // --- 7. An id enable() cannot find (never scanned, or scanned then
    //     reset) is silently skipped rather than aborting — a save whose
    //     persisted DPKS block names a pack folder that is gone this session
    //     must still open. ---
    {
        gameplay::PerSaveDataStack stack;
        stack.scan(saveB);  // discovers nothing
        stack.enable("ghost-pack");
        assert(stack.enabledOrder().empty());
    }

    // --- SaveRepository DPKS persistence -------------------------------------
    const auto saveRoot = tmp / "repo";
    persistence::SaveRepository repository{saveRoot};
    persistence::SaveGame game = repository.create("dpks-world", 42ULL);
    assert(game.enabledDataPacks.empty());  // a fresh save starts with none enabled
    game.enabledDataPacks = {"alpha", "beta"};
    repository.save(game);
    const auto reloaded = repository.load(game.summary.identifier);
    assert(reloaded.enabledDataPacks.size() == 2U);
    assert(reloaded.enabledDataPacks[0] == "alpha");
    assert(reloaded.enabledDataPacks[1] == "beta");

    // A save that predates DPKS (an in-memory SaveGame nobody ever set
    // enabledDataPacks on, written and reloaded) round-trips as empty — the
    // all-built-in sparse default, not a crash. SaveGame's own zero-init
    // already gives this for a freshly created save; this pins that save()/
    // load() do not require the field to be populated.
    persistence::SaveGame bareGame = repository.create("bare-world", 7ULL);
    assert(bareGame.enabledDataPacks.empty());
    repository.save(bareGame);
    const auto bareReloaded = repository.load(bareGame.summary.identifier);
    assert(bareReloaded.enabledDataPacks.empty());

    // A genuinely pre-PACK-1 world.dat: take a real save() output (which
    // carries a DPKS block, even if empty) and physically excise those bytes
    // — reproducing exactly what an old build's writer produced, since DPKS
    // did not exist before this card. The checksum trailer (FNV-1a over
    // everything before it, SaveRepository's own `checksum()`) is recomputed
    // the same way so the patched file still passes SaveRepository's own
    // integrity check; only DPKS's absence is being simulated, not corruption.
    // Must load as all-built-in (empty enabledDataPacks), never throw — the
    // sparse "old world.dat loads all-built-in default, no crash" point.
    {
        persistence::SaveGame oldGame = repository.create("old-format-world", 9ULL);
        oldGame.enabledDataPacks = {"would-not-exist-on-an-old-build"};
        repository.save(oldGame);
        const auto worldDatPath = saveRoot / oldGame.summary.identifier / "world.dat";

        std::vector<std::uint8_t> bytes;
        {
            std::ifstream input{worldDatPath, std::ios::binary | std::ios::ate};
            assert(input.is_open());
            const auto length = input.tellg();
            bytes.resize(static_cast<std::size_t>(length));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), length);
        }

        // The format-version u32 sits immediately after the 8-byte magic.
        // Dial it back to 18 (one below the current 19, still inside the
        // owner-block era >= 17) so this file reads as genuinely pre-PACK-1,
        // not merely "current format with DPKS bytes removed" (which the
        // reader would never actually see from a real old build).
        assert(bytes.size() >= 12U);
        bytes[8] = 18U;
        bytes[9] = 0U;
        bytes[10] = 0U;
        bytes[11] = 0U;

        // Find the DPKS tag ('D','P','K','S' little-endian) and cut its whole
        // framed block [tag..tag+blockSize) out of the byte stream.
        constexpr std::uint32_t kDpksTag =
            'D' | ('P' << 8) | ('K' << 16) | ('S' << 24);
        std::size_t tagOffset = bytes.size();
        for (std::size_t index = 0; index + 4U <= bytes.size(); ++index) {
            const std::uint32_t candidate =
                static_cast<std::uint32_t>(bytes[index]) |
                (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U) |
                (static_cast<std::uint32_t>(bytes[index + 2U]) << 16U) |
                (static_cast<std::uint32_t>(bytes[index + 3U]) << 24U);
            if (candidate == kDpksTag) {
                tagOffset = index;
                break;
            }
        }
        assert(tagOffset != bytes.size());
        const std::uint32_t blockSize =
            static_cast<std::uint32_t>(bytes[tagOffset + 4U]) |
            (static_cast<std::uint32_t>(bytes[tagOffset + 5U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[tagOffset + 6U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[tagOffset + 7U]) << 24U);
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(tagOffset),
                   bytes.begin() + static_cast<std::ptrdiff_t>(tagOffset + blockSize));

        // Recompute the FNV-1a trailer over everything but itself, mirroring
        // SaveRepository's private checksum().
        constexpr std::size_t kChecksumBytes = 8U;
        assert(bytes.size() >= kChecksumBytes);
        const std::size_t payloadSize = bytes.size() - kChecksumBytes;
        std::uint64_t hash = 1469598103934665603ULL;
        for (std::size_t index = 0; index < payloadSize; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
        for (std::size_t offset = 0; offset < kChecksumBytes; ++offset) {
            bytes[payloadSize + offset] = static_cast<std::uint8_t>(hash >> (offset * 8U));
        }

        {
            std::ofstream output{worldDatPath, std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        }

        const auto patchedReload = repository.load(oldGame.summary.identifier);
        assert(patchedReload.enabledDataPacks.empty());
    }

    // Disabling (persisting an empty list after having enabled something)
    // round-trips too — the "disable a pack persists across save/reload"
    // acceptance point.
    game.enabledDataPacks.clear();
    repository.save(game);
    const auto disabledReload = repository.load(game.summary.identifier);
    assert(disabledReload.enabledDataPacks.empty());

    return 0;
}
