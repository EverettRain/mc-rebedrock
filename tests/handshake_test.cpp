#include "net/Handshake.hpp"
#include "net/LoopbackTransport.hpp"
#include "net/TcpTransport.hpp"
#include "net/Transport.hpp"

#include "gameplay/BlockIdRemap.hpp"
#include "gameplay/StreamCodec.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Stage C, §8.2 / D2: the connection handshake. These prove the login gate
// headless — the two hello messages round-trip through their codec, matching
// protocol versions accept over both the loopback pair and a real socket (so the
// handshake is transport-agnostic, the same abstraction claim D1 made), a version
// mismatch is refused with the server's version handed back, a non-rebedrock peer
// is refused, and a silent peer times out rather than hanging. No GLFW, no Vulkan.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"handshake_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc;

// Both hellos survive encode → decode intact, and decode rejects a frame that is
// not the hello it expects (wrong tag) or is truncated.
void testHelloCodecRoundTrip() {
    const net::ClientHello client{net::kAppMagic, 7U};
    const auto clientBytes = net::encodeClientHello(client);
    const auto decodedClient = net::decodeClientHello(clientBytes);
    REQUIRE(decodedClient.has_value());
    REQUIRE(*decodedClient == client);

    const net::ServerHello server{false, 3U, "incompatible client"};
    const auto serverBytes = net::encodeServerHello(server);
    const auto decodedServer = net::decodeServerHello(serverBytes);
    REQUIRE(decodedServer.has_value());
    REQUIRE(*decodedServer == server);

    // A server hello is not a client hello and vice versa: the tag guards it.
    REQUIRE(!net::decodeClientHello(serverBytes).has_value());
    REQUIRE(!net::decodeServerHello(clientBytes).has_value());
    // A truncated frame decodes to nothing rather than throwing out.
    REQUIRE(!net::decodeClientHello(std::span<const std::uint8_t>{clientBytes.data(), 3}).has_value());
}

// Matching versions accept, over the in-process loopback pair. The two sides run
// concurrently (the exchange is a request/response), which is the real client
// thread / server thread topology.
void testMatchingVersionsAcceptOverLoopback() {
    auto pair = net::makeLoopbackPair();
    auto serverSide = std::async(std::launch::async,
                                 [&] { return net::performServerHandshake(*pair.server); });
    const auto clientResult = net::performClientHandshake(*pair.client);
    const auto serverResult = serverSide.get();

    REQUIRE(clientResult.ok());
    REQUIRE(serverResult.ok());
    REQUIRE(clientResult.peerProtocolVersion == net::kProtocolVersion);
    REQUIRE(serverResult.peerProtocolVersion == net::kProtocolVersion);
}

// A connected TCP pair over 127.0.0.1, the D1 primitive reused so the handshake
// runs over a real socket exactly as it runs over the queue.
struct SocketPair {
    std::unique_ptr<net::TcpChannel> client;
    std::unique_ptr<net::TcpChannel> server;
};

SocketPair connectPair() {
    auto listener = std::make_shared<net::TcpListener>();
    const std::uint16_t port = listener->port();
    REQUIRE(port != 0);
    auto accepted = std::async(std::launch::async, [listener] { return listener->accept(); });
    auto client = net::TcpChannel::connect(port);
    auto server = accepted.get();
    return SocketPair{std::move(client), std::move(server)};
}

// Matching versions accept over a real socket — the handshake is unaware it is on
// TCP rather than the loopback queue.
void testMatchingVersionsAcceptOverSocket() {
    auto pair = connectPair();
    auto serverSide = std::async(std::launch::async,
                                 [&] { return net::performServerHandshake(*pair.server); });
    const auto clientResult = net::performClientHandshake(*pair.client);
    const auto serverResult = serverSide.get();

    REQUIRE(clientResult.ok());
    REQUIRE(serverResult.ok());
}

// A client one version ahead is refused: the server returns VersionMismatch and
// tells the client its own version, and the client sees Rejected carrying that
// version — protocol version negotiation with a defined outcome, not a stream of
// undecodable frames.
void testVersionMismatchRejected() {
    auto pair = connectPair();
    auto serverSide = std::async(std::launch::async,
                                 [&] { return net::performServerHandshake(*pair.server); });
    const auto clientResult =
        net::performClientHandshake(*pair.client, net::kProtocolVersion + 1U);
    const auto serverResult = serverSide.get();

    REQUIRE(!clientResult.ok());
    REQUIRE(clientResult.status == net::HandshakeStatus::Rejected);
    REQUIRE(clientResult.peerProtocolVersion == net::kProtocolVersion);
    REQUIRE(!clientResult.reason.empty());

    REQUIRE(serverResult.status == net::HandshakeStatus::VersionMismatch);
    REQUIRE(serverResult.peerProtocolVersion == net::kProtocolVersion + 1U);
}

