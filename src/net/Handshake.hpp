#pragma once

// The connection handshake (stage C, §8.2 / D2): the exchange that gates a fresh
// connection before any gameplay message flows. It is the login phase of 26.1's
// connection state machine (HANDSHAKING → LOGIN → PLAY) reduced to what a single
// game intent needs — a client announces the protocol it speaks, the server
// accepts or refuses, and only an accepted connection carries GameCommand /
// snapshot / event / entity frames. No Netty, no 227-packet table: the two hello
// messages ride the same [tag u8][size u32] frame the rest of the wire uses, so
// the handshake runs over any MessageChannel — the loopback pair a single-player
// session holds and the TCP socket a remote client connects on, unchanged.
//
// Why this is net-positive even if multiplayer only ever runs on loopback: a
// connection that must state its protocol and be accepted before it can act is a
// clean, single connection model. It gives a version mismatch a defined outcome
// (a refusal with the server's version, not a stream of frames one side cannot
// decode) and gives the dedicated server a real accept→login→play sequence to
// slot every future connection into, rather than a socket that is assumed to be
// a compatible peer.

#include "core/VersionManifest.hpp"
#include "gameplay/BlockIdRemap.hpp"
#include "gameplay/StreamCodec.hpp"
#include "net/Transport.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace mc::net {

// The wire protocol version. Bump it whenever a message's byte layout changes in
// a way an older peer cannot decode — it is the single number the handshake
// negotiates. It is protocol, not a build number: two builds with the same wire
// format share it.
//
// v4: block identity crosses the wire as a dense BlockId rather than an
// identifier string, and the ServerHello carries the server's block-registry
// name snapshot so the client can remap peer ids to its own by name (R0-4).
//
// The value lives in the single version manifest (core/VersionManifest.hpp); this
// is a named alias so the handshake code reads naturally, but the number is
// defined once, in kVersion, and bumped there.
inline constexpr std::uint32_t kProtocolVersion = core::kVersion.protocolVersion;

// Identifies this as a rebedrock game connection, so a stray or hostile peer
// that opens the socket but speaks something else is refused at the first frame
// instead of being fed game state. ('R'<<24 | 'B'<<16 | 'D'<<8 | 1).
inline constexpr std::uint32_t kAppMagic = 0x52424401U;

// The handshake occupies frame tags well clear of the gameplay categories
// (commands/snapshots/events/entity end around 20), so a handshake frame and a
// gameplay frame can never be confused even though they share the wire — and a
// gameplay frame arriving during the handshake phase is rejected by tag rather
// than misdecoded.
inline constexpr std::uint8_t kClientHelloTag = 240U;
inline constexpr std::uint8_t kServerHelloTag = 241U;

// The client's opening message: who it is (magic) and what protocol it speaks.
struct ClientHello final {
    std::uint32_t magic = kAppMagic;
    std::uint32_t protocolVersion = kProtocolVersion;

    friend bool operator==(const ClientHello&, const ClientHello&) = default;
};

// The server's reply: whether the connection is accepted, the server's own
// protocol version (so a refused client learns what to match), and a reason when
// refused.
struct ServerHello final {
    bool accepted = false;
    std::uint32_t protocolVersion = kProtocolVersion;
    std::string reason;
    // The server's block registry as a name list in BlockId order, so the client
    // can build a peer-id -> local-id remap by name (R0-4). Sent only on an
    // accepted connection; empty on a refusal, which the client closes anyway.
    gameplay::BlockRegistrySnapshot blockRegistry;

    friend bool operator==(const ServerHello&, const ServerHello&) = default;
};

[[nodiscard]] inline std::vector<std::uint8_t> encodeClientHello(const ClientHello& hello) {
    std::vector<std::uint8_t> bytes;
    gameplay::codec::appendFrame(bytes, kClientHelloTag, [&] {
        persistence::appendInteger(bytes, hello.magic);
        persistence::appendInteger(bytes, hello.protocolVersion);
    });
    return bytes;
}

