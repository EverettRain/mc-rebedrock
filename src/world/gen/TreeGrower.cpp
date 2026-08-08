#include "world/gen/TreeGrower.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <cmath>

namespace mc::world::gen {

bool treeReplaceable(Block block) {
    return block == Block::Air || isLeaves(block) || block == Block::GrassPlant ||
           block == Block::Dandelion || block == Block::Water;
}

namespace {

constexpr float kPi = 3.14159265358979323846F;

void placeLog(TreeWriter& writer, int x, int y, int z, Block log) {
    if (treeReplaceable(writer.block(x, y, z))) {
        writer.setBlock(x, y, z, log);
        writer.setOrientation(x, y, z, BlockOrientation::Up);
    }
}

// BlobFoliagePlacer: the ball of leaves oak and birch wear. The crown hangs
// from the trunk top down `foliageHeight` rows — layer 0 sits at the top and
// each layer below widens by one every two rows, exactly vanilla's
// `max((foliageRadius + nodeRadius - 1) - i / 2, 0)` over the foliage height
// (not the full trunk length, which would stretch the ball into a pyramid).
// The four corners are trimmed by `isInvalidForLeaves`: dropped entirely on the
// top layer and half the time on every layer below, which rounds the silhouette.
void placeBlobFoliage(
    TreeWriter& writer,
    JavaRandom& random,
    int x,
    int topY,
    int z,
    Block leaves,
    int foliageRadius,
    int foliageHeight) {
    for (int layer = 0; layer >= -foliageHeight; --layer) {
        const int radius = std::max((foliageRadius - 1) - layer / 2, 0);
        for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
            for (int offsetZ = -radius; offsetZ <= radius; ++offsetZ) {
                const int dx = std::abs(offsetX);
                const int dz = std::abs(offsetZ);
                if (dx == radius && dz == radius && (layer == 0 || random.nextInt(2) == 0)) {
                    continue;
                }
                if (writer.block(x + offsetX, topY + layer, z + offsetZ) == Block::Air) {
                    writer.setBlock(x + offsetX, topY + layer, z + offsetZ, leaves);
                }
            }
        }
    }
}

// SpruceFoliagePlacer / PineFoliagePlacer: a cone that widens downward, with
// the pine's crown compressed into the top few layers.
void placeConiferFoliage(
    TreeWriter& writer,
    JavaRandom& random,
    int x,
    int topY,
    int z,
    Block leaves,
    int height,
    bool pine) {
    int radius = pine ? 1 : 0;
    int layersAtRadius = 0;
    for (int layer = 0; layer < height; ++layer) {
        const int y = topY - layer;
        for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
            for (int offsetZ = -radius; offsetZ <= radius; ++offsetZ) {
                if (std::abs(offsetX) + std::abs(offsetZ) > radius + (radius > 1 ? 1 : 0)) {
                    continue;
                }
                if (writer.block(x + offsetX, y, z + offsetZ) == Block::Air) {
                    writer.setBlock(x + offsetX, y, z + offsetZ, leaves);
                }
            }
        }
        ++layersAtRadius;
        if (pine) {
            // The pine keeps each radius for two layers before stepping out.
            if (layersAtRadius >= 2) {
                ++radius;
                layersAtRadius = 0;
            }
        } else if (radius < 2 || random.nextInt(2) == 0) {
            ++radius;
        } else {
            radius = std::max(radius - 1, 1);
        }
        radius = std::min(radius, 3);
    }
}

// AcaciaFoliagePlacer: a flat plate two blocks across with a thinner ring above.
void placeAcaciaFoliage(TreeWriter& writer, int x, int topY, int z, Block leaves) {
    for (int offsetX = -3; offsetX <= 3; ++offsetX) {
        for (int offsetZ = -3; offsetZ <= 3; ++offsetZ) {
            if (std::abs(offsetX) + std::abs(offsetZ) > 4) {
                continue;
            }
            if (writer.block(x + offsetX, topY, z + offsetZ) == Block::Air) {
                writer.setBlock(x + offsetX, topY, z + offsetZ, leaves);
            }
        }
    }
    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
        for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
            if (writer.block(x + offsetX, topY + 1, z + offsetZ) == Block::Air) {
                writer.setBlock(x + offsetX, topY + 1, z + offsetZ, leaves);
            }
        }
    }
}

} // namespace

