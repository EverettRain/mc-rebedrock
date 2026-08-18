#pragma once

// The TCP transport (stage C, §8.2 / D1): a MessageChannel whose peer is on a
// real socket instead of a shared in-process queue. It is the first channel that
// has to face what a raw byte stream does and the loopback never did — TCP does
// not preserve message boundaries, so one recv() may hand back half a frame or
// several frames coalesced. TcpChannel absorbs that entirely behind sendFrame /
// receiveFrame: it reassembles the stream into whole frames using the shared
// [tag u8][size u32] header (peekFrameLength), so everything above it — the
// NetMessage codecs, GameRuntime's channel side, ClientMirror — is byte-for-byte
// unaware whether it is talking to a loopback queue or a socket. That "the upper
// layers do not change a line" is the whole acceptance of the MessageChannel
// abstraction.
//
// Shape, modelled on 26.1's Connection but with no Netty: one non-blocking IO
// thread per channel owns the socket and does both directions with a single
// poll(); a self-pipe wakes it the instant a producer enqueues or the channel
// is closing, so it neither spins nor sleeps on a timeout. Outbound frames sit
// in a bounded queue — a slow or stalled peer applies backpressure to the sender
// rather than growing memory without limit. Inbound whole frames sit in a second
// queue that receiveFrame drains non-blockingly, exactly the poll-once-per-frame
// contract the loopback channel already meets.
//
// POSIX sockets only (Linux and macOS, the two platforms this project builds
// on). The renderer thread and the sim thread are the producer/consumer pair, so
// the IO thread's synchronisation is what a mac -fsanitize=thread run verifies.

#include "gameplay/StreamCodec.hpp"
#include "net/Transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mc::net {

// send() must not raise SIGPIPE and kill the process when the peer has gone
// away; a dead peer is an ordinary event the IO thread handles by closing. Linux
// exposes this per-call with MSG_NOSIGNAL; macOS has no such flag but sets it
// per-socket with SO_NOSIGPIPE (applied in configureSocket).
#if defined(MSG_NOSIGNAL)
inline constexpr int kTcpSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kTcpSendFlags = 0;
#endif

// Backpressure bound: the sender blocks once this many bytes are queued and not
// yet on the wire, so a stalled peer cannot make the queue grow without limit.
// A single frame larger than the bound is still admitted when the queue is empty
// (so an oversized snapshot cannot deadlock the sender against itself).
inline constexpr std::size_t kTcpMaxSendQueueBytes = 8U * 1024U * 1024U;
// A frame length read from a header this large is treated as a corrupt or
// hostile peer and drops the connection, rather than reserving gigabytes for a
// frame that will never complete. Comfortably above any real snapshot frame.
inline constexpr std::size_t kTcpMaxFrameBytes = 64U * 1024U * 1024U;
// One recv() reads up to this many bytes into the reassembly buffer per wake.
inline constexpr std::size_t kTcpReadChunkBytes = 64U * 1024U;

namespace detail {

// Puts a connected socket into the mode the IO thread needs: non-blocking (the
// thread multiplexes read and write with poll and must never block inside one),
// TCP_NODELAY (a per-tick command or snapshot frame is small and must not wait
// for Nagle to coalesce it — the loopback path has zero latency, so the socket
// path must not add a Nagle delay), and, on macOS, SO_NOSIGPIPE. Throws on any
// failure because a half-configured socket is not usable.
inline void configureSocket(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("TcpChannel: cannot set the socket non-blocking");
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

// A self-pipe used to wake poll() the moment there is outbound work or the
// channel is closing, so the IO thread blocks in poll indefinitely otherwise and
// never spins on a timeout. The read end is drained by the IO thread; the write
// end is non-blocking so a producer that finds the pipe already full (a wake is
// pending) simply moves on rather than blocking.
struct WakePipe final {
    int readEnd = -1;
    int writeEnd = -1;

    WakePipe() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            throw std::runtime_error("TcpChannel: cannot create the wake pipe");
        }
        readEnd = fds[0];
        writeEnd = fds[1];
        for (int fd : {readEnd, writeEnd}) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
    }
    ~WakePipe() {
        if (readEnd >= 0) {
            ::close(readEnd);
        }
        if (writeEnd >= 0) {
            ::close(writeEnd);
        }
    }
    WakePipe(const WakePipe&) = delete;
    WakePipe& operator=(const WakePipe&) = delete;

