#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkSection.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <glm/vec3.hpp>

// Exercises dropped items as a headless simulation: they fall and rest on a
// floor, lean on walls instead of passing through, merge with identical
// neighbours, and drift into the player's backpack with a pickup animation.
namespace {

// A stone-floored chunk with a wall along x == 1 (cells 1..2) that rises to
// y == 4. Items spawn and fall around it.
mc::world::World buildTestWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    for (int y = 1; y <= 4; ++y) {
        for (int z = 0; z < 16; ++z) {
            chunk.setBlock(1, y, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

mc::gameplay::ItemStack stone(std::uint8_t count) {
    return {mc::world::Block::Stone, count, mc::gameplay::blockItemFor(mc::world::Block::Stone)};
}

} // namespace

int main() {
    using namespace mc;
    using namespace mc::gameplay;

    // --- A dropped item falls and rests flush on the floor. ---
    {
        world::World world = buildTestWorld();
        ItemEntitySystem items;
        items.spawn({3.0F, 6.0F, 3.0F}, stone(1U), {0.0F, 0.0F, 0.0F});
        Inventory inventory;
        // The player stands far away so the item is not magnet-picked up.
        const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
        for (int tick = 0; tick < 200; ++tick) {
            static_cast<void>(items.tick(world, farPlayer, inventory));
            if (!items.entities().empty() &&
                items.entities().front().velocity.y == 0.0F &&
                items.entities().front().position.y < 2.0F) {
                break;
            }
        }
        assert(items.entities().size() == 1U);
        // The 0.25³ box rests with its base on the floor's top (y == 1).
        const float y = items.entities().front().position.y;
        assert(y > 1.05F && y < 1.2F);
    }

    // --- A dropped item leans on a wall instead of passing through it. ---
    {
        world::World world = buildTestWorld();
        ItemEntitySystem items;
        // Thrown leftward toward the wall at x == 1..2, from above the floor.
        items.spawn({6.0F, 2.5F, 3.0F}, stone(1U), {-0.35F, 0.0F, 0.0F});
        Inventory inventory;
        const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
        for (int tick = 0; tick < 200; ++tick) {
            static_cast<void>(items.tick(world, farPlayer, inventory));
        }
        assert(!items.entities().empty());
        // The box's west edge (centre - 0.125) stays on the wall's east face (2).
        assert(items.entities().front().position.x >= 2.05F);
        assert(items.entities().front().position.x < 3.0F);
    }

    // --- Identical items in the same cell merge into one group. ---
    {
        world::World world = buildTestWorld();
        ItemEntitySystem items;
        items.spawn({4.0F, 2.0F, 4.0F}, stone(3U), {0.0F, 0.0F, 0.0F});
        items.spawn({4.05F, 2.05F, 4.05F}, stone(2U), {0.0F, 0.0F, 0.0F});
        Inventory inventory;
        const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
        for (int tick = 0; tick < 40; ++tick) {
            static_cast<void>(items.tick(world, farPlayer, inventory));
        }
        // After settling, the pair is a single entity carrying the combined stack.
        const auto& entities = items.entities();
        assert(entities.size() == 1U);
        assert(entities.front().stack.count == 5U);
    }

    // --- Within reach, an item drifts to the player and is collected. ---
    {
        world::World world = buildTestWorld();
        ItemEntitySystem items;
        // Just inside the 1.5-block magnet radius, resting on the floor.
        items.spawn({4.0F, 1.2F, 3.0F}, stone(2U), {0.0F, 0.0F, 0.0F});
        Inventory inventory;
        const glm::vec3 player{3.0F, 1.0F, 3.0F};
        for (int tick = 0; tick < 300; ++tick) {
            static_cast<void>(items.tick(world, player, inventory));
            if (items.entities().empty()) {
                break;
            }
        }
        assert(items.entities().empty());
        // The stone landed in the player's backpack.
        bool collected = false;
        for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
            const auto& stack = inventory.slot(index);
            if (stack.block == world::Block::Stone && stack.count == 2U) {
                collected = true;
            }
        }
        assert(collected);
    }

    return 0;
}