bool ChunkTreeWriter::inBounds(int x, int y, int z) const {
    const int localX = x - chunkX_ * kChunkWidth;
    const int localZ = z - chunkZ_ * kChunkDepth;
    return localX >= 0 && localX < kChunkWidth && y >= 0 && y < kWorldHeight &&
           localZ >= 0 && localZ < kChunkDepth;
}

Block ChunkTreeWriter::block(int x, int y, int z) const {
    if (!inBounds(x, y, z)) {
        return Block::Air;
    }
    return chunk_.block(x - chunkX_ * kChunkWidth, y, z - chunkZ_ * kChunkDepth);
}

bool ChunkTreeWriter::setBlock(int x, int y, int z, Block value) {
    if (!inBounds(x, y, z)) {
        // The crown crosses the chunk border: hold the block back so the
        // streamer can apply it to the neighbouring chunk once it is published.
        // The orientation arrives on the following setOrientation call, which
        // patches the matching entry below.
        borderBlocks_.push_back({x, y, z, value, BlockOrientation::North});
        return false;
    }
    chunk_.setBlock(x - chunkX_ * kChunkWidth, y, z - chunkZ_ * kChunkDepth, value);
    return true;
}

bool ChunkTreeWriter::setOrientation(int x, int y, int z, BlockOrientation value) {
    if (!inBounds(x, y, z)) {
        for (auto& block : borderBlocks_) {
            if (block.worldX == x && block.y == y && block.worldZ == z) {
                block.orientation = value;
                break;
            }
        }
        return false;
    }
    chunk_.setOrientation(x - chunkX_ * kChunkWidth, y, z - chunkZ_ * kChunkDepth, value);
    return true;
}

bool isSoilForPlants(Block block) {
    return block == Block::Grass || block == Block::Dirt || block == Block::CoarseDirt ||
           block == Block::Podzol;
}

TreeChoice treeChoiceForSapling(Block sapling) {
    switch (sapling) {
    case Block::SpruceSapling:
        return {TreeKind::Spruce, Block::SpruceLog, Block::SpruceLeaves, 1.0F};
    case Block::BirchSapling:
        return {TreeKind::Birch, Block::BirchLog, Block::BirchLeaves, 1.0F};
    case Block::JungleSapling:
        return {TreeKind::JungleTree, Block::JungleLog, Block::JungleLeaves, 1.0F};
    case Block::AcaciaSapling:
        return {TreeKind::Acacia, Block::AcaciaLog, Block::AcaciaLeaves, 1.0F};
    case Block::DarkOakSapling:
        return {TreeKind::DarkOak, Block::DarkOakLog, Block::DarkOakLeaves, 1.0F};
    default:
        return {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 1.0F};
    }
}

bool isSapling(Block block) {
    return block == Block::OakSapling || block == Block::SpruceSapling ||
           block == Block::BirchSapling || block == Block::JungleSapling ||
           block == Block::AcaciaSapling || block == Block::DarkOakSapling;
}

