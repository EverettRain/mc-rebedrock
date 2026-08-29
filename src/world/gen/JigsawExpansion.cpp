#include "world/gen/JigsawExpansion.hpp"

#include "gameplay/Random.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstdlib>
#include <deque>

namespace mc::world::gen {
namespace {

// Half-open world box [min, max) a piece occupies. Adjacent boxes (a connection)
// do not overlap; only a real intrusion does.
struct Aabb final {
    int minX = 0, minY = 0, minZ = 0;
    int maxX = 0, maxY = 0, maxZ = 0;
};

[[nodiscard]] Aabb boxOf(const StructureTemplateDef& tmpl, int originX, int originY, int originZ,
                         StructureRotation rotation) {
    const int sizeX = rotatedSizeX(tmpl.sizeX, tmpl.sizeZ, rotation);
    const int sizeZ = rotatedSizeZ(tmpl.sizeX, tmpl.sizeZ, rotation);
    return {originX, originY, originZ, originX + sizeX, originY + tmpl.sizeY, originZ + sizeZ};
}

[[nodiscard]] bool overlaps(const Aabb& a, const Aabb& b) {
    return a.minX < b.maxX && a.maxX > b.minX && a.minY < b.maxY && a.maxY > b.minY &&
           a.minZ < b.maxZ && a.maxZ > b.minZ;
}

// The unit step of a direction: North=-Z, South=+Z, East=+X, West=-X, Up/Down=±Y.
[[nodiscard]] std::array<int, 3> step(BlockOrientation dir) {
    switch (dir) {
    case BlockOrientation::North: return {0, 0, -1};
    case BlockOrientation::South: return {0, 0, 1};
    case BlockOrientation::East: return {1, 0, 0};
    case BlockOrientation::West: return {-1, 0, 0};
    case BlockOrientation::Up: return {0, 1, 0};
    case BlockOrientation::Down: return {0, -1, 0};
    }
    return {0, 0, 0};
}

// A weighted-random try order over a pool's elements (indices), drawn without
// replacement so every element is attempted but in a weight-biased order — the
// shuffle Java's getShuffledJigsawBlocks/weighted pool does, flattened.
[[nodiscard]] std::vector<std::size_t> weightedOrder(
    const std::vector<data::StructurePoolElement>& elements, std::uint64_t& rng) {
    std::vector<std::size_t> remaining(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        remaining[i] = i;
    }
    std::vector<std::size_t> order;
    order.reserve(elements.size());
    while (!remaining.empty()) {
        std::int64_t total = 0;
        for (const auto index : remaining) {
            total += elements[index].weight;
        }
        auto pick = static_cast<std::int64_t>(mc::rng::nextInt(rng, static_cast<std::uint32_t>(total)));
        std::size_t chosen = 0;
        std::int64_t cumulative = 0;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            cumulative += elements[remaining[i]].weight;
            if (pick < cumulative) {
                chosen = i;
                break;
            }
        }
        order.push_back(remaining[chosen]);
        remaining[chosen] = remaining.back();
        remaining.pop_back();
    }
    return order;
}

// The four rotations in a seed-shuffled order, so a piece does not always prefer
// the same facing.
[[nodiscard]] std::array<StructureRotation, 4> rotationOrder(std::uint64_t& rng) {
    std::array<StructureRotation, 4> rotations{StructureRotation::None, StructureRotation::Clockwise90,
                                               StructureRotation::Clockwise180,
                                               StructureRotation::Counterclockwise90};
    for (int i = 3; i > 0; --i) {
        const auto j = static_cast<int>(mc::rng::nextInt(rng, static_cast<std::uint32_t>(i + 1)));
        std::swap(rotations[static_cast<std::size_t>(i)], rotations[static_cast<std::size_t>(j)]);
    }
    return rotations;
}

} // namespace

