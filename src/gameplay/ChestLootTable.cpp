#include "gameplay/ChestLootTable.hpp"

#include "core/Json.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/Random.hpp"
#include "world/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace mc::gameplay {
namespace {

// A loot number in an integer context (a roll count, a stack size). Constant
// rounds; Uniform draws min + nextInt(max-min+1), the integer-uniform
// UniformLootNumberProvider produces (and the shape MiningSystem::randomCount
// already uses for block loot).
[[nodiscard]] int evalNumberInt(const data::ChestNumber& number, std::uint64_t& state) {
    const int low = static_cast<int>(std::lround(number.min));
    if (number.kind == data::ChestNumberKind::Constant) {
        return low;
    }
    const int high = static_cast<int>(std::lround(number.max));
    if (high <= low) {
        return low;
    }
    const auto span = static_cast<std::uint32_t>(high - low) + 1U;
    return low + static_cast<int>(mc::rng::nextInt(state, span));
}

// A loot item name -> stack, the same block-name / block-item / plain-item rule
// LootTable::resolveDrop uses. Returns false for content this build lacks (the
// stack is then skipped, its rng draws already spent).
[[nodiscard]] bool resolveItemStack(std::string_view name, std::uint8_t count, ItemStack& out) {
    if (const auto block = world::blockFromIdentifier(name); block.has_value()) {
        out = ItemStack{*block, count, blockItemFor(*block)};
        return true;
    }
    if (const Item* item = itemFromIdentifier(name); item != nullptr) {
        if (const BlockItem* blockItem = asBlockItem(item); blockItem != nullptr) {
            out = ItemStack{blockItem->block(), count, item};
        } else {
            out = ItemStack{world::Block::Air, count, item};
        }
        return true;
    }
    return false;
}

// `loot_table/chests/igloo_chest.json` -> `chests/igloo_chest`, the loot-table
// id half a structure references (namespaced by the caller to
// `minecraft:chests/igloo_chest`).
[[nodiscard]] std::string_view lootTableIdFromPath(std::string_view path) {
    constexpr std::string_view kPrefix = "loot_table/";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) {
        path.remove_prefix(kPrefix.size());
    }
    if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
        path.remove_suffix(5U);
    }
    return path;
}

} // namespace

void ChestLootTable::loadBuiltinDefaults() { tables_.clear(); }

void ChestLootTable::load(const assets::ResourceProvider& resources) {
    loadBuiltinDefaults();
    applyOverlay(resources);
}

void ChestLootTable::applyOverlay(const assets::ResourceProvider& resources) {
    for (const auto& location :
         resources.list("minecraft", "loot_table/chests", assets::PackType::ServerData)) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        const std::string_view id = lootTableIdFromPath(location.path);
        std::string identifier = location.space + ":" + std::string{id};
        core::Json root;
        try {
            root = core::Json::parse(std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed table must not take the rest of the pack down
        }
        if (auto def = data::jeChestLoot(root, identifier); def.has_value()) {
            tables_.insert_or_assign(def->identifier, std::move(*def));
        }
    }
}

const data::ChestLootTableDef* ChestLootTable::find(std::string_view identifier) const {
    const auto found = tables_.find(std::string{identifier});
    return found == tables_.end() ? nullptr : &found->second;
}

std::vector<ItemStack> ChestLootTable::roll(const data::ChestLootTableDef& table,
                                            std::uint64_t& state) const {
    std::vector<ItemStack> out;
    for (const auto& pool : table.pools) {
        const int rolls = evalNumberInt(pool.rolls, state);
        std::int64_t totalWeight = 0;
        for (const auto& entry : pool.entries) {
            totalWeight += entry.weight;
        }
        if (totalWeight <= 0) {
            continue;
        }
        for (int roll = 0; roll < rolls; ++roll) {
            const auto pick =
                static_cast<std::int64_t>(mc::rng::nextInt(state, static_cast<std::uint32_t>(totalWeight)));
            const data::ChestLootEntry* chosen = nullptr;
            std::int64_t cumulative = 0;
            for (const auto& entry : pool.entries) {
                cumulative += entry.weight;
                if (pick < cumulative) {
                    chosen = &entry;
                    break;
                }
            }
            if (chosen == nullptr || chosen->kind == data::ChestEntryKind::Empty) {
                continue;
            }
            // set_count sizes the stack; a chained add-function adds to it. Drawn
            // before the item is resolved so a missing item does not shift the rng.
            int count = 1;
            for (const auto& function : chosen->functions) {
                if (function.kind == data::ChestFunctionKind::SetCount) {
                    const int value = evalNumberInt(function.count, state);
                    count = function.add ? count + value : value;
                }
            }
            count = std::clamp(count, 1, 255);
            ItemStack stack;
            if (resolveItemStack(chosen->name, static_cast<std::uint8_t>(count), stack)) {
                out.push_back(stack);
            }
        }
    }
    return out;
}

std::vector<ItemStack> ChestLootTable::roll(std::string_view identifier,
                                            std::uint64_t& state) const {
    if (const data::ChestLootTableDef* table = find(identifier); table != nullptr) {
        return roll(*table, state);
    }
    return {};
}

void ChestLootTable::fillSlots(std::span<ItemStack> slots, const data::ChestLootTableDef& table,
                               std::uint64_t& state) const {
    std::vector<ItemStack> stacks = roll(table, state);

    std::vector<std::size_t> freeSlots;
    freeSlots.reserve(slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].empty()) {
            freeSlots.push_back(index);
        }
    }

    for (const ItemStack& stack : stacks) {
        if (freeSlots.empty()) {
            break; // more loot than the container holds: drop the surplus
        }
        const auto pick = mc::rng::nextInt(state, static_cast<std::uint32_t>(freeSlots.size()));
        slots[freeSlots[pick]] = stack;
        freeSlots[pick] = freeSlots.back();
        freeSlots.pop_back();
    }
}

ChestLootTable& chestLootTable() {
    static ChestLootTable table;
    return table;
}

} // namespace mc::gameplay
