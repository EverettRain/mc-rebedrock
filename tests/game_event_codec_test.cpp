// N4 message-boundary slice 3: the side-effect events (world edits, sounds,
// particles, the player's death) survive a byte round trip — including the
// block-state property palette and the species identifier — split back out of a
// stream, and skip an unknown tag by size, the same forward-compatibility as
// the command and snapshot codecs.

#include "gameplay/GameEventCodec.hpp"

#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using namespace mc;

namespace {

void checkRoundTrip(const gameplay::GameEvent& event) {
    const auto bytes = gameplay::encodeGameEvent(event);
    const auto decoded = gameplay::decodeGameEvent(bytes);
    assert(decoded.has_value());
    assert(event == *decoded);
}

}  // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    // --- A world edit with a property-carrying state (a lit furnace, a mature
    // wheat crop, water with a fluid level) round-trips. ---
    checkRoundTrip(gameplay::GameEvent{gameplay::WorldEditEvent{
        4, 64, 8,
        world::BlockState{world::Block::Furnace, world::BlockOrientation::West}.withLit(true),
        true}});
    checkRoundTrip(gameplay::GameEvent{gameplay::WorldEditEvent{
        -3, 63, 2, world::BlockState{world::Block::WheatCrops}.withAge(5), false}});
    checkRoundTrip(gameplay::GameEvent{gameplay::WorldEditEvent{
        0, 60, 0, world::BlockState{world::Block::Water,
                                    world::BlockOrientation::North, 3U},
        false}});
    // A state with no declared properties (plain stone) round-trips too.
    checkRoundTrip(gameplay::GameEvent{
        gameplay::WorldEditEvent{1, 2, 3, world::BlockState{world::Block::Stone}, false}});
    // A slab's SlabType must survive: a hand-listed property set once dropped it,
    // so a top or double slab reached the client mesh as a bottom one. Each half
    // round-trips through the schema-driven codec.
    for (const auto portion : {world::SlabPortion::Bottom, world::SlabPortion::Top,
                               world::SlabPortion::Double}) {
        checkRoundTrip(gameplay::GameEvent{gameplay::WorldEditEvent{
            4, 5, 6, world::BlockState{world::Block::OakSlab}.withSlabPortion(portion), true}});
    }

    // --- A creature sound round-trips its species by identifier. ---
    checkRoundTrip(gameplay::GameEvent{gameplay::SoundEvent{
        gameplay::SoundEventKind::CreatureStep, {10.5F, 64.0F, -8.0F}, world::Block::Stone,
        &gameplay::entities::builtinSpecies("pig"), 0.8F, false}});
    // A block sound with no species (null) round-trips as empty.
    checkRoundTrip(gameplay::GameEvent{gameplay::SoundEvent{
        gameplay::SoundEventKind::BlockBreak, {1.0F, 2.0F, 3.0F}, world::Block::OakLog, nullptr,
        1.0F, false}});
    // A heavy fall carries the heavy flag.
    checkRoundTrip(gameplay::GameEvent{gameplay::SoundEvent{
        gameplay::SoundEventKind::PlayerFall, {0.0F, 40.0F, 0.0F}, world::Block::Grass, nullptr,
        1.0F, true}});

    // --- A particle event round-trips. ---
    checkRoundTrip(gameplay::GameEvent{gameplay::ParticleEvent{
        gameplay::ParticleEventKind::BlockBreak, glm::ivec3{5, 65, 5}, world::Block::Dirt}});

    // --- The death fact round-trips (empty payload). ---
    checkRoundTrip(gameplay::GameEvent{gameplay::PlayerDiedEvent{}});
    checkRoundTrip(gameplay::GameEvent{gameplay::ClientActionEvent{
        gameplay::ClientActionEventKind::OpenContainer,
        gameplay::ContainerScreen::Furnace, {4, 65, -2}, true}});

    // --- Two events in one stream split back at their frame boundaries. ---
    {
        const gameplay::GameEvent first{gameplay::SoundEvent{
            gameplay::SoundEventKind::ItemPickup, {1.0F, 2.0F, 3.0F}, world::Block::Stone, nullptr,
            1.0F, false}};
        const gameplay::GameEvent second{gameplay::PlayerDiedEvent{}};
        auto stream = gameplay::encodeGameEvent(first);
        const auto secondBytes = gameplay::encodeGameEvent(second);
        const auto firstSize = stream.size();
        stream.insert(stream.end(), secondBytes.begin(), secondBytes.end());

        const auto boundary = gameplay::encodedGameEventSize(stream);
        assert(boundary == firstSize);
        const auto decodedFirst = gameplay::decodeGameEvent(std::span{stream.data(), boundary});
        assert(decodedFirst.has_value() && first == *decodedFirst);
        const auto decodedSecond = gameplay::decodeGameEvent(
            std::span{stream.data() + boundary, stream.size() - boundary});
        assert(decodedSecond.has_value() && second == *decodedSecond);
    }

    // --- An unknown event tag is skipped by size, not fatal. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(99U);
        persistence::appendInteger(bytes, std::uint32_t{3});
        bytes.push_back(1);
        bytes.push_back(2);
        bytes.push_back(3);
        assert(gameplay::encodedGameEventSize(bytes) == 5U + 3U);
        assert(!gameplay::decodeGameEvent(bytes).has_value());
    }

    // --- A truncated frame reports an incomplete size and refuses to decode. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(15U);
        persistence::appendInteger(bytes, std::uint32_t{1000U});
        bytes.push_back(1);
        assert(gameplay::encodedGameEventSize(bytes) == 0U);
        assert(!gameplay::decodeGameEvent(bytes).has_value());
    }

    std::cout << "PASS: game_event_codec_test\n";
    return 0;
}
