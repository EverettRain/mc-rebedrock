#include "world/WorldConstants.hpp"

int main() {
    if (mc::world::kChunkWidth != 16 || mc::world::kChunkDepth != 16) {
        return 1;
    }
    if (mc::world::kWorldHeight != 384 || mc::world::kSectionCount != 24 ||
        mc::world::kMinY != -64 || mc::world::kMaxY != 320) {
        return 2;
    }
    // The section helpers stay inside the world column, with exact endpoints —
    // a regression that drops the kMinY offset would put a negative row in the
    // wrong section and the exact checks catch it.
    if (mc::world::sectionIndexFromWorldY(mc::world::kMinY) != 0) {
        return 3;
    }
    if (mc::world::sectionIndexFromWorldY(-1) != 3) {
        return 4;
    }
    if (mc::world::sectionIndexFromWorldY(0) != 4) {
        return 5;
    }
    if (mc::world::sectionIndexFromWorldY(mc::world::kMaxY - 1) != mc::world::kSectionCount - 1) {
        return 6;
    }
    if (mc::world::yInSectionFromWorldY(mc::world::kMinY) != 0) {
        return 7;
    }
    if (mc::world::yInSectionFromWorldY(-1) != 15) {
        return 8;
    }
    if (mc::world::sectionOriginY(0) != mc::world::kMinY) {
        return 9;
    }
    if (mc::world::kSeaLevel != 63) {
        return 10;
    }
    return 0;
}