std::vector<JigsawPiece> jigsawExpand(const StructureManager& manager, std::string_view startPoolId,
                                      int originX, int originY, int originZ, int maxDepth,
                                      int maxDistance, std::uint64_t& rngState) {
    std::vector<JigsawPiece> pieces;
    const data::StructurePoolDef* startPool = manager.findPool(startPoolId);
    if (startPool == nullptr || startPool->elements.empty()) {
        return pieces;
    }
    // Start piece: a weighted element, a random rotation, at the origin.
    const auto startOrder = weightedOrder(startPool->elements, rngState);
    const StructureTemplateDef* startTemplate = nullptr;
    std::string startLocation;
    for (const auto index : startOrder) {
        if (const auto* t = manager.find(startPool->elements[index].location); t != nullptr) {
            startTemplate = t;
            startLocation = startPool->elements[index].location;
            break;
        }
    }
    if (startTemplate == nullptr) {
        return pieces;
    }
    const auto startRotation = rotationOrder(rngState).front();
    pieces.push_back({startLocation, originX, originY, originZ, startRotation});

    std::vector<Aabb> boxes;
    boxes.push_back(boxOf(*startTemplate, originX, originY, originZ, startRotation));

    std::deque<std::pair<std::size_t, int>> queue; // (piece index, depth)
    queue.emplace_back(0U, 0);

    while (!queue.empty()) {
        const auto [pieceIndex, depth] = queue.front();
        queue.pop_front();
        if (depth >= maxDepth) {
            continue;
        }
        const JigsawPiece piece = pieces[pieceIndex];
        const StructureTemplateDef* tmpl = manager.find(piece.templateId);
        if (tmpl == nullptr) {
            continue;
        }

        for (const StructureJigsawBlock& source : tmpl->jigsaws) {
            // The source jigsaw's world cell + front, then the cell the connecting
            // jigsaw must land in (one step along the front).
            const LocalPos local = rotateLocal({source.x, source.y, source.z}, tmpl->sizeX,
                                                tmpl->sizeZ, piece.rotation);
            const int srcX = piece.originX + local.x;
            const int srcY = piece.originY + local.y;
            const int srcZ = piece.originZ + local.z;
            const BlockOrientation srcFront = rotateOrientation(source.front, piece.rotation);
            const auto delta = step(srcFront);
            const int connX = srcX + delta[0];
            const int connY = srcY + delta[1];
            const int connZ = srcZ + delta[2];
            const BlockOrientation want = oppositeOrientation(srcFront);

            const data::StructurePoolDef* pool = manager.findPool(source.pool);
            if (pool == nullptr) {
                continue;
            }
            bool connected = false;
            for (const auto elementIndex : weightedOrder(pool->elements, rngState)) {
                const auto& element = pool->elements[elementIndex];
                const StructureTemplateDef* candidate = manager.find(element.location);
                if (candidate == nullptr) {
                    continue;
                }
                for (const StructureRotation rotation : rotationOrder(rngState)) {
                    for (const StructureJigsawBlock& target : candidate->jigsaws) {
                        if (target.name != source.target) {
                            continue;
                        }
                        if (rotateOrientation(target.front, rotation) != want) {
                            continue;
                        }
                        // Offset the candidate so its jigsaw lands in the connection cell.
                        const LocalPos tl = rotateLocal({target.x, target.y, target.z},
                                                        candidate->sizeX, candidate->sizeZ, rotation);
                        const int candX = connX - tl.x;
                        const int candY = connY - tl.y;
                        const int candZ = connZ - tl.z;
                        if (std::abs(candX - originX) > maxDistance ||
                            std::abs(candZ - originZ) > maxDistance) {
                            continue;
                        }
                        const Aabb box = boxOf(*candidate, candX, candY, candZ, rotation);
                        bool collides = false;
                        for (std::size_t boxIndex = 0; boxIndex < boxes.size(); ++boxIndex) {
                            // A child may overlap the piece it connects to — village
                            // streets carry their house connectors inside their own
                            // box, so the house sits on the street's plot (vanilla's
                            // use_expansion_hack). It must still clear every *other*
                            // placed piece.
                            if (boxIndex == pieceIndex) {
                                continue;
                            }
                            if (overlaps(box, boxes[boxIndex])) {
                                collides = true;
                                break;
                            }
                        }
                        if (collides) {
                            continue;
                        }
                        pieces.push_back({element.location, candX, candY, candZ, rotation});
                        boxes.push_back(box);
                        queue.emplace_back(pieces.size() - 1U, depth + 1);
                        connected = true;
                        break;
                    }
                    if (connected) {
                        break;
                    }
                }
                if (connected) {
                    break;
                }
            }
        }
    }
    return pieces;
}

} // namespace mc::world::gen
