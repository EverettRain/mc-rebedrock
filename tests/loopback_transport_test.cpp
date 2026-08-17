#include "net/LoopbackTransport.hpp"
#include "net/NetMessage.hpp"
#include "net/Transport.hpp"

#include "gameplay/GameCommand.hpp"
#include "gameplay/GameSession.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// Stage C, slice 1: the in-process loopback transport and the single message
// stream over it. These are the only "network" primitives in the client/server
// split; this test proves them headless — the byte layer round-trips every
// message category, a mixed stream splits back by frame, the two loopback ends
// carry frames only to the peer, and a real GameSession runs the command→tick→
// snapshot path over the channel exactly as the renderer will over its two
// threads. No GLFW, no Vulkan.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"loopback_transport_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc;

// A do-nothing SimulationHost so a GameSession can tick headless. The end-to-end
// case drives interaction that raises no side effects, so every reaction is a
// no-op.
struct NullHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// Each message category survives encode → decode with its value intact, routed
// back to the right boundary purely by its frame tag.
void testMessageRoundTrip() {
    const net::NetMessage command{gameplay::GameCommand{gameplay::SwapSlot{5U}}};
    const net::NetMessage snapshotMessage{
        gameplay::PublishedSnapshot{[] {
            gameplay::PlayerTickSnapshot snapshot;
            snapshot.serverTick = 42U;
            snapshot.selectedHotbarSlot = 7U;
            return snapshot;
        }()}};
    const net::NetMessage eventMessage{gameplay::GameEvent{gameplay::SoundEvent{
        gameplay::SoundEventKind::BlockBreak, glm::vec3{1.0F, 2.0F, 3.0F}, world::Block::Stone}}};

    for (const auto& message : {command, snapshotMessage, eventMessage}) {
        const auto bytes = net::encodeMessage(message);
        REQUIRE(!bytes.empty());
        REQUIRE(net::encodedMessageSize(bytes) == bytes.size());
        const auto decoded = net::decodeMessage(bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->index() == message.index());
        REQUIRE(*decoded == message);
    }

    // The frame tag alone decides the boundary: a command decodes as a command,
    // a snapshot as a snapshot, an event as an event.
    REQUIRE(std::holds_alternative<gameplay::GameCommand>(*net::decodeMessage(
        net::encodeMessage(command))));
    REQUIRE(std::holds_alternative<gameplay::PublishedSnapshot>(*net::decodeMessage(
        net::encodeMessage(snapshotMessage))));
    REQUIRE(std::holds_alternative<gameplay::GameEvent>(*net::decodeMessage(
        net::encodeMessage(eventMessage))));
}

// A concatenation of frames from all three categories splits back into the
// original messages, in order, using encodedMessageSize to find each frame — the
// mixed stream the transport carries.
void testMixedStreamSplits() {
    const std::vector<net::NetMessage> sent{
        net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{2U}}},
        net::NetMessage{gameplay::PublishedSnapshot{[] {
            gameplay::WorldSnapshot snapshot;
            snapshot.dayTimeTicks = 1200.0;
            return snapshot;
        }()}},
        net::NetMessage{gameplay::GameEvent{gameplay::WorldEditEvent{
            4, 5, 6, world::BlockState{world::Block::Stone}, true}}},
        net::NetMessage{gameplay::GameCommand{gameplay::UseItem{gameplay::InteractionHand::Main}}},
    };

    std::vector<std::uint8_t> stream;
    for (const auto& message : sent) {
        const auto frame = net::encodeMessage(message);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    std::vector<net::NetMessage> received;
    std::size_t cursor = 0;
    while (cursor < stream.size()) {
        const std::span<const std::uint8_t> remaining{stream.data() + cursor,
                                                      stream.size() - cursor};
        const auto frameSize = net::encodedMessageSize(remaining);
        REQUIRE(frameSize > 0);
        const auto message = net::decodeMessage(remaining);
        REQUIRE(message.has_value());
        received.push_back(*message);
        cursor += frameSize;
    }

    REQUIRE(cursor == stream.size());
    REQUIRE(received.size() == sent.size());
    for (std::size_t index = 0; index < sent.size(); ++index) {
        REQUIRE(received[index] == sent[index]);
    }
}

// The loopback pair is directional: a frame a client sends reaches only the
// server end, and the reverse.
void testLoopbackDirectionality() {
    auto pair = net::makeLoopbackPair();

    pair.client->sendFrame({1U, 2U, 3U});
    std::vector<std::uint8_t> frame;
    // The client does not receive its own send.
    REQUIRE(!pair.client->receiveFrame(frame));
    // The server does.
    REQUIRE(pair.server->receiveFrame(frame));
    REQUIRE((frame == std::vector<std::uint8_t>{1U, 2U, 3U}));
    // And the queue is now empty.
    REQUIRE(!pair.server->receiveFrame(frame));

    // The reverse direction is independent.
    pair.server->sendFrame({9U});
    REQUIRE(!pair.server->receiveFrame(frame));
    REQUIRE(pair.client->receiveFrame(frame));
    REQUIRE((frame == std::vector<std::uint8_t>{9U}));
}