// A peer whose first frame is not a rebedrock hello (wrong magic) is refused as a
// bad peer, and the server still replies so the peer is not left hanging.
void testBadMagicRejected() {
    auto pair = connectPair();
    auto serverSide = std::async(std::launch::async,
                                 [&] { return net::performServerHandshake(*pair.server); });

    // Send a well-formed ClientHello frame but with the wrong application magic.
    net::ClientHello impostor;
    impostor.magic = 0xDEADBEEFU;
    pair.client->sendFrame(net::encodeClientHello(impostor));

    const auto serverResult = serverSide.get();
    REQUIRE(serverResult.status == net::HandshakeStatus::BadPeer);

    // The server replied with a refusal the client can read.
    std::optional<net::ServerHello> reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    std::vector<std::uint8_t> frame;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pair.client->receiveFrame(frame)) {
            reply = net::decodeServerHello(frame);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(reply.has_value());
    REQUIRE(!reply->accepted);
}

// A server whose client never sends a hello times out cleanly — the handshake
// never hangs past its deadline. Uses a short timeout so the test is quick.
void testSilentPeerTimesOut() {
    auto pair = net::makeLoopbackPair();
    const auto before = std::chrono::steady_clock::now();
    const auto result =
        net::performServerHandshake(*pair.server, net::kProtocolVersion,
                                    std::chrono::milliseconds{200});
    const auto elapsed = std::chrono::steady_clock::now() - before;

    REQUIRE(result.status == net::HandshakeStatus::Timeout);
    // It returned near the deadline, not instantly and not never.
    REQUIRE(elapsed >= std::chrono::milliseconds{150});
    REQUIRE(elapsed < std::chrono::seconds{5});
}

// R0-4: block identity crosses the wire as a dense BlockId, and the ServerHello
// carries the server's block registry so the client can remap the peer's ids to
// its own by name. An accepted hello round-trips the whole name list.
void testServerHelloCarriesRegistrySnapshot() {
    const auto snapshot = gameplay::localBlockRegistrySnapshot();
    REQUIRE(snapshot.size() == world::blockRegistry().size());
    // Entry i is the block the sender calls BlockId i.
    REQUIRE(snapshot[static_cast<std::size_t>(world::Block::Stone)] == "rebedrock:stone");

    const net::ServerHello server{true, net::kProtocolVersion, {}, snapshot};
    const auto bytes = net::encodeServerHello(server);
    const auto decoded = net::decodeServerHello(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->blockRegistry == snapshot);
    REQUIRE(*decoded == server);
}

// On loopback both ends share one registry, so the remap the accepted client
// builds is the identity — no per-block cost in single-player.
void testLoopbackHandshakeBuildsIdentityRemap() {
    auto pair = net::makeLoopbackPair();
    auto serverSide = std::async(std::launch::async,
                                 [&] { return net::performServerHandshake(*pair.server); });
    const auto clientResult = net::performClientHandshake(*pair.client);
    static_cast<void>(serverSide.get());

    REQUIRE(clientResult.ok());
    REQUIRE(clientResult.remap.isIdentity());
    // A block's id maps straight to itself.
    const auto stone = world::blockId(world::Block::Stone);
    REQUIRE(clientResult.remap.toLocal(stone.value()) == stone);
}

// Two ends whose registries assign different ids to the same block still agree,
// because the remap aligns by name, not by the raw id. The peer here has one
// extra block ahead of ours, so every shared block sits one id higher on its
// side; the remap has to undo exactly that shift.
void testCrossProcessRemapAlignsByName() {
    auto peer = gameplay::localBlockRegistrySnapshot();
    peer.insert(peer.begin(), "servermod:extra");  // shifts every shared id by +1
    const gameplay::BlockIdRemap remap{peer};

    REQUIRE(!remap.isIdentity());
    // A name this build does not have (the peer's extra block) maps to nothing.
    REQUIRE(!remap.toLocal(0).valid());
    // Every shared block maps back from its shifted peer id to our own id by name.
    for (std::size_t ordinal = 0; ordinal < world::kBuiltinBlockCount; ++ordinal) {
        const auto local = world::BlockId::of(static_cast<world::BlockId::Value>(ordinal));
        REQUIRE(remap.toLocal(static_cast<std::uint16_t>(ordinal + 1U)) == local);
    }
}

