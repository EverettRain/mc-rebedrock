#pragma once

// The in-process loopback transport (stage C, slice 1, step 1): two MessageChannel
// ends connected by a crossed pair of thread-safe frame queues — 26.1's
// LocalChannel. What one end sends, only the other end receives; there is no
// network latency and no fragmentation, so this is the channel that lets
// single-player run the client/server message path with the tick and the render
// on their two existing threads before any socket exists.
//
// makeLoopbackPair() returns the two ends already wired together. By convention
// the first is the client end and the second the server end, but the channel is
// symmetric — the queues do not care which side is authoritative.

#include "net/Transport.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mc::net {

// A thread-safe FIFO of byte frames. One is the client→server direction, the
// other server→client; the two channel ends hold them crossed.
class LoopbackFrameQueue final {
  public:
    void push(std::vector<std::uint8_t> frame) {
        const std::lock_guard<std::mutex> guard{mutex_};
        frames_.push_back(std::move(frame));
    }
    [[nodiscard]] bool pop(std::vector<std::uint8_t>& outFrame) {
        const std::lock_guard<std::mutex> guard{mutex_};
        if (frames_.empty()) {
            return false;
        }
        outFrame = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

  private:
    std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> frames_;
};

// One end of the loopback pair. It sends into the outbound queue and receives
// from the inbound queue; its peer holds the same two queues swapped, so a frame
// can only travel to the other side.
class LoopbackChannel final : public MessageChannel {
  public:
    LoopbackChannel(std::shared_ptr<LoopbackFrameQueue> outbound,
                    std::shared_ptr<LoopbackFrameQueue> inbound)
        : outbound_{std::move(outbound)}, inbound_{std::move(inbound)} {}

    void sendFrame(std::vector<std::uint8_t> frame) override {
        outbound_->push(std::move(frame));
    }
    [[nodiscard]] bool receiveFrame(std::vector<std::uint8_t>& outFrame) override {
        return inbound_->pop(outFrame);
    }

  private:
    std::shared_ptr<LoopbackFrameQueue> outbound_;
    std::shared_ptr<LoopbackFrameQueue> inbound_;
};

struct LoopbackPair final {
    std::unique_ptr<LoopbackChannel> client;
    std::unique_ptr<LoopbackChannel> server;
};

// Builds a connected client/server pair. The two queues are shared by both ends
// (each holds one as outbound and the other as inbound), so they must outlive
// neither end alone — shared_ptr keeps them alive until both channels are gone.
[[nodiscard]] inline LoopbackPair makeLoopbackPair() {
    auto clientToServer = std::make_shared<LoopbackFrameQueue>();
    auto serverToClient = std::make_shared<LoopbackFrameQueue>();
    return LoopbackPair{
        std::make_unique<LoopbackChannel>(clientToServer, serverToClient),
        std::make_unique<LoopbackChannel>(serverToClient, clientToServer),
    };
}

}  // namespace mc::net
