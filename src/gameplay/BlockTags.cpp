#include "gameplay/BlockTags.hpp"

#include "core/Json.hpp"
#include "data/TagFile.hpp"
#include "world/BlockRegistry.hpp"

#include <unordered_set>
#include <vector>

namespace mc::gameplay {
namespace {

// Indexed by BlockTag. 26.1 renamed the folder from `blocks` to `block`; the
// plan document still says `blocks`, which would silently resolve nothing.
constexpr std::array<std::string_view, kBlockTagCount> kBlockTagPaths{{
    "tags/block/mineable/pickaxe.json",
    "tags/block/mineable/axe.json",
    "tags/block/mineable/shovel.json",
    "tags/block/mineable/hoe.json",
    "tags/block/needs_stone_tool.json",
    "tags/block/needs_iron_tool.json",
    "tags/block/needs_diamond_tool.json",
    "tags/block/leaves.json",
    "tags/block/logs.json",
}};

// The built-in defaults, transcribed from 26.1's own tag files.
//
// They exist because of a real deployment constraint: a *resource* pack carries
// only `assets/`. The standard `vanilla-26.1` pack a player installs has no
// `data/` at all — only a tree extracted straight from the jar does. Loading
// tags exclusively from the pack would therefore leave most installations with
// every block untagged, which reads as "no tool requirement and no tool speed
// bonus" and quietly destroys mining.
//
// So these are the floor, and a data pack overrides them per tag. They are also
// where the five switch chains in MiningSystem went: the point of B2' is that
// this data is stated once, in tag shape, instead of five times in case lists
// that drift apart — which is the reason T0.1 had to exist at all.
//
// Spelled as identifiers rather than enumerators so they read exactly like the
// JSON they mirror, and so a block this build lacks is simply skipped.
constexpr std::array<std::string_view, 49> kBuiltinPickaxe{
    "stone",        "cobblestone",       "bricks",           "coal_ore",
    "iron_ore",     "gold_ore",          "diamond_ore",      "furnace",
    "obsidian",     "netherrack",        "stone_bricks",     "mossy_cobblestone",
    "sandstone",    "granite",           "diorite",          "andesite",
    "lapis_ore",    "redstone_ore",      "emerald_ore",      "mossy_stone_bricks",
    "chiseled_stone_bricks",             "quartz_block",     "polished_granite",
    "polished_diorite",                  "polished_andesite", "smooth_stone",
    // STRUCT/WG deep-layer blocks: copper ore and the whole deepslate family plus
    // its ore variants are the stone-family blocks terrain now generates below y=0.
    // Without a mineable/pickaxe tag a pickaxe gives no speed bonus (miningSpeed
    // stays 1), so deepslate's hardness 3-4.5 felt unmineable. (The crafted stone
    // building blocks — deepslate/tuff/blackstone stairs, slabs, walls, etc. — are
    // not generated in terrain and are a separate tag backlog.)
    "copper_ore",
    "deepslate",           "cobbled_deepslate",   "polished_deepslate",
    "deepslate_bricks",    "cracked_deepslate_bricks", "deepslate_tiles",
    "cracked_deepslate_tiles", "chiseled_deepslate", "reinforced_deepslate",
    "deepslate_coal_ore",  "deepslate_iron_ore",  "deepslate_copper_ore",
    "deepslate_gold_ore",  "deepslate_redstone_ore", "deepslate_emerald_ore",
    "deepslate_lapis_ore", "deepslate_diamond_ore",
    // ENCH-2: 26.1 lists enchanting_table in mineable/pickaxe and in no
    // needs_*_tool tag — any pickaxe mines it, and a bare hand still drops it
    // (there is no harvest tier), it is only slow.
    "enchanting_table",
    // ENCH-3: #minecraft:anvil (all three wear states) is in mineable/pickaxe,
    // and so is the block of iron the anvil is crafted from.
    "anvil", "chipped_anvil", "damaged_anvil", "iron_block",
};
constexpr std::array<std::string_view, 17> kBuiltinAxe{
    "oak_planks",    "oak_log",    "spruce_planks", "birch_planks",
    "spruce_log",    "birch_log",  "bookshelf",     "crafting_table",
    "pumpkin",       "melon",      "chest",         "jungle_log",
    "jungle_planks", "acacia_log", "acacia_planks", "dark_oak_log",
    "dark_oak_planks",
};
constexpr std::array<std::string_view, 10> kBuiltinShovel{
    "grass_block", "dirt",        "sand",   "gravel",   "clay",
    "snow_block",  "coarse_dirt", "podzol", "red_sand", "farmland",
};
constexpr std::array<std::string_view, 6> kBuiltinLeaves{
    "oak_leaves",    "spruce_leaves", "birch_leaves",
    "jungle_leaves", "acacia_leaves", "dark_oak_leaves",
};
constexpr std::array<std::string_view, 6> kBuiltinLogs{
    "oak_log", "spruce_log", "birch_log", "jungle_log", "acacia_log", "dark_oak_log",
};
// The tier gate (needs_*_tool): the deepslate ore variants mirror their stone
// counterpart's tier, and copper needs a stone pickaxe (26.1). coal/deepslate
// coal need only a wooden pickaxe, so they carry no needs_* tag.
constexpr std::array<std::string_view, 6> kBuiltinNeedsStone{
    "iron_ore", "lapis_ore", "copper_ore",
    "deepslate_iron_ore", "deepslate_lapis_ore", "deepslate_copper_ore",
};
constexpr std::array<std::string_view, 8> kBuiltinNeedsIron{
    "gold_ore", "diamond_ore", "redstone_ore", "emerald_ore",
    "deepslate_gold_ore", "deepslate_diamond_ore", "deepslate_redstone_ore",
    "deepslate_emerald_ore",
};
constexpr std::array<std::string_view, 1> kBuiltinNeedsDiamond{"obsidian"};

// How deep a chain of `#tag` references may go before it is treated as a cycle.
// 26.1's deepest real chain is `logs` -> `logs_that_burn` -> `oak_logs`, so this
// is generous; the visited set already stops a true cycle, and this bounds a
// pathological pack that nests without repeating.
constexpr int kMaximumTagDepth = 16;

// Turns `<namespace>:<name>` (or a bare name) into the tag's content path.
[[nodiscard]] assets::ResourceLocation tagLocation(std::string_view reference) {
    const auto separator = reference.find(':');
    const std::string_view space =
        separator == std::string_view::npos ? std::string_view{"minecraft"}
                                            : reference.substr(0, separator);
    const std::string_view name = separator == std::string_view::npos
                                      ? reference
                                      : reference.substr(separator + 1U);
    return assets::data("tags/block/" + std::string{name} + ".json", space);
}

// Accumulates the blocks a tag names, following `#tag` references. `visited`
// carries the tag paths already expanded on this branch, so a self-referential
// pack terminates instead of recursing forever.
// Returns whether any pack actually supplied this tag, which is what decides
// whether the built-in default is overridden or kept.
bool collectTag(const assets::ResourceProvider& resources, const assets::ResourceLocation& location,
                int depth, std::unordered_set<std::string>& visited,
                std::vector<world::Block>& out) {
    if (depth > kMaximumTagDepth || !visited.insert(location.toString()).second) {
        return false;
    }
    // Low priority to high, the way vanilla merges data packs: each pack
    // appends, and one that declares `replace` discards everything below it.
    const auto files = resources.readAllBytes(location);
    bool supplied = !files.empty();
    for (const auto& bytes : files) {
        core::Json root;
        try {
            root = core::Json::parse(
                std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed tag must not take the rest down
        }
        // The file shape is the D-1 tag codec now, not a hand walk of the Json.
        // A file whose Json is not a tag object supplies no members (but still
        // counts as supplied, so it replaces the built-in with nothing).
        data::TagFile tag;
        if (!data::Codec<data::TagFile>::read(root, tag)) {
            continue;
        }
        if (tag.replace) {
            out.clear();
        }
        for (const auto& entry : tag.values) {
            if (entry.id.empty()) {
                continue;
            }
            if (entry.id.front() == '#') {
                supplied = collectTag(resources,
                                      tagLocation(std::string_view{entry.id}.substr(1U)),
                                      depth + 1, visited, out) ||
                           supplied;
                continue;
            }
            // A vanilla tag names hundreds of blocks this build does not have.
            // Skipping them is the expected case, not a failure.
            if (const auto block = world::blockFromIdentifier(entry.id); block.has_value()) {
                out.push_back(*block);
            }
        }
    }
    return supplied;
}

} // namespace

std::string_view blockTagPath(BlockTag tag) {
    const auto index = static_cast<std::size_t>(tag);
    return index < kBlockTagPaths.size() ? kBlockTagPaths[index] : std::string_view{};
}

void BlockTagTable::set(world::Block block, BlockTag tag) {
    set(world::blockId(block), tag);
}

void BlockTagTable::set(world::BlockId block, BlockTag tag) {
    const auto index = block.index();
    // Grow to cover this id: a caller building a table by hand (a test, or a
    // block that registered after load) must be able to tag any id the registry
    // hands out, not only the ones present when the table was last sized.
    if (index >= masks_.size()) {
        masks_.resize(index + 1U);
    }
    masks_[index].set(static_cast<std::size_t>(tag));
}

void BlockTagTable::clear(BlockTag tag) {
    for (auto& mask : masks_) {
        mask.reset(static_cast<std::size_t>(tag));
    }
}

void BlockTagTable::loadBuiltinDefaults() {
    // Sized to the registry, so every registered BlockId has a slot before any
    // tag is applied (built-in content today, external content once R0-5 opens).
    masks_.assign(world::blockCount(), BlockTagMask{});
    dataDrivenTags_ = BlockTagMask{};
    const auto apply = [this](BlockTag tag, auto&& identifiers) {
        for (const auto identifier : identifiers) {
            if (const auto block = world::blockFromIdentifier(identifier); block.has_value()) {
                set(*block, tag);
            }
        }
    };
    apply(BlockTag::MineableWithPickaxe, kBuiltinPickaxe);
    apply(BlockTag::MineableWithAxe, kBuiltinAxe);
    apply(BlockTag::MineableWithShovel, kBuiltinShovel);
    // 26.1 puts leaves in mineable/hoe, which is why shears and hoes clear a
    // canopy quickly; the two tags cover the same blocks.
    apply(BlockTag::MineableWithHoe, kBuiltinLeaves);
    apply(BlockTag::NeedsStoneTool, kBuiltinNeedsStone);
    apply(BlockTag::NeedsIronTool, kBuiltinNeedsIron);
    apply(BlockTag::NeedsDiamondTool, kBuiltinNeedsDiamond);
    apply(BlockTag::Leaves, kBuiltinLeaves);
    apply(BlockTag::Logs, kBuiltinLogs);
}

void BlockTagTable::load(const assets::ResourceProvider& resources) {
    // Start from the built-ins so an installation whose pack carries only
    // `assets/` — which is every ordinary resource pack — still mines correctly.
    loadBuiltinDefaults();
    for (std::size_t index = 0; index < kBlockTagCount; ++index) {
        const auto tag = static_cast<BlockTag>(index);
        std::unordered_set<std::string> visited;
        std::vector<world::Block> blocks;
        if (!collectTag(resources, assets::data(std::string{blockTagPath(tag)}), 0, visited,
                        blocks)) {
            continue; // no pack supplies this tag: keep the built-in default
        }
        // A supplied tag replaces the default wholesale rather than adding to
        // it, so a data pack that removes a block from `mineable/pickaxe`
        // actually removes it.
        clear(tag);
        dataDrivenTags_.set(static_cast<std::size_t>(tag));
        for (const auto block : blocks) {
            set(block, tag);
        }
    }
}

BlockTagTable& blockTags() {
    static BlockTagTable table = [] {
        BlockTagTable defaults;
        defaults.loadBuiltinDefaults();
        return defaults;
    }();
    return table;
}

} // namespace mc::gameplay
