#pragma once

// The transport abstraction (stage C, slice 1): a byte-frame channel with two
// ends. This is the only "network" boundary in the whole client/server split —
// everything above it (GameCommand / snapshot / event codecs, the runtime, the
// renderer's mirror) is unaware of whether the peer is in this process, on a
// TCP socket, or across a LAN.
//
// It carries self-delimiting frames, not a raw byte stream: one sendFrame is one
// encodeMessage result, one receiveFrame yields one whole frame back. A frame
// already carries its own size in its header (StreamCodec framing), so a future
// TCP channel reassembles the stream into frames behind this same interface; the
// loopback channel below needs no reassembly because it never fragments.
//
// Modelled on 26.1's Connection over a LocalChannel: the in-process pair share a
// pair of queues so a single-player client speaks to its own server over exactly
// the code path a remote client would.

#include "net/NetMessage.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mc::net {

// One end of a bidirectional byte-frame channel. Both methods are thread-safe
// with respect to the peer end: the producer thread sends while the consumer
// thread receives, which is the sim/render split a single-player session already
// runs. receiveFrame is non-blocking — the caller polls it once per tick/frame
// the way the renderer already polls the command queue and the event bridge.
class MessageChannel {
  public:
    virtual ~MessageChannel() = default;

    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;

    // Hands one already-framed message to the peer end.
    virtual void sendFrame(std::vector<std::uint8_t> frame) = 0;
    // Takes the next frame the peer sent, in send order; false when none is
    // pending.
    [[nodiscard]] virtual bool receiveFrame(std::vector<std::uint8_t>& outFrame) = 0;

  protected:
    MessageChannel() = default;
};

// Typed conveniences over the byte-frame channel. sendMessage frames a
// NetMessage and hands it over; receiveMessage takes the next frame and decodes
// it. A frame that decodes to nothing (an unknown tag from a newer peer, or
// content this build lacks) is reported as an empty optional but still consumed,
// so a poll loop makes progress rather than stalling on it.
inline void sendMessage(MessageChannel& channel, const NetMessage& message) {
    channel.sendFrame(encodeMessage(message));
}

[[nodiscard]] inline bool receiveMessage(MessageChannel& channel,
                                         std::optional<NetMessage>& outMessage,
                                         const gameplay::BlockIdRemap* remap = nullptr) {
    std::vector<std::uint8_t> frame;
    if (!channel.receiveFrame(frame)) {
        return false;
    }
    outMessage = decodeMessage(frame, remap);
    return true;
}

}  // namespace mc::net
