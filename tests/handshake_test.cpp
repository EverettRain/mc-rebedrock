#include "net/Handshake.hpp"
#include "net/LoopbackTransport.hpp"
#include "net/TcpTransport.hpp"
#include "net/Transport.hpp"

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

}  // namespace

int main() {
    testHelloCodecRoundTrip();
    testMatchingVersionsAcceptOverLoopback();
    testMatchingVersionsAcceptOverSocket();
    testVersionMismatchRejected();
    testBadMagicRejected();
    testSilentPeerTimesOut();
    return 0;
}