// End to end over the wire: a block the peer encodes with *its* id decodes to the
// right local block through the remap — and to the wrong one without it, which is
// exactly why the id must be remapped and never trusted raw across ends.
void testWireBlockRemap() {
    auto peer = gameplay::localBlockRegistrySnapshot();
    peer.insert(peer.begin(), "servermod:extra");
    const gameplay::BlockIdRemap remap{peer};

    const auto localStone = world::blockId(world::Block::Stone);
    // The peer's id for stone is one higher than ours.
    std::vector<std::uint8_t> bytes;
    persistence::appendInteger(bytes, static_cast<std::uint16_t>(localStone.value() + 1U));

    std::size_t cursor = 0;
    const auto viaRemap = gameplay::codec::readBlock(bytes, cursor, &remap);
    REQUIRE(viaRemap.has_value());
    REQUIRE(*viaRemap == world::Block::Stone);

    // The same bytes, read as a raw id with no remap, land on a different block —
    // the corruption the remap exists to prevent.
    cursor = 0;
    const auto viaRaw = gameplay::codec::readBlock(bytes, cursor, nullptr);
    REQUIRE(!viaRaw.has_value() || *viaRaw != world::Block::Stone);

    // A block-state round-trips through the wire with an identity remap (loopback),
    // proving the id form carries the properties intact.
    std::vector<std::uint8_t> stateBytes;
    const world::BlockState furnace =
        world::BlockState{world::Block::Furnace, world::BlockOrientation::West}.withLit(true);
    gameplay::codec::appendBlockState(stateBytes, furnace);
    std::size_t stateCursor = 0;
    const auto state = gameplay::codec::readBlockState(stateBytes, stateCursor, nullptr);
    REQUIRE(state.has_value());
    REQUIRE(*state == furnace);

    // AR-B4-2: the codec is schema-driven — appendBlockState walks
    // kStatePropertyCount and emits whatever the block declares — so a new
    // property rides the wire with no codec change at all. This is the
    // assertion the SlabType miss earns: back then a hand-listed codec dropped
    // a newly added axis silently, and the only thing standing between that and
    // a repeat is a test that adds the newest properties and checks they came
    // back. Every axis AR-B4-2 introduced is set to a non-default value here,
    // because a default would round-trip even through a codec that dropped it.
    for (const world::BlockState sent :
         {world::BlockState{world::Block::OakDoor, world::BlockOrientation::North}
              .withOpen(true)
              .withPowered(true)
              .withHinge(world::DoorHinge::Right)
              .withDoorUpperHalf(true),
          world::BlockState{world::Block::OakFenceGate, world::BlockOrientation::South}
              .withOpen(true)
              .withPowered(true)
              .withInWall(true),
          world::BlockState{world::Block::Repeater, world::BlockOrientation::East}
              .withRepeaterDelay(3)
              .withPowered(true)
              .withRepeaterLocked(true)}) {
        std::vector<std::uint8_t> wire;
        gameplay::codec::appendBlockState(wire, sent);
        std::size_t cursorBack = 0;
        const auto back = gameplay::codec::readBlockState(wire, cursorBack, nullptr);
        REQUIRE(back.has_value());
        REQUIRE(*back == sent);
    }
    // Read the new axes back by name as well as by state equality, so a codec
    // that happened to preserve the raw id while losing a property still fails.
    {
        const auto gate = world::BlockState{world::Block::OakFenceGate}.withInWall(true);
        std::vector<std::uint8_t> wire;
        gameplay::codec::appendBlockState(wire, gate);
        std::size_t cursorBack = 0;
        const auto back = gameplay::codec::readBlockState(wire, cursorBack, nullptr);
        REQUIRE(back.has_value());
        REQUIRE(back->inWall());
        const auto repeater = world::BlockState{world::Block::Repeater}.withRepeaterLocked(true);
        std::vector<std::uint8_t> repeaterWire;
        gameplay::codec::appendBlockState(repeaterWire, repeater);
        std::size_t repeaterCursor = 0;
        const auto repeaterBack =
            gameplay::codec::readBlockState(repeaterWire, repeaterCursor, nullptr);
        REQUIRE(repeaterBack.has_value());
        REQUIRE(repeaterBack->repeaterLocked());
    }
}

}  // namespace

int main() {
    testHelloCodecRoundTrip();
    testMatchingVersionsAcceptOverLoopback();
    testMatchingVersionsAcceptOverSocket();
    testVersionMismatchRejected();
    testBadMagicRejected();
    testSilentPeerTimesOut();
    testServerHelloCarriesRegistrySnapshot();
    testLoopbackHandshakeBuildsIdentityRemap();
    testCrossProcessRemapAlignsByName();
    testWireBlockRemap();
    return 0;
}