    // Nudge the IO thread. A single byte is enough; if the pipe is already full
    // the wake it represents has not been consumed yet, so dropping this write is
    // correct (EAGAIN is expected and ignored).
    void signal() const {
        const std::uint8_t byte = 1U;
        const ssize_t written = ::write(writeEnd, &byte, 1);
        static_cast<void>(written);
    }
    // Consume every pending wake byte so the next poll blocks until the next one.
    void drain() const {
        std::uint8_t scratch[64];
        while (::read(readEnd, scratch, sizeof(scratch)) > 0) {
        }
    }
};

}  // namespace detail

// One end of a TCP-connected channel. Construct it from an already-connected,
// already-configured socket (TcpChannel::connect for the client end, TcpListener
// for the server end); it takes ownership of the fd and starts its IO thread.
class TcpChannel final : public MessageChannel {
  public:
    explicit TcpChannel(int fd) : fd_{fd} {
        running_.store(true);
        ioThread_ = std::thread{[this] { runIoLoop(); }};
    }

    ~TcpChannel() override { close(); }

    // Connects to a listener on 127.0.0.1:port and returns the client end. The
    // connect itself is blocking (the socket is switched to non-blocking only
    // afterwards, so the handshake stays a simple synchronous call); throws on
    // failure.
    [[nodiscard]] static std::unique_ptr<TcpChannel> connect(std::uint16_t port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("TcpChannel: cannot create the client socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            throw std::runtime_error("TcpChannel: connect to 127.0.0.1 failed: " + reason);
        }
        detail::configureSocket(fd);
        return std::make_unique<TcpChannel>(fd);
    }

    // Enqueues one already-framed message for the IO thread to write. Blocks the
    // caller while the bound is exceeded (backpressure), and returns immediately
    // once the channel is closed — a frame sent to a dead peer is dropped, not an
    // error, exactly as a lost datagram would be.
    void sendFrame(std::vector<std::uint8_t> frame) override {
        if (frame.empty()) {
            return;
        }
        {
            std::unique_lock<std::mutex> guard{sendMutex_};
            sendSpace_.wait(guard, [&] {
                return !running_.load() ||
                       sendQueue_.empty() ||
                       sendQueuedBytes_ + frame.size() <= kTcpMaxSendQueueBytes;
            });
            if (!running_.load()) {
                return;
            }
            sendQueuedBytes_ += frame.size();
            sendQueue_.push_back(std::move(frame));
        }
        wake_.signal();
    }

    // Takes the next whole frame the peer sent, in order; false when none is
    // ready. Non-blocking — the caller polls it once per tick/frame. Frames the
    // IO thread already reassembled before the peer closed stay drainable here.
    [[nodiscard]] bool receiveFrame(std::vector<std::uint8_t>& outFrame) override {
        std::lock_guard<std::mutex> guard{recvMutex_};
        if (recvQueue_.empty()) {
            return false;
        }
        outFrame = std::move(recvQueue_.front());
        recvQueue_.pop_front();
        return true;
    }

    // True once the IO thread has seen the peer close or a fatal socket error.
    // The handshake layer (D2) polls it to notice a dropped connection; a closed
    // channel still lets receiveFrame drain whatever arrived before the close.
    [[nodiscard]] bool closed() const { return closed_.load(); }

