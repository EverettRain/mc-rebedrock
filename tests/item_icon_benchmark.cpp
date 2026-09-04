// RN-14 (REGULAR §5.4): what the item-model icon costs per frame.
//
// The inventory icon is a per-frame path — every visible slot is re-recorded
// every frame — and RN-14 changed two things about it:
//
//   * the CPU work per icon: it used to be "read three atlas layers and push one
//     struct"; it is now "look the block's item model up and push one struct per
//     visible face of each of its boxes";
//   * the DRAW COUNT: one per icon, now one per visible face per box.
//
// The second is the one that scales the GPU side, so it is counted exactly rather
// than timed. The first is timed here.
//
// This is deliberately not a "is it fast enough" gate — the icon path was never
// near a budget. It exists so the numbers are on the record, and so the
// per-frame cost of `iconBoxOf` (which walks six facings and searches four corner
// positions per corner) is visibly NOT being paid: the icon boxes are baked into
// `kItemIconBoxes` at compile time and the draw path indexes them.
//
// Release build:
//   cmake --build build/linux-release --target mc_rebedrock_item_icon_benchmark
//   ./build/linux-release/tests/mc_rebedrock_item_icon_benchmark

#include "world/Block.hpp"
#include "world/ItemModel.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

using namespace mc::world;

// A screenful: 27 storage slots + 9 hotbar, filled the way a redstone-and-
// building player's inventory actually is — the shaped blocks that RN-14 turned
// from one-box cubes into multi-box models are over-represented on purpose, so
// the number is an upper bound rather than an average.
[[nodiscard]] std::vector<Block> screenful() {
    return {
        Block::Stone,      Block::OakPlanks,   Block::OakStairs,   Block::OakStairs,
        Block::CobblestoneStairs,
        Block::CobblestoneWall, Block::OakFenceGate,
        Block::StonePressurePlate, Block::StoneButton, Block::OakSlab,
        Block::Chest,      Block::Furnace,     Block::Observer,    Block::Piston,
        Block::Dirt,       Block::Cobblestone, Block::OakLog,      Block::Glass,
        Block::OakStairs,  Block::CobblestoneWall, Block::OakFenceGate,
        Block::StoneButton, Block::StonePressurePlate, Block::OakSlab,
        Block::Sand,       Block::Gravel,      Block::IronBlock,   Block::GoldBlock,
        Block::OakStairs,  Block::OakFenceGate, Block::CobblestoneWall,
        Block::Stone,      Block::OakPlanks,   Block::Bricks,      Block::Bookshelf,
        Block::CraftingTable,
    };
}

// Exactly what HudRenderer::drawHudBlockIcon does per icon, minus the two Vulkan
// calls: resolve the model, walk its boxes and their visible faces, and assemble
// the sixteen push-constant floats for each.
[[nodiscard]] float recordIcon(Block block) {
    float sink = 0.0F;
    const auto layers = cubeItemLayers(block);
    const auto range = itemModelRange(block);
    for (std::size_t b = 0; b < range.count; ++b) {
        const IconBox& icon = kItemIconBoxes[static_cast<std::size_t>(range.first) + b];
        for (std::size_t f = 0; f < kIconFaces.size(); ++f) {
            if (!icon.present[f]) {
                continue;
            }
            sink += icon.from.x + icon.from.y + icon.from.z;
            sink += icon.to.x + icon.to.y + icon.to.z;
            sink += itemFaceLayer(layers, icon.slot[f]);
            for (std::size_t c = 0; c < 4; ++c) {
                sink += icon.uvCorner[f][c].x + icon.uvCorner[f][c].y;
            }
        }
    }
    return sink;
}

[[nodiscard]] std::size_t drawsFor(Block block) {
    std::size_t draws = 0;
    const auto range = itemModelRange(block);
    for (std::size_t b = 0; b < range.count; ++b) {
        const IconBox& icon = kItemIconBoxes[static_cast<std::size_t>(range.first) + b];
        for (std::size_t f = 0; f < kIconFaces.size(); ++f) {
            draws += icon.present[f] ? 1U : 0U;
        }
    }
    return draws;
}

} // namespace

int main() {
    const auto slots = screenful();

    // --- the draw count, exactly ---
    std::size_t draws = 0;
    std::size_t worstBlockDraws = 0;
    for (const Block block : slots) {
        const std::size_t blockDraws = drawsFor(block);
        draws += blockDraws;
        worstBlockDraws = blockDraws > worstBlockDraws ? blockDraws : worstBlockDraws;
    }
    std::printf("slots                 %zu\n", slots.size());
    std::printf("draws before RN-14    %zu   (one per icon)\n", slots.size());
    std::printf("draws after  RN-14    %zu   (one per visible face per box)\n", draws);
    std::printf("worst single icon     %zu draws (a fence gate: 8 boxes)\n", worstBlockDraws);

    // --- the CPU record cost ---
    constexpr int kWarmup = 200;
    constexpr int kFrames = 20000;
    float sink = 0.0F;
    for (int i = 0; i < kWarmup; ++i) {
        for (const Block block : slots) {
            sink += recordIcon(block);
        }
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        for (const Block block : slots) {
            sink += recordIcon(block);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    std::printf("record a screenful    %.0f ns/frame  (%.1f ns per icon, %.1f ns per draw)\n",
                nanoseconds / kFrames, nanoseconds / kFrames / static_cast<double>(slots.size()),
                nanoseconds / kFrames / static_cast<double>(draws));
    std::printf("(sink %.1f)\n", static_cast<double>(sink));
    return 0;
}
