#include "world/ChunkSection.hpp"
#include "world/NibbleArray.hpp"

#include <cassert>

int main() {
    mc::world::NibbleArray values{15U};
    assert(values.uniform());
    assert(values.get(0U) == 15U);
    assert(values.get(4095U) == 15U);
    assert(values.set(1U, 3U));
    assert(!values.uniform());
    assert(values.get(0U) == 15U);
    assert(values.get(1U) == 3U);
    assert(!values.set(1U, 3U));
    values.fill(7U);
    assert(values.uniform());
    assert(values.get(2048U) == 7U);

    mc::world::ChunkSection section;
    assert(section.skyLight(2, 3, 4) == 0U);
    assert(section.setSkyLight(2, 3, 4, 15U));
    assert(section.skyLight(2, 3, 4) == 15U);
    assert(section.setBlockLight(2, 3, 4, 14U));
    assert(section.blockLight(2, 3, 4) == 14U);
    return 0;
}
