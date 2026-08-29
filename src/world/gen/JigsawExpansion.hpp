#pragma once

// STRUCT-3b: the jigsaw expansion algorithm — the graph walk that assembles a
// village (or any jigsaw structure) out of templates connected at their jigsaw
// blocks. Given a start pool, it places the start piece, then breadth-first
// connects a template drawn from each open jigsaw's target pool, rotating and
// offsetting it so the two jigsaw blocks meet face to face, rejecting a placement
// that would collide with a piece already down or stray past the size / distance
// budget.
//
// The DOD form of Java's JigsawPlacement.addPieces: no `Holder`/`Either`/
// `MutableObject`/boxed weighted list and no recursion — a flat `std::deque` work
// queue over indices into a flat `pieces` vector, weighted picks drawn from
// `mc::rng`. Determinism is the STRUCT rule: the same seed assembles the same
// village. This produces the *layout* (which template, where, which rotation);
// STRUCT-3c stamps each piece with the STRUCT-2 placer.

#include "world/StructureManager.hpp"
#include "world/StructureRotation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mc::world::gen {

// One placed piece of an assembled jigsaw structure: which template, its world
// origin (the template's local (0,0,0) after placement), and its rotation. Stamp
// with placeStructure(template, origin, rotation).
struct JigsawPiece final {
    std::string templateId;
    int originX = 0;
    int originY = 0;
    int originZ = 0;
    StructureRotation rotation = StructureRotation::None;
};

// Assembles the structure whose start pool is `startPoolId`, with its start piece
// origin at (originX, originY, originZ). `maxDepth` is the structure's `size` (how
// many connection hops out from the start), `maxDistance` the block radius pieces
// must stay within (`max_distance_from_center`). Returns every placed piece,
// starting with the start piece; empty when the start pool/template is missing.
// `rngState` is advanced deterministically.
[[nodiscard]] std::vector<JigsawPiece> jigsawExpand(const StructureManager& manager,
                                                    std::string_view startPoolId, int originX,
                                                    int originY, int originZ, int maxDepth,
                                                    int maxDistance, std::uint64_t& rngState);

} // namespace mc::world::gen
