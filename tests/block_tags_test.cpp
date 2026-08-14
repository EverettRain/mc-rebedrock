#include "assets/ResourceProvider.hpp"
#include "gameplay/BlockTags.hpp"
#include "world/Block.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

// Block tags come from the standard 26.1 data pack rather than from a switch
// chain in the C++ source. What is pinned here is the loading contract: the
// `data/` half of a pack is a different root from `assets/`, `#tag` references
// expand recursively, packs merge low-to-high with `replace` truncating, and
// identifiers this build has no block for are skipped rather than fatal.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"block_tags_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::BlockTag;
using mc::world::Block;

void writeTag(const std::filesystem::path& packRoot, std::string_view name,
              std::string_view json) {
    const auto path = packRoot / "data" / "minecraft" / "tags" / "block" / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    REQUIRE(static_cast<bool>(out));
    out << json;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace mc;

    const fs::path root = fs::temp_directory_path() / "block_tags_test";
    fs::remove_all(root);
    const fs::path packRoot = root / "pack";
    const fs::path overlayRoot = root / "overlay";
    fs::create_directories(packRoot);
    fs::create_directories(overlayRoot);

    // --- A data resource resolves under `data/`, not `assets/`. Getting this
    // wrong resolves nothing at all and every block reads as untagged. ---
    {
        const auto location = assets::data("tags/block/leaves.json");
        REQUIRE(location.type == assets::PackType::ServerData);
        assets::StandardPackResourceProvider pack{packRoot};
        const auto resolved = pack.locate(location);
        REQUIRE(resolved.string().find("data") != std::string::npos);
        REQUIRE(resolved.string().find("assets") == std::string::npos);
        // A client location for the same path stays under assets/.
        REQUIRE(pack.locate(assets::textures("block/stone.png")).string().find("assets") !=
                std::string::npos);
    }

    // --- The real shapes vanilla ships: a flat list, and a tag that is nothing
    // but references to other tags (26.1's `logs` is exactly this). ---
    {
        writeTag(packRoot, "mineable/pickaxe.json",
                 R"({"values": ["minecraft:stone", "minecraft:iron_ore",
                                "minecraft:nonexistent_block_from_a_later_version"]})");
        writeTag(packRoot, "needs_stone_tool.json", R"({"values": ["minecraft:iron_ore"]})");
        writeTag(packRoot, "mineable/axe.json", R"({"values": ["#minecraft:logs"]})");
        writeTag(packRoot, "logs.json", R"({"values": ["#minecraft:oak_logs"]})");
        writeTag(packRoot, "oak_logs.json", R"({"values": ["minecraft:oak_log"]})");
        writeTag(packRoot, "leaves.json", R"({"values": ["minecraft:oak_leaves"]})");

        assets::StandardPackResourceProvider pack{packRoot};
        mc::gameplay::BlockTagTable tags;
        tags.load(pack);

        REQUIRE(tags.dataDriven(BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::Stone, BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::IronOre, BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::IronOre, BlockTag::NeedsStoneTool));
        // Stone is mineable but needs no tier: the two tags are independent.
        REQUIRE(!tags.has(Block::Stone, BlockTag::NeedsStoneTool));
        // Two levels of `#` indirection resolved to a real block.
        REQUIRE(tags.has(Block::OakLog, BlockTag::MineableWithAxe));
        REQUIRE(tags.has(Block::OakLeaves, BlockTag::Leaves));
        // A block in no tag stays clean.
        REQUIRE(!tags.has(Block::Dirt, BlockTag::MineableWithPickaxe));
        REQUIRE(!tags.has(Block::Stone, BlockTag::MineableWithAxe));
    }

    // --- A cycle terminates instead of hanging. A broken pack must not be able
    // to freeze startup. ---
    {
        const fs::path cycleRoot = root / "cycle";
        fs::create_directories(cycleRoot);
        writeTag(cycleRoot, "mineable/pickaxe.json", R"({"values": ["#minecraft:loop_a"]})");
        writeTag(cycleRoot, "loop_a.json",
                 R"({"values": ["#minecraft:loop_b", "minecraft:stone"]})");
        writeTag(cycleRoot, "loop_b.json", R"({"values": ["#minecraft:loop_a"]})");

        assets::StandardPackResourceProvider pack{cycleRoot};
        mc::gameplay::BlockTagTable tags;
        tags.load(pack);
        REQUIRE(tags.has(Block::Stone, BlockTag::MineableWithPickaxe));
    }

    // --- Layering: a higher pack appends to a lower one... ---
    {
        writeTag(overlayRoot, "mineable/pickaxe.json", R"({"values": ["minecraft:cobblestone"]})");
        assets::StandardPackResourceProvider base{packRoot};
        assets::StandardPackResourceProvider overlay{overlayRoot};
        const assets::LayeredResourceProvider resources{base, {&overlay}};

        mc::gameplay::BlockTagTable tags;
        tags.load(resources);
        REQUIRE(tags.has(Block::Cobblestone, BlockTag::MineableWithPickaxe)); // from the overlay
        REQUIRE(tags.has(Block::Stone, BlockTag::MineableWithPickaxe));       // still from the base
    }

    // --- ...and `replace` discards what the packs below contributed. ---
    {
        const fs::path replaceRoot = root / "replace";
        fs::create_directories(replaceRoot);
        writeTag(replaceRoot, "mineable/pickaxe.json",
                 R"({"replace": true, "values": ["minecraft:cobblestone"]})");
        assets::StandardPackResourceProvider base{packRoot};
        assets::StandardPackResourceProvider overlay{replaceRoot};
        const assets::LayeredResourceProvider resources{base, {&overlay}};

        mc::gameplay::BlockTagTable tags;
        tags.load(resources);
        REQUIRE(tags.has(Block::Cobblestone, BlockTag::MineableWithPickaxe));
        REQUIRE(!tags.has(Block::Stone, BlockTag::MineableWithPickaxe));
        // A tag no pack supplied keeps its built-in contents regardless.
        REQUIRE(tags.has(Block::Obsidian, BlockTag::NeedsDiamondTool));
    }

    // --- No data pack at all — which is every ordinary resource pack, since a
    // resource pack carries only `assets/`. The built-in defaults must carry the
    // whole table, or mining silently loses every tool requirement and speed
    // bonus on a normal installation. ---
    {
        assets::StandardPackResourceProvider empty{root / "does-not-exist"};
        mc::gameplay::BlockTagTable tags;
        tags.load(empty);
        REQUIRE(!tags.dataDriven(BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::Stone, BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::IronOre, BlockTag::NeedsStoneTool));
        REQUIRE(tags.has(Block::Obsidian, BlockTag::NeedsDiamondTool));
        REQUIRE(tags.has(Block::OakLog, BlockTag::MineableWithAxe));
        REQUIRE(tags.has(Block::Dirt, BlockTag::MineableWithShovel));
        REQUIRE(tags.has(Block::OakLeaves, BlockTag::Leaves));
        // And still says no to a block in none of them.
        REQUIRE(!tags.has(Block::Dirt, BlockTag::MineableWithPickaxe));
    }

    // --- A malformed tag file does not take the rest of the load down. ---
    {
        const fs::path brokenRoot = root / "broken";
        fs::create_directories(brokenRoot);
        writeTag(brokenRoot, "mineable/pickaxe.json", "{ not json");
        writeTag(brokenRoot, "leaves.json", R"({"values": ["minecraft:oak_leaves"]})");
        assets::StandardPackResourceProvider pack{brokenRoot};
        mc::gameplay::BlockTagTable tags;
        tags.load(pack);
        // The malformed file still counts as supplied, so it replaces the
        // built-in pickaxe tag with nothing rather than silently keeping it.
        REQUIRE(!tags.has(Block::Stone, BlockTag::MineableWithPickaxe));
        REQUIRE(tags.has(Block::OakLeaves, BlockTag::Leaves));
    }

    fs::remove_all(root);
    return 0;
}
