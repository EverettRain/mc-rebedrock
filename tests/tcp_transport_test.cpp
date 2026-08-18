#include "net/NetMessage.hpp"
#include "net/TcpTransport.hpp"
#include "net/Transport.hpp"

#include "gameplay/GameCommand.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/StreamCodec.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// Stage C, §8.2 / D1: the TCP transport. The loopback test proved the message
// path headless over an in-process queue; this one proves the same MessageChannel
// abstraction over a real 127.0.0.1 socket — that TcpChannel absorbs the one thing
// the socket does and the queue does not (a byte stream with no message
// boundaries: half frames and coalesced frames), and that the exact command→tick→
// snapshot session path runs over bytes on the wire with GameRuntime/ClientMirror
// unaware they are talking to a socket. No GLFW, no Vulkan. Single process, two
// threads, one connection.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"tcp_transport_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc;

// A do-nothing SimulationHost so a GameSession can tick headless (same as the
// loopback test: the interaction the end-to-end case drives raises no side
// effects).
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

// A connected pair over 127.0.0.1: a listener accepts on a background thread
// while the client connects, so the two ends come back already wired. This is
// D1's "one server thread + one render thread over a real socket" in miniature.
struct SocketPair {
    std::unique_ptr<net::TcpChannel> client;
    std::unique_ptr<net::TcpChannel> server;
};

SocketPair connectPair() {
    auto listener = std::make_shared<net::TcpListener>();
    const std::uint16_t port = listener->port();
    REQUIRE(port != 0);
    std::future<std::unique_ptr<net::TcpChannel>> accepted =
        std::async(std::launch::async, [listener] { return listener->accept(); });
    auto client = net::TcpChannel::connect(port);
    auto server = accepted.get();
    return SocketPair{std::move(client), std::move(server)};
}

// Blocks (bounded) for the next frame to arrive on a channel, since the socket
// path is asynchronous — the IO thread reassembles a frame some time after the
// peer sent it, unlike the loopback queue which is ready the instant send
// returns. Fails the test rather than hanging if nothing arrives.
std::vector<std::uint8_t> awaitFrame(net::TcpChannel& channel) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    std::vector<std::uint8_t> frame;
    while (std::chrono::steady_clock::now() < deadline) {
        if (channel.receiveFrame(frame)) {
            return frame;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    throw std::runtime_error{"tcp_transport_test: timed out waiting for a frame"};
}

bool awaitMessage(net::TcpChannel& channel, std::optional<net::NetMessage>& out) {
    const auto frame = awaitFrame(channel);
    out = net::decodeMessage(frame);
    return true;
}

// A typed NetMessage round-trips over the real socket, decoded on the far end to
// exactly the value that was sent — the same assertion the loopback typed
// round-trip makes, now with a TCP hop in between.
void testTypedRoundTripOverSocket() {
    auto pair = connectPair();

    const net::NetMessage command{gameplay::GameCommand{gameplay::SwapSlot{4U}}};
    net::sendMessage(*pair.client, command);
    std::optional<net::NetMessage> received;
    REQUIRE(awaitMessage(*pair.server, received));
    REQUIRE(received.has_value());
    REQUIRE(*received == command);

    gameplay::WorldSnapshot world;
    world.serverTick = 99U;
    const net::NetMessage snapshotMessage{gameplay::PublishedSnapshot{world}};
    net::sendMessage(*pair.server, snapshotMessage);
    received.reset();
    REQUIRE(awaitMessage(*pair.client, received));
    REQUIRE(received.has_value());
    REQUIRE(*received == snapshotMessage);
}

// Many frames sent back to back arrive whole, in order, and none merge or split —
// the receiver sees exactly the frames the sender framed even though TCP is free
// to coalesce them into arbitrary reads. This is the reassembly the loopback
// never had to do.
void testManyFramesReassembleInOrder() {
    auto pair = connectPair();
    constexpr std::size_t kCount = 4000U;

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
        std::vector<std::uint8_t> frame;
        if (!pair.server->receiveFrame(frame)) {
            std::this_thread::yield();
            continue;
        }
        message = net::decodeMessage(frame);
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

// Opens a raw blocking socket connected to the listener — the sender side of the
// fragmentation test, where we control exactly how the frame bytes are split into
// segments (which TcpChannel's own sendFrame, sending whole frames, cannot do).
int connectRawClient(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

void rawWriteAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        REQUIRE(written > 0);
        offset += static_cast<std::size_t>(written);
    }
}

// The pointed test of reassembly: a raw client writes a frame one byte at a time
// — each byte its own segment, worst-case fragmentation — and the TcpChannel
// server still delivers exactly one whole frame, holding the partial back until
// its last byte lands. Then it writes two frames' bytes coalesced into a single
// write and the channel splits them back into exactly two whole frames. This is
// the half-packet/coalesced-packet case the loopback queue never faces.
void testHalfPacketAndCoalescedReassembly() {
    auto listener = std::make_shared<net::TcpListener>();
    const std::uint16_t port = listener->port();
    std::future<std::unique_ptr<net::TcpChannel>> accepted =
        std::async(std::launch::async, [listener] { return listener->accept(); });
    const int rawClient = connectRawClient(port);
    auto server = accepted.get();

    // Frame 1, dribbled a byte at a time. With TCP_NODELAY on the accepted socket
    // and one-byte writes, the server sees a genuinely torn stream.
    const auto frameA = net::encodeMessage(
        net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{7U}}});
    REQUIRE(frameA.size() > gameplay::codec::kFrameHeaderBytes);
    for (std::size_t index = 0; index < frameA.size(); ++index) {
        rawWriteAll(rawClient, frameA.data() + index, 1);
        // Before the final byte, no whole frame can exist; assert the channel
        // holds it back rather than surfacing a torn frame.
        if (index + 1 < frameA.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            std::vector<std::uint8_t> premature;
            REQUIRE(!server->receiveFrame(premature));
        }
    }
    const auto arrivedA = awaitFrame(*server);
    REQUIRE(arrivedA == frameA);

    // Frames 2 and 3 written coalesced in one syscall; the channel must split
    // them back into two whole frames.
    const auto frameB = net::encodeMessage(
        net::NetMessage{gameplay::GameCommand{gameplay::UseItem{gameplay::InteractionHand::Main}}});
    const auto frameC = net::encodeMessage(
        net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{9U}}});
    std::vector<std::uint8_t> coalesced;
    coalesced.insert(coalesced.end(), frameB.begin(), frameB.end());
    coalesced.insert(coalesced.end(), frameC.begin(), frameC.end());
    rawWriteAll(rawClient, coalesced.data(), coalesced.size());

    const auto arrivedB = awaitFrame(*server);
    REQUIRE(arrivedB == frameB);
    const auto arrivedC = awaitFrame(*server);
    REQUIRE(arrivedC == frameC);

    ::close(rawClient);
}

