// AR-B4-5: BlockSetType — the record a door/trapdoor is built from in vanilla,
// answering what depends on the *material* rather than the shape.
//
// Two things were wrong before it existed. An iron door opened to a bare hand,
// which removes the entire reason to build one: in vanilla it answers redstone
// and nothing else. And every door in the roster played `block.wooden_door.*`,
// so iron and copper creaked like oak.

#include "audio/BlockInteractionSounds.hpp"
#include "world/Block.hpp"

#include <cassert>
#include <string_view>

namespace {

using mc::world::Block;
using mc::world::BlockSetTypeId;

} // namespace

int main() {
    // --- The table is what vanilla's BlockSetType says. IRON is the only one of
    // the three that refuses a hand (BlockSetType.java:29-35 — `canOpenByHand`
    // is the second field, false for iron, true for copper and every wood). ---
    {
        assert(!mc::world::kBlockSetTypes[static_cast<std::size_t>(BlockSetTypeId::Iron)]
                    .canOpenByHand);
        assert(mc::world::kBlockSetTypes[static_cast<std::size_t>(BlockSetTypeId::Copper)]
                   .canOpenByHand);
        assert(mc::world::kBlockSetTypes[static_cast<std::size_t>(BlockSetTypeId::Wood)]
                   .canOpenByHand);
    }

    // --- Every openable in the roster is filed under the right material. The
    // list is spelled out rather than derived from the block's name, so that a
    // block named "iron_something" that is not iron, or a copper variant added
    // without a set type, shows up here instead of behaving like oak. ---
    {
        for (const Block wood : {Block::OakDoor, Block::SpruceDoor, Block::JungleDoor,
                                 Block::AcaciaDoor, Block::DarkOakDoor, Block::OakTrapdoor,
                                 Block::SpruceTrapdoor, Block::JungleTrapdoor}) {
            assert(mc::world::blockDefinition(wood).setType == BlockSetTypeId::Wood);
            assert(mc::world::blockSetTypeOf(wood).canOpenByHand);
        }
        for (const Block iron : {Block::IronDoor, Block::IronTrapdoor}) {
            assert(mc::world::blockDefinition(iron).setType == BlockSetTypeId::Iron);
            assert(!mc::world::blockSetTypeOf(iron).canOpenByHand);
        }
        for (const Block copper :
             {Block::WaxedCopperDoor, Block::WaxedOxidizedCopperDoor,
              Block::OxidizedCopperTrapdoor, Block::WaxedOxidizedCopperTrapdoor}) {
            assert(mc::world::blockDefinition(copper).setType == BlockSetTypeId::Copper);
            // Copper is *not* iron: it opens by hand, which is exactly the
            // distinction a "metal doors are redstone-only" shortcut would lose.
            assert(mc::world::blockSetTypeOf(copper).canOpenByHand);
        }
    }

    // --- The sound families differ by material, and the door and trapdoor
    // families differ from each other within a material. ---
    {
        assert(mc::world::blockSetTypeOf(Block::OakDoor).doorSounds == "wooden_door");
        assert(mc::world::blockSetTypeOf(Block::IronDoor).doorSounds == "iron_door");
        assert(mc::world::blockSetTypeOf(Block::WaxedCopperDoor).doorSounds == "copper_door");
        assert(mc::world::blockSetTypeOf(Block::OakTrapdoor).trapdoorSounds == "wooden_trapdoor");
        assert(mc::world::blockSetTypeOf(Block::IronTrapdoor).trapdoorSounds == "iron_trapdoor");
        assert(mc::world::blockSetTypeOf(Block::OxidizedCopperTrapdoor).trapdoorSounds ==
               "copper_trapdoor");
        // No two set types share a door family — otherwise "sounds are per
        // material" would be true of the table and false in effect.
        assert(mc::world::blockSetTypeOf(Block::OakDoor).doorSounds !=
               mc::world::blockSetTypeOf(Block::IronDoor).doorSounds);
        assert(mc::world::blockSetTypeOf(Block::IronDoor).doorSounds !=
               mc::world::blockSetTypeOf(Block::WaxedCopperDoor).doorSounds);
    }

    // --- The consumer, not just the table. Asserting the table alone would pass
    // while AudioSystem still returned a hard-coded "wooden_door" for every
    // door, which is exactly the bug this replaced. ---
    {
        assert(mc::audio::interactionSoundFamily(Block::OakDoor) == "wooden_door");
        assert(mc::audio::interactionSoundFamily(Block::IronDoor) == "iron_door");
        assert(mc::audio::interactionSoundFamily(Block::WaxedCopperDoor) == "copper_door");
        assert(mc::audio::interactionSoundFamily(Block::OakTrapdoor) == "wooden_trapdoor");
        assert(mc::audio::interactionSoundFamily(Block::IronTrapdoor) == "iron_trapdoor");
        assert(mc::audio::interactionSoundFamily(Block::OxidizedCopperTrapdoor) ==
               "copper_trapdoor");
        // The families that are not BlockSetType's business are untouched: a
        // fence gate is built from a WoodType in vanilla, not a BlockSetType,
        // and there is no iron one.
        assert(mc::audio::interactionSoundFamily(Block::OakFenceGate) == "fence_gate");
        assert(mc::audio::interactionSoundFamily(Block::Chest) == "chest");
        assert(mc::audio::interactionSoundFamily(Block::Stone).empty());
    }

    // --- Nothing else in the roster accidentally acquired a non-wood set type
    // (the field defaults to Wood and only the openables should set it). ---
    {
        for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
            const auto block = static_cast<Block>(i);
            const auto model = mc::world::blockDefinition(block).model;
            if (model != mc::world::BlockModel::Door &&
                model != mc::world::BlockModel::TrapDoor) {
                assert(mc::world::blockDefinition(block).setType == BlockSetTypeId::Wood);
            }
        }
    }

    return 0;
}