// The typed helpers frame and unframe a NetMessage across the channel.
void testTypedRoundTripOverChannel() {
    auto pair = net::makeLoopbackPair();

    const net::NetMessage command{gameplay::GameCommand{gameplay::SwapSlot{4U}}};
    net::sendMessage(*pair.client, command);
    std::optional<net::NetMessage> received;
    REQUIRE(net::receiveMessage(*pair.server, received));
    REQUIRE(received.has_value());
    REQUIRE(*received == command);

    gameplay::WorldSnapshot world;
    world.serverTick = 99U;
    const net::NetMessage snapshotMessage{gameplay::PublishedSnapshot{world}};
    net::sendMessage(*pair.server, snapshotMessage);
    received.reset();
    REQUIRE(net::receiveMessage(*pair.client, received));
    REQUIRE(received.has_value());
    REQUIRE(*received == snapshotMessage);

    // An empty channel yields nothing.
    REQUIRE(!net::receiveMessage(*pair.client, received));
}

// A frame with an unknown tag (a newer peer's message) is consumed on receive
// and reported as an empty decode, so a poll loop makes progress instead of
// stalling on it — the same forward-compatibility the codecs already prove.
void testUnknownTagIsConsumed() {
    std::vector<std::uint8_t> frame;
    gameplay::codec::appendFrame(frame, 200U, [&] {
        frame.push_back(0xAAU);
        frame.push_back(0xBBU);
    });
    REQUIRE(net::encodedMessageSize(frame) == frame.size());
    REQUIRE(!net::decodeMessage(frame).has_value());

    auto pair = net::makeLoopbackPair();
    pair.client->sendFrame(frame);
    std::optional<net::NetMessage> received;
    // receiveMessage still returns true (a frame was consumed) with no value.
    REQUIRE(net::receiveMessage(*pair.server, received));
    REQUIRE(!received.has_value());
}

// The queue preserves order and stays race-free under a producer thread sending
// while a consumer thread receives — the sim/render split's actual shape. (Its
// data-race freedom is what mac's -fsanitize=thread run confirms; here the
// assertion is the ordering and the completeness.)
void testThreadedOrdering() {
    auto pair = net::makeLoopbackPair();
    constexpr std::size_t kCount = 2000U;

    std::thread producer{[&] {
        for (std::size_t index = 0; index < kCount; ++index) {
            net::sendMessage(*pair.client,
                             net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{index}}});
        }
    }};

    std::vector<std::size_t> receivedIndices;
    receivedIndices.reserve(kCount);
    while (receivedIndices.size() < kCount) {
        std::optional<net::NetMessage> message;
        if (!net::receiveMessage(*pair.server, message)) {
            continue;
        }
        REQUIRE(message.has_value());
        const auto& command = std::get<gameplay::GameCommand>(*message);
        receivedIndices.push_back(std::get<gameplay::SwapSlot>(command).index);
    }
    producer.join();

    REQUIRE(receivedIndices.size() == kCount);
    for (std::size_t index = 0; index < kCount; ++index) {
        REQUIRE(receivedIndices[index] == index);
    }
}

// The whole point of slice 1: single-player runs the message path. A client end
// ships a SwapSlot; the server end drains its channel into the authoritative
// GameSession, ticks it, then publishes the resulting snapshot back over the
// channel; the client decodes it into its mirror. The hotbar selection the
// command asked for is what the mirror shows — end to end, over bytes, with no
// direct enqueueCommand or direct snapshot read.
void testEndToEndThroughSession() {
    auto pair = net::makeLoopbackPair();

    world::World world;
    gameplay::GameSession session;
    NullHost host;

    // Client: the player picked hotbar slot 3.
    net::sendMessage(*pair.client,
                     net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{3U}}});

    // Server: drain the channel into the session's command queue.
    std::optional<net::NetMessage> incoming;
    while (net::receiveMessage(*pair.server, incoming)) {
        REQUIRE(incoming.has_value());
        REQUIRE(std::holds_alternative<gameplay::GameCommand>(*incoming));
        session.enqueueCommand(std::get<gameplay::GameCommand>(*incoming));
    }

    // Server: advance one authoritative tick, which applies the command and
    // publishes fresh snapshots, then ship the player snapshot back.
    session.tick(world, host);
    net::sendMessage(*pair.server,
                     net::NetMessage{gameplay::PublishedSnapshot{session.playerTickSnapshot()}});

    // Client: decode the mirror and read the selection from it, not from the
    // session.
    gameplay::PlayerTickSnapshot mirror;
    REQUIRE(net::receiveMessage(*pair.client, incoming));
    REQUIRE(incoming.has_value());
    const auto& snapshot = std::get<gameplay::PublishedSnapshot>(*incoming);
    REQUIRE(std::holds_alternative<gameplay::PlayerTickSnapshot>(snapshot));
    mirror = std::get<gameplay::PlayerTickSnapshot>(snapshot);

    REQUIRE(mirror.selectedHotbarSlot == 3U);
    // The mirror is a faithful copy of what the authoritative session published.
    REQUIRE(mirror == session.playerTickSnapshot());
}

}  // namespace

int main() {
    testMessageRoundTrip();
    testMixedStreamSplits();
    testLoopbackDirectionality();
    testTypedRoundTripOverChannel();
    testUnknownTagIsConsumed();
    testThreadedOrdering();
    testEndToEndThroughSession();
    return 0;
}