bool growTree(
    TreeWriter& writer,
    JavaRandom& random,
    const TreeChoice& choice,
    int worldX,
    int groundY,
    int worldZ) {
    if (!isSoilForPlants(writer.block(worldX, groundY, worldZ))) {
        return false;
    }
    const int baseY = groundY + 1;
    const Block log = choice.log;
    const Block leaves = choice.leaves;

    switch (choice.kind) {
    case TreeKind::Oak:
    case TreeKind::Birch:
    case TreeKind::SwampOak: {
        // StraightTrunkPlacer(base, randomA, randomB) + BlobFoliagePlacer.
        const int base = choice.kind == TreeKind::Oak ? 4 : (choice.kind == TreeKind::Birch ? 5 : 5);
        const int extra = choice.kind == TreeKind::SwampOak ? 3 : 2;
        const int height = base + random.nextInt(extra + 1);
        if (baseY + height + 2 >= kWorldHeight) return false;
        for (int y = 0; y < height; ++y) {
            placeLog(writer, worldX, baseY + y, worldZ, log);
        }
        // BlobFoliagePlacer(foliageRadius, ..., height): the oak/birch ball is
        // three rows tall regardless of the trunk's length.
        placeBlobFoliage(writer, random, worldX, baseY + height, worldZ, leaves,
                         choice.kind == TreeKind::SwampOak ? 3 : 2, 3);
        return true;
    }
    case TreeKind::FancyOak: {
        // LargeOakTrunkPlacer(3, 11, 0): a tall trunk with two or three limbs
        // that each carry their own blob.
        const int height = 3 + random.nextInt(12);
        if (baseY + height + 4 >= kWorldHeight) return false;
        for (int y = 0; y < height; ++y) {
            placeLog(writer, worldX, baseY + y, worldZ, log);
        }
        const int limbs = 2 + random.nextInt(2);
        for (int limb = 0; limb < limbs; ++limb) {
            const int limbY = baseY + height - 1 - random.nextInt(std::max(height / 3, 1));
            const int reach = 1 + random.nextInt(2);
            const float angle = random.nextFloat() * kPi * 2.0F;
            const int tipX = worldX +
                static_cast<int>(std::lround(std::cos(angle) * static_cast<float>(reach)));
            const int tipZ = worldZ +
                static_cast<int>(std::lround(std::sin(angle) * static_cast<float>(reach)));
            for (int step = 1; step <= reach; ++step) {
                const int x = worldX + (tipX - worldX) * step / reach;
                const int z = worldZ + (tipZ - worldZ) * step / reach;
                placeLog(writer, x, limbY + step, z, log);
            }
            placeBlobFoliage(writer, random, tipX, limbY + reach + 1, tipZ, leaves, 2, 3);
        }
        placeBlobFoliage(writer, random, worldX, baseY + height, worldZ, leaves, 2, 3);
        return true;
    }
    case TreeKind::Spruce:
    case TreeKind::Pine: {
        const bool pine = choice.kind == TreeKind::Pine;
        const int height = pine ? 6 + random.nextInt(5) : 5 + random.nextInt(3);
        if (baseY + height + 3 >= kWorldHeight) return false;
        for (int y = 0; y < height; ++y) {
            placeLog(writer, worldX, baseY + y, worldZ, log);
        }
        const int foliageHeight = pine ? 3 + random.nextInt(2) : height - 1;
        placeConiferFoliage(writer, random, worldX, baseY + height, worldZ, leaves,
                            foliageHeight, pine);
        if (writer.block(worldX, baseY + height + 1, worldZ) == Block::Air) {
            writer.setBlock(worldX, baseY + height + 1, worldZ, leaves);
        }
        return true;
    }
    case TreeKind::JungleTree: {
        const int height = 4 + random.nextInt(9);
        if (baseY + height + 3 >= kWorldHeight) return false;
        for (int y = 0; y < height; ++y) {
            placeLog(writer, worldX, baseY + y, worldZ, log);
        }
        placeBlobFoliage(writer, random, worldX, baseY + height, worldZ, leaves, 2, 3);
        return true;
    }
    case TreeKind::Acacia: {
        // ForkingTrunkPlacer(5, 2, 2): the trunk leans partway up and the crown
        // sits over the lean, which is what gives an acacia its silhouette.
        const int height = 5 + random.nextInt(3);
        if (baseY + height + 3 >= kWorldHeight) return false;
        const int forkAt = height / 2 + random.nextInt(2);
        const int leanX = random.nextInt(3) - 1;
        const int leanZ = leanX == 0 ? (random.nextBoolean() ? 1 : -1) : 0;
        int x = worldX;
        int z = worldZ;
        for (int y = 0; y < height; ++y) {
            if (y == forkAt) {
                x += leanX;
                z += leanZ;
            }
            placeLog(writer, x, baseY + y, z, log);
        }
        placeAcaciaFoliage(writer, x, baseY + height, z, leaves);
        return true;
    }
    case TreeKind::DarkOak: {
        // DarkOakTrunkPlacer(6, 2, 1) + DarkOakFoliagePlacer(0, 0, 0, 0): a 2x2
        // trunk whose upper run leans up to two cells, a four-layer canopy whose
        // widest layer is 8x8 (radius 4) with an optional 2x2 cap, and 1-in-3
        // branch columns off the outer ring, each with a small foliage node.
        // Footprints are eccentric squares (offsets -radius+1..radius, centred
        // between the trunk's two corners), corner-trimmed into an octagon,
        // exactly like vanilla's generateSquare over -baseHeight..baseHeight+1.
        const int height = 6 + random.nextInt(3);
        if (baseY + height + 3 >= kWorldHeight) return false;
        for (int cornerX = 0; cornerX < 2; ++cornerX) {
            for (int cornerZ = 0; cornerZ < 2; ++cornerZ) {
                if (!isSoilForPlants(writer.block(worldX + cornerX, groundY, worldZ + cornerZ))) {
                    return false;
                }
            }
        }
        // The trunk leans from `leanAt` upward: `lean` cells in one horizontal
        // direction, matching DarkOakTrunkPlacer's `i = height - nextInt(4)` and
        // `j = 2 - nextInt(3)`.
        const int leanAt = height - random.nextInt(4);
        int lean = 2 - random.nextInt(3);
        int leanX = 0;
        int leanZ = 0;
        if (lean > 0) {
            switch (random.nextInt(4)) {
            case 0: leanX = 1; break;
            case 1: leanX = -1; break;
            case 2: leanZ = 1; break;
            default: leanZ = -1; break;
            }
        }
        int tipX = worldX;
        int tipZ = worldZ;
        for (int y = 0; y < height; ++y) {
            if (y >= leanAt && lean > 0) {
                tipX += leanX;
                tipZ += leanZ;
                --lean;
            }
            for (int cornerX = 0; cornerX < 2; ++cornerX) {
                for (int cornerZ = 0; cornerZ < 2; ++cornerZ) {
                    placeLog(writer, tipX + cornerX, baseY + y, tipZ + cornerZ, log);
                }
            }
        }
        const int canopyY = baseY + height;
        const auto placeFoliageLayer = [&](int cx, int cz, int radius, int yOffset,
                                           bool trimCorners) {
            for (int offsetX = -radius + 1; offsetX <= radius; ++offsetX) {
                for (int offsetZ = -radius + 1; offsetZ <= radius; ++offsetZ) {
                    if (trimCorners &&
                        (offsetX == -radius + 1 || offsetX == radius) &&
                        (offsetZ == -radius + 1 || offsetZ == radius)) {
                        continue;
                    }
                    if (writer.block(cx + offsetX, canopyY + yOffset, cz + offsetZ) == Block::Air) {
                        writer.setBlock(cx + offsetX, canopyY + yOffset, cz + offsetZ, leaves);
                    }
                }
            }
        };
        // The main canopy hangs off the leaned trunk tip: 6x6 under the crown,
        // 8x8 at the crown, 6x6 above, and a 50% 2x2 cap on top.
        placeFoliageLayer(tipX, tipZ, 3, -1, true);
        placeFoliageLayer(tipX, tipZ, 4, 0, true);
        placeFoliageLayer(tipX, tipZ, 3, 1, true);
        if (random.nextBoolean()) {
            placeFoliageLayer(tipX, tipZ, 1, 2, false);
        }
        // The outer ring around the trunk base grows 1-in-3 branch columns that
        // descend below the canopy, each topped by its own small foliage node.
        for (int q = -1; q <= 2; ++q) {
            for (int r = -1; r <= 2; ++r) {
                if ((q < 0 || q > 1 || r < 0 || r > 1) && random.nextInt(3) <= 0) {
                    const int reach = 2 + random.nextInt(3);
                    for (int step = 0; step < reach; ++step) {
                        placeLog(writer, tipX + q, canopyY - step - 1, tipZ + r, log);
                    }
                    placeFoliageLayer(tipX + q, tipZ + r, 2, -1, true);
                    placeFoliageLayer(tipX + q, tipZ + r, 1, 0, false);
                }
            }
        }
        return true;
    }
    }
    return false;
}

} // namespace mc::world::gen