[[nodiscard]] inline std::optional<ClientHello> decodeClientHello(
    std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = gameplay::codec::readFrame(bytes, cursor);
        if (!frame.has_value() || frame->first != kClientHelloTag) {
            return std::nullopt;
        }
        ClientHello hello;
        hello.magic = persistence::readInteger<std::uint32_t>(bytes, cursor);
        hello.protocolVersion = persistence::readInteger<std::uint32_t>(bytes, cursor);
        return hello;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

[[nodiscard]] inline std::vector<std::uint8_t> encodeServerHello(const ServerHello& hello) {
    std::vector<std::uint8_t> bytes;
    gameplay::codec::appendFrame(bytes, kServerHelloTag, [&] {
        persistence::appendInteger(bytes, static_cast<std::uint8_t>(hello.accepted ? 1U : 0U));
        persistence::appendInteger(bytes, hello.protocolVersion);
        gameplay::codec::appendString32(bytes, hello.reason);
        persistence::appendInteger(bytes, static_cast<std::uint32_t>(hello.blockRegistry.size()));
        for (const auto& name : hello.blockRegistry) {
            gameplay::codec::appendString32(bytes, name);
        }
    });
    return bytes;
}

[[nodiscard]] inline std::optional<ServerHello> decodeServerHello(
    std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = gameplay::codec::readFrame(bytes, cursor);
        if (!frame.has_value() || frame->first != kServerHelloTag) {
            return std::nullopt;
        }
        ServerHello hello;
        hello.accepted = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0U;
        hello.protocolVersion = persistence::readInteger<std::uint32_t>(bytes, cursor);
        hello.reason = gameplay::codec::readString32(bytes, cursor);
        const auto count = persistence::readInteger<std::uint32_t>(bytes, cursor);
        hello.blockRegistry.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            hello.blockRegistry.push_back(gameplay::codec::readString32(bytes, cursor));
        }
        return hello;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// How a handshake ended. Only Accepted lets play begin; every other outcome
// means the caller closes the connection. VersionMismatch and BadPeer are the
// server's verdicts on the client; Timeout and Disconnected are transport
// outcomes either side can hit.
enum class HandshakeStatus {
    Accepted,         // Versions agree; the channel now carries play frames.
    VersionMismatch,  // Peer speaks a protocol this build cannot.
    BadPeer,          // First frame was not a valid rebedrock hello.
    Rejected,         // The server refused for a stated reason (client side).
    Timeout,          // The peer sent no hello within the deadline.
};

struct HandshakeResult final {
    HandshakeStatus status = HandshakeStatus::Timeout;
    // The peer's protocol version, when it sent a decodable hello.
    std::uint32_t peerProtocolVersion = 0U;
    // Human-readable cause when not Accepted.
    std::string reason;
    // The client's peer-id -> local-id block remap, built from the server's
    // registry snapshot on an accepted connection (R0-4). Identity on the server
    // side and on loopback, where the two registries match. The connection's
    // decode path carries this into receiveMessage/decodeMessage.
    gameplay::BlockIdRemap remap;

    [[nodiscard]] bool ok() const { return status == HandshakeStatus::Accepted; }
};

inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{5000};

namespace detail {

// Blocks (up to the deadline) for the next frame on a non-blocking channel — the
// handshake is a one-time request/response at connection setup, not a per-tick
// path, so a short poll-sleep is the right shape. nullopt on timeout.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> awaitHandshakeFrame(
    MessageChannel& channel, std::chrono::steady_clock::time_point deadline) {
    std::vector<std::uint8_t> frame;
    for (;;) {
        if (channel.receiveFrame(frame)) {
            return frame;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

}  // namespace detail

// The client side: announce our protocol, then wait for the server's verdict.
// Runs on the client's connection thread (the render process's, or a test's);
// the server side must be running concurrently on its own thread, since this
// blocks on the reply. Returns Accepted only when the server accepted; Rejected
// (with the server's reason and version) when it refused; Timeout when no
// decodable reply arrived — never hangs past the deadline.
[[nodiscard]] inline HandshakeResult performClientHandshake(
    MessageChannel& channel, std::uint32_t protocolVersion = kProtocolVersion,
    std::chrono::milliseconds timeout = kDefaultHandshakeTimeout) {
    channel.sendFrame(encodeClientHello(ClientHello{kAppMagic, protocolVersion}));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto frame = detail::awaitHandshakeFrame(channel, deadline);
    if (!frame.has_value()) {
        return HandshakeResult{HandshakeStatus::Timeout, 0U, "no server hello before the deadline"};
    }
    const auto hello = decodeServerHello(*frame);
    if (!hello.has_value()) {
        return HandshakeResult{HandshakeStatus::BadPeer, 0U, "server sent a malformed hello"};
    }
    if (hello->accepted) {
        HandshakeResult result{HandshakeStatus::Accepted, hello->protocolVersion, {}};
        // Reconcile the server's block ids to ours by name; identity when the two
        // registries match (loopback, or a peer with the same content).
        result.remap = gameplay::BlockIdRemap{hello->blockRegistry};
        return result;
    }
    return HandshakeResult{HandshakeStatus::Rejected, hello->protocolVersion, hello->reason};
}

// The server side: wait for the client's hello, decide, and reply. Runs on the
// server's per-connection thread (a test's, or the dedicated accept loop's).
// Sends a refusal (with this build's version, so the client learns what to
// match) and returns the matching non-Accepted status when the client's magic or
// version does not check out; sends an acceptance and returns Accepted when it
// does. Timeout when the client sent nothing decodable.
[[nodiscard]] inline HandshakeResult performServerHandshake(
    MessageChannel& channel, std::uint32_t protocolVersion = kProtocolVersion,
    std::chrono::milliseconds timeout = kDefaultHandshakeTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto frame = detail::awaitHandshakeFrame(channel, deadline);
    if (!frame.has_value()) {
        return HandshakeResult{HandshakeStatus::Timeout, 0U, "no client hello before the deadline"};
    }
    const auto hello = decodeClientHello(*frame);
    if (!hello.has_value()) {
        channel.sendFrame(encodeServerHello(
            ServerHello{false, protocolVersion, "unrecognized protocol"}));
        return HandshakeResult{HandshakeStatus::BadPeer, 0U, "client sent a malformed hello"};
    }
    if (hello->magic != kAppMagic) {
        channel.sendFrame(encodeServerHello(
            ServerHello{false, protocolVersion, "not a rebedrock connection"}));
        return HandshakeResult{HandshakeStatus::BadPeer, hello->protocolVersion,
                               "wrong application magic"};
    }
    if (hello->protocolVersion != protocolVersion) {
        const std::string reason =
            hello->protocolVersion < protocolVersion ? "outdated client" : "incompatible client";
        channel.sendFrame(encodeServerHello(ServerHello{false, protocolVersion, reason}));
        return HandshakeResult{HandshakeStatus::VersionMismatch, hello->protocolVersion, reason};
    }
    // An accepted client gets our block registry so it can remap our ids to its
    // own by name; a refusal above sent none.
    channel.sendFrame(encodeServerHello(
        ServerHello{true, protocolVersion, {}, gameplay::localBlockRegistrySnapshot()}));
    return HandshakeResult{HandshakeStatus::Accepted, hello->protocolVersion, {}};
}

}  // namespace mc::net
