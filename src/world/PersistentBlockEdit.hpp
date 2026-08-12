#pragma once

#include "world/Block.hpp"

#include <cstdint>

namespace mc::world {

struct PersistentBlockEdit final {
    int x = 0;
    int y = 0;
    int z = 0;
    Block block = Block::Air;
    std::uint8_t fluidLevel = 0U;
    BlockOrientation orientation = BlockOrientation::North;
    // AbstractFurnaceBlock.LIT. Saves older than format 14 have no field for
    // it; their burning furnaces were a separate `lit_furnace` block, and the
    // loader turns that identifier back into this flag.
    bool lit = false;

    [[nodiscard]] bool operator==(const PersistentBlockEdit&) const = default;
};

} // namespace mc::world