    // Stops the IO thread and releases the socket. Idempotent; the destructor
    // calls it. After this, sendFrame drops and receiveFrame only drains what was
    // already reassembled.
    void close() {
        bool wasRunning = running_.exchange(false);
        if (wasRunning) {
            // Wake both the IO thread (poll) and any sender parked on the bound.
            wake_.signal();
            sendSpace_.notify_all();
        }
        if (ioThread_.joinable()) {
            ioThread_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;

  private:
    // The IO thread body: multiplex read and write on the one socket with poll,
    // woken immediately by the self-pipe on new outbound data or on shutdown.
    void runIoLoop() {
        while (running_.load()) {
            pollfd fds[2];
            fds[0].fd = fd_;
            fds[0].events = POLLIN | (hasOutbound() ? POLLOUT : 0);
            fds[0].revents = 0;
            fds[1].fd = wake_.readEnd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;

            const int ready = ::poll(fds, 2, -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                markClosed();
                break;
            }
            if ((fds[1].revents & POLLIN) != 0) {
                wake_.drain();  // A send or a shutdown; the loop re-evaluates.
            }
            if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
                markClosed();
                break;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                if (!readAvailable()) {
                    break;  // Peer closed or a fatal read error; markClosed done.
                }
            }
            // A hangup after we have drained the readable bytes still ends the
            // loop, but only once POLLIN has been serviced so no received frame
            // is lost.
            if ((fds[0].revents & POLLHUP) != 0 && (fds[0].revents & POLLIN) == 0) {
                markClosed();
                break;
            }
            if ((fds[0].revents & POLLOUT) != 0) {
                if (!writePending()) {
                    break;  // Fatal write error; markClosed done.
                }
            }
        }
    }

    // Whether there is anything to write: a partially written frame in flight or
    // a queued one waiting. Only the queue needs the lock; writeBuffer_ and
    // writeOffset_ belong to the IO thread alone.
    [[nodiscard]] bool hasOutbound() {
        if (writeOffset_ < writeBuffer_.size()) {
            return true;
        }
        std::lock_guard<std::mutex> guard{sendMutex_};
        return !sendQueue_.empty();
    }

    // Drains everything the socket has right now into the reassembly buffer, then
    // splits whole frames out of it. Returns false when the peer closed (recv 0)
    // or a fatal error occurred; true when the socket is merely drained (EAGAIN).
    bool readAvailable() {
        for (;;) {
            const std::size_t base = recvBuffer_.size();
            recvBuffer_.resize(base + kTcpReadChunkBytes);
            const ssize_t got = ::recv(fd_, recvBuffer_.data() + base, kTcpReadChunkBytes, 0);
            if (got > 0) {
                recvBuffer_.resize(base + static_cast<std::size_t>(got));
                if (!extractFrames()) {
                    markClosed();  // Oversized frame: corrupt or hostile peer.
                    return false;
                }
                if (static_cast<std::size_t>(got) < kTcpReadChunkBytes) {
                    return true;  // Read less than asked: the socket is drained.
                }
                continue;  // Filled the chunk; there may be more buffered.
            }
            recvBuffer_.resize(base);  // Undo the speculative grow.
            if (got == 0) {
                markClosed();  // Orderly peer close.
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;  // Nothing more to read for now.
            }
            if (errno == EINTR) {
                continue;
            }
            markClosed();  // Fatal read error.
            return false;
        }
    }

    // Splits every complete frame out of recvBuffer_ and hands it to receiveFrame
    // via the inbound queue, leaving any trailing partial frame for the next
    // read — the reassembly that makes the socket look like the loopback's whole
    // frames. Returns false only when a frame header announces an impossible
    // length (drop the connection); an incomplete frame is normal and just waits.
    [[nodiscard]] bool extractFrames() {
        std::size_t consumed = 0;
        std::vector<std::vector<std::uint8_t>> ready;
        for (;;) {
            const std::span<const std::uint8_t> remaining{recvBuffer_.data() + consumed,
                                                          recvBuffer_.size() - consumed};
            const auto length = gameplay::codec::peekFrameLength(remaining);
            if (!length.has_value()) {
                break;  // Not even a full header yet.
            }
            if (*length > kTcpMaxFrameBytes) {
                return false;
            }
            if (remaining.size() < *length) {
                break;  // Header complete, payload still arriving.
            }
            ready.emplace_back(remaining.begin(), remaining.begin() + *length);
            consumed += *length;
        }
        if (consumed > 0) {
            recvBuffer_.erase(recvBuffer_.begin(),
                              recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
        if (!ready.empty()) {
            std::lock_guard<std::mutex> guard{recvMutex_};
            for (auto& frame : ready) {
                recvQueue_.push_back(std::move(frame));
            }
        }
        return true;
    }

    // Writes as much of the outbound stream as the socket will take. Pulls the
    // next queued frame into writeBuffer_ when the current one is fully sent (and
    // wakes a sender parked on the bound), then send()s from the current offset.
    // Returns false only on a fatal write error.
    bool writePending() {
        for (;;) {
            if (writeOffset_ >= writeBuffer_.size()) {
                std::unique_lock<std::mutex> guard{sendMutex_};
                if (sendQueue_.empty()) {
                    return true;  // Nothing left to write.
                }
                writeBuffer_ = std::move(sendQueue_.front());
                sendQueue_.pop_front();
                sendQueuedBytes_ -= writeBuffer_.size();
                writeOffset_ = 0;
                guard.unlock();
                sendSpace_.notify_all();  // Freed queue space for a parked sender.
            }
            const ssize_t sent = ::send(fd_, writeBuffer_.data() + writeOffset_,
                                        writeBuffer_.size() - writeOffset_, kTcpSendFlags);
            if (sent > 0) {
                writeOffset_ += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;  // Socket buffer full; resume on the next POLLOUT.
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            markClosed();  // Fatal write error (including a reset peer).
            return false;
        }
    }

    // Marks the peer gone and unparks any sender waiting on the bound so it stops
    // rather than waiting for space that will never come.
    void markClosed() {
        closed_.store(true);
        running_.store(false);
        sendSpace_.notify_all();
    }

    int fd_;
    std::thread ioThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    detail::WakePipe wake_;

    // Outbound: producer threads push framed messages; the IO thread pops them.
    std::mutex sendMutex_;
    std::condition_variable sendSpace_;
    std::deque<std::vector<std::uint8_t>> sendQueue_;
    std::size_t sendQueuedBytes_ = 0;
    // The frame currently being written and how far into it we have got. IO
    // thread only — never shared, so it needs no lock.
    std::vector<std::uint8_t> writeBuffer_;
    std::size_t writeOffset_ = 0;

    // Inbound: the IO thread reassembles whole frames and pushes them; the
    // consumer thread drains them in receiveFrame.
    std::mutex recvMutex_;
    std::deque<std::vector<std::uint8_t>> recvQueue_;
    // The partial byte stream awaiting reassembly. IO thread only.
    std::vector<std::uint8_t> recvBuffer_;
};

// A one-shot loopback listener: binds 127.0.0.1 on the requested port (0 lets
// the OS choose, reported by port()), and accept() blocks for one client and
// returns its server-end channel. Enough for D1's single client on one machine;
// LAN/dedicated (D6/D7) grow it into a per-connection accept loop.
class TcpListener final {
  public:
    explicit TcpListener(std::uint16_t port = 0) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            throw std::runtime_error("TcpListener: cannot create the listen socket");
        }
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const std::string reason = std::strerror(errno);
            ::close(listenFd_);
            throw std::runtime_error("TcpListener: bind failed: " + reason);
        }
        if (::listen(listenFd_, 4) != 0) {
            const std::string reason = std::strerror(errno);
            ::close(listenFd_);
            throw std::runtime_error("TcpListener: listen failed: " + reason);
        }
        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            boundPort_ = ntohs(bound.sin_port);
        }
    }

    ~TcpListener() {
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // The port actually bound — the OS-chosen one when constructed with 0.
    [[nodiscard]] std::uint16_t port() const { return boundPort_; }

    // Blocks for one client and returns its server-end channel.
    [[nodiscard]] std::unique_ptr<TcpChannel> accept() {
        for (;;) {
            const int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd >= 0) {
                detail::configureSocket(fd);
                return std::make_unique<TcpChannel>(fd);
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("TcpListener: accept failed");
        }
    }

  private:
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;
};

}  // namespace mc::net
