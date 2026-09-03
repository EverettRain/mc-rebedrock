#pragma once

// AR-B4-5: which `block.<family>.open|close|click` event family a block's
// interaction sounds live under.
//
// It is a header of its own, and constexpr, for two reasons. It is pure data
// about blocks rather than anything to do with playing audio — AudioSystem just
// reads it — and keeping it out of AudioSystem.cpp means a test can assert the
// mapping without linking miniaudio and the whole sound-asset registry behind
// it. That matters here: the interesting claim is "an iron door sounds like
// iron", and before AR-B4-5 the answer was a string literal that said `wooden_door`
// for every door in the roster.

#include "world/Block.hpp"

#include <string_view>

namespace mc::audio {

[[nodiscard]] constexpr std::string_view interactionSoundFamily(world::Block block) {
    switch (world::blockDefinition(block).model) {
    case world::BlockModel::Door:
        // AR-B4-5: from the block's BlockSetType, not a literal. This used to
        // answer "wooden_door" for every door in the roster, so an iron door and
        // a copper one both creaked like oak.
        return world::blockSetTypeOf(block).doorSounds;
    case world::BlockModel::TrapDoor:
        return world::blockSetTypeOf(block).trapdoorSounds;
    case world::BlockModel::FenceGate:
        return "fence_gate";
    case world::BlockModel::Button:
        return "stone_button";
    case world::BlockModel::PressurePlate:
        // Only the stone plate exists in this roster; a wooden/weighted plate
        // would pick its own family here.
        return "stone_pressure_plate";
    default:
        break;
    }
    if (block == world::Block::Chest || block == world::Block::TrappedChest) {
        return "chest";
    }
    if (block == world::Block::Lever) {
        return "lever";
    }
    return {};
}

} // namespace mc::audio