// An unknown tag (a newer peer's message) crossing the socket is consumed and
// reported as an empty decode, so a poll loop makes progress — the same
// forward-compatibility the loopback proves, verified through the reassembler.
void testUnknownTagOverSocket() {
    auto pair = connectPair();

    std::vector<std::uint8_t> frame;
    gameplay::codec::appendFrame(frame, 250U, [&] {
        frame.push_back(0xAAU);
        frame.push_back(0xBBU);
        frame.push_back(0xCCU);
    });
    REQUIRE(net::encodedMessageSize(frame) == frame.size());
    pair.client->sendFrame(frame);

    const auto arrived = awaitFrame(*pair.server);
    REQUIRE(arrived == frame);              // The bytes reassembled exactly.
    REQUIRE(!net::decodeMessage(arrived).has_value());  // But decode to nothing.
}

// closed() flips once the peer's channel is destroyed (its socket closed), and
// frames already reassembled before the close stay drainable — an orderly
// disconnect the handshake layer will act on.
void testPeerCloseIsObserved() {
    auto pair = connectPair();

    net::sendMessage(*pair.client,
                     net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{2U}}});
    // Make sure the frame is delivered before we drop the client.
    std::optional<net::NetMessage> received;
    REQUIRE(awaitMessage(*pair.server, received));
    REQUIRE(received.has_value());

    pair.client.reset();  // Destroy the client end: closes its socket.

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!pair.server->closed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(pair.server->closed());
}

// The whole point of D1: the command→tick→snapshot session path runs over a real
// socket. A client ships a SwapSlot; the server drains its channel into the
// authoritative GameSession, ticks it, publishes the snapshot back over the
// socket; the client decodes it into its mirror. The selection the mirror shows
// is the one the command asked for — end to end, over bytes on the wire, with no
// direct enqueueCommand and no direct snapshot read. This is the exact assertion
// the loopback end-to-end test makes, now with TCP as the transport, proving the
// MessageChannel abstraction holds unchanged.
void testEndToEndThroughSessionOverSocket() {
    auto pair = connectPair();

    world::World world;
    gameplay::GameSession session;
    NullHost host;

    // Client: the player picked hotbar slot 3.
    net::sendMessage(*pair.client,
                     net::NetMessage{gameplay::GameCommand{gameplay::SwapSlot{3U}}});

    // Server: drain the socket into the session's command queue.
    std::optional<net::NetMessage> incoming;
    REQUIRE(awaitMessage(*pair.server, incoming));
    REQUIRE(incoming.has_value());
    REQUIRE(std::holds_alternative<gameplay::GameCommand>(*incoming));
    session.enqueueCommand(std::get<gameplay::GameCommand>(*incoming));

    // Server: one authoritative tick applies the command and publishes snapshots;
    // ship the player snapshot back over the socket.
    session.tick(world, host);
    net::sendMessage(*pair.server,
                     net::NetMessage{gameplay::PublishedSnapshot{session.playerTickSnapshot()}});

    // Client: decode the mirror and read the selection from it, not the session.
    std::optional<net::NetMessage> mirrorMessage;
    REQUIRE(awaitMessage(*pair.client, mirrorMessage));
    REQUIRE(mirrorMessage.has_value());
    const auto& snapshot = std::get<gameplay::PublishedSnapshot>(*mirrorMessage);
    REQUIRE(std::holds_alternative<gameplay::PlayerTickSnapshot>(snapshot));
    const auto mirror = std::get<gameplay::PlayerTickSnapshot>(snapshot);

    REQUIRE(mirror.selectedHotbarSlot == 3U);
    REQUIRE(mirror == session.playerTickSnapshot());
}

}  // namespace

int main() {
    testTypedRoundTripOverSocket();
    testManyFramesReassembleInOrder();
    testHalfPacketAndCoalescedReassembly();
    testUnknownTagOverSocket();
    testPeerCloseIsObserved();
    testEndToEndThroughSessionOverSocket();
    return 0;
}
