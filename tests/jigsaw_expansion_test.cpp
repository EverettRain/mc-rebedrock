// STRUCT-3b: the jigsaw expansion algorithm. What is pinned: the start piece is
// placed and a template from a jigsaw's target pool is connected face to face (its
// box adjacent, not overlapping); the `size` budget bounds how far the graph
// grows; a candidate that would collide with a placed piece is rejected; and the
// walk is deterministic in its rng state. Templates and pools are built directly
// (no NBT) so the geometry is exercised in isolation.

#include "gameplay/Random.hpp"
#include "world/StructureManager.hpp"
#include "world/StructureTemplate.hpp"
#include "world/gen/JigsawExpansion.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using mc::world::BlockOrientation;
using mc::world::StructureJigsawBlock;
using mc::world::StructureManager;
using mc::world::StructureTemplateDef;
using mc::world::gen::jigsawExpand;
using mc::world::gen::JigsawPiece;

StructureTemplateDef box(int sx, int sy, int sz, std::vector<StructureJigsawBlock> jigsaws) {
    StructureTemplateDef t;
    t.sizeX = sx;
    t.sizeY = sy;
    t.sizeZ = sz;
    t.jigsaws = std::move(jigsaws);
    return t;
}

mc::data::StructurePoolDef pool(std::string id, std::vector<std::string> locations) {
    mc::data::StructurePoolDef p;
    p.id = std::move(id);
    p.fallback = "minecraft:empty";
    for (auto& loc : locations) {
        mc::data::StructurePoolElement e;
        e.location = std::move(loc);
        e.weight = 1;
        p.elements.push_back(std::move(e));
    }
    return p;
}

// A start template with one East-facing jigsaw into `wingPool`, and a wing template
// with one West-facing jigsaw (name matches the start's target), dead-ending.
StructureManager villageLike(int wingSizeX = 1, int wingJigsawX = 0) {
    StructureManager m;
    m.add("minecraft:start_t",
          box(1, 1, 1, {{0, 0, 0, BlockOrientation::East, "s_out", "w_in", "minecraft:wing", "", false}}));
    m.add("minecraft:wing_t",
          box(wingSizeX, 1, 1,
              {{static_cast<std::int8_t>(wingJigsawX), 0, 0, BlockOrientation::West, "w_in", "dead",
                "minecraft:empty", "", false}}));
    m.addPool(pool("minecraft:start", {"minecraft:start_t"}));
    m.addPool(pool("minecraft:wing", {"minecraft:wing_t"}));
    return m;
}

void testBasicConnection() {
    const StructureManager m = villageLike();
    std::uint64_t rng = mc::rng::seedFromValue(1234ULL);
    const auto pieces = jigsawExpand(m, "minecraft:start", 0, 64, 0, 4, 80, rng);
    // start + one wing.
    assert(pieces.size() == 2);
    assert(pieces[0].templateId == "minecraft:start_t");
    assert(pieces[0].originX == 0 && pieces[0].originY == 64 && pieces[0].originZ == 0);
    assert(pieces[1].templateId == "minecraft:wing_t");
    // The wing sits exactly one cell from the start on a horizontal axis (a 1x1x1
    // connection), same Y — adjacent, not overlapping.
    const int dx = std::abs(pieces[1].originX);
    const int dz = std::abs(pieces[1].originZ);
    assert(pieces[1].originY == 64);
    assert((dx == 1 && dz == 0) || (dx == 0 && dz == 1));
}

void testDeterministic() {
    const StructureManager m = villageLike();
    std::uint64_t a = mc::rng::seedFromValue(99ULL);
    std::uint64_t b = mc::rng::seedFromValue(99ULL);
    const auto first = jigsawExpand(m, "minecraft:start", 0, 64, 0, 4, 80, a);
    const auto second = jigsawExpand(m, "minecraft:start", 0, 64, 0, 4, 80, b);
    assert(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        assert(first[i].templateId == second[i].templateId);
        assert(first[i].originX == second[i].originX && first[i].originZ == second[i].originZ);
        assert(first[i].rotation == second[i].rotation);
    }
}

void testSizeLimit() {
    const StructureManager m = villageLike();
    std::uint64_t rng = mc::rng::seedFromValue(7ULL);
    // maxDepth 0: the start piece is placed but no connection is expanded.
    const auto pieces = jigsawExpand(m, "minecraft:start", 0, 64, 0, 0, 80, rng);
    assert(pieces.size() == 1);
    assert(pieces[0].templateId == "minecraft:start_t");
}

void testChildMayOverlapParent() {
    // A wing whose matching jigsaw is at its far (+X) end extends back over the
    // start piece it connects to. A child is allowed to overlap the piece it
    // connects to (village streets carry their house connectors inside their own
    // box), so the wing is placed — 2 pieces, not refused.
    const StructureManager m = villageLike(/*wingSizeX=*/3, /*wingJigsawX=*/2);
    std::uint64_t rng = mc::rng::seedFromValue(3ULL);
    const auto pieces = jigsawExpand(m, "minecraft:start", 0, 64, 0, 4, 80, rng);
    assert(pieces.size() == 2);
}

void testNonParentCollisionRejected() {
    // start -> mid -> tail, where tail's connector is at its far end so it extends
    // back over the *start* piece. tail's parent is mid (overlap allowed), but the
    // start is not tail's parent, so the overlap with start is a real collision and
    // tail is refused: only start + mid remain.
    StructureManager m;
    // start (1x1x1): East connector into pool "a".
    m.add("minecraft:start_t",
          box(1, 1, 1, {{0, 0, 0, BlockOrientation::East, "s", "m_in", "minecraft:a", "", false}}));
    // mid (1x1x1): West connector (joins start's East) + East connector into "b".
    m.add("minecraft:mid_t",
          box(1, 1, 1,
              {{0, 0, 0, BlockOrientation::West, "m_in", "s", "minecraft:x", "", false},
               {0, 0, 0, BlockOrientation::East, "m_out", "t_in", "minecraft:b", "", false}}));
    // tail (3x1x1): its West connector is at the far +X end (x=2), so joining mid's
    // East makes it reach back to x=0 (over the start piece).
    m.add("minecraft:tail_t",
          box(3, 1, 1, {{2, 0, 0, BlockOrientation::West, "t_in", "dead", "minecraft:empty", "", false}}));
    m.addPool(pool("minecraft:start", {"minecraft:start_t"}));
    m.addPool(pool("minecraft:a", {"minecraft:mid_t"}));
    m.addPool(pool("minecraft:b", {"minecraft:tail_t"}));

    std::uint64_t rng = mc::rng::seedFromValue(5ULL);
    const auto pieces = jigsawExpand(m, "minecraft:start", 0, 64, 0, 4, 80, rng);
    // start at (0,0,0), mid at (1,0,0); tail would span x[0,3) overlapping start -> refused.
    assert(pieces.size() == 2);
    assert(pieces[0].templateId == "minecraft:start_t");
    assert(pieces[1].templateId == "minecraft:mid_t");
}

void testMissingPool() {
    StructureManager m;
    std::uint64_t rng = mc::rng::seedFromValue(1ULL);
    assert(jigsawExpand(m, "minecraft:nope", 0, 64, 0, 4, 80, rng).empty());
}

} // namespace

int main() {
    testBasicConnection();
    testDeterministic();
    testSizeLimit();
    testChildMayOverlapParent();
    testNonParentCollisionRejected();
    testMissingPool();
    return 0;
}
