#include "world/WorldConstants.hpp"

int main() {
    if (mc::world::kChunkWidth != 16 || mc::world::kChunkDepth != 16) {
        return 1;
    }
    if (mc::world::kWorldHeight != 256 || mc::world::kSectionCount != 16) {
        return 2;
    }
    if (mc::world::kSeaLevel != 63) {
        return 3;
    }
    return 0;
}
