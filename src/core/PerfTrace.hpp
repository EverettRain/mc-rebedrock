#pragma once

// A small, opt-in trace-event recorder for cross-thread performance captures.
// It deliberately does no I/O from a hot path.  Set MC_REBEDROCK_PERF_TRACE to
// a JSON output path, then call flush() at a controlled shutdown/checkpoint.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mc::diag {

class PerfTrace final {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kDefaultMaxEvents = 200000U;
    static constexpr std::size_t kAbsoluteMaxEvents = 1000000U;

    // A public constructor makes the recorder testable without changing process
    // environment. Production callers should use instance().
    explicit PerfTrace(std::filesystem::path outputPath = {},
                       std::size_t maxEvents = kDefaultMaxEvents,
                       double delaySeconds = 0.0, double durationSeconds = 0.0)
        : outputPath_(std::move(outputPath)),
          maxEvents_(std::clamp(maxEvents, std::size_t{1U}, kAbsoluteMaxEvents)),
          enabled_(!outputPath_.empty()),
          systemEpochUs_(systemEpochUs()),
          captureStartUs_(timestampUs() + std::max(0.0, delaySeconds) * 1000000.0),
          captureEndUs_(durationSeconds > 0.0 ? captureStartUs_ + durationSeconds * 1000000.0
                                              : std::numeric_limits<double>::infinity()) {
        // This is the capture's fixed upper memory bound: one Event per allowed
        // trace event, allocated before any hot-path producer starts.
        if (enabled_) {
            try {
                events_.resize(maxEvents_);
            } catch (const std::bad_alloc&) {
                enabled_ = false;
                std::cerr << "PerfTrace: unable to reserve " << maxEvents_
                          << " events; tracing disabled\n";
            }
        }
    }

    PerfTrace(const PerfTrace&) = delete;
    PerfTrace& operator=(const PerfTrace&) = delete;

    [[nodiscard]] static PerfTrace& instance() {
        // Normal use must call flush() after all trace producers stop. This
        // destructor is only a last-resort process-exit capture; it cannot make
        // a caller that leaves producer threads alive during static teardown safe.
        struct Instance final {
            PerfTrace trace{pathFromEnvironment(), maxEventsFromEnvironment(),
                            secondsFromEnvironment("MC_REBEDROCK_PERF_TRACE_DELAY_SECONDS"),
                            secondsFromEnvironment("MC_REBEDROCK_PERF_TRACE_DURATION_SECONDS")};
            ~Instance() noexcept {
                try { static_cast<void>(trace.flush()); }
                catch (const std::exception& error) { std::cerr << "PerfTrace: exit flush failed: " << error.what() << '\n'; }
                catch (...) { std::cerr << "PerfTrace: exit flush failed\n"; }
            }
        };
        static Instance instance;
        return instance.trace;
    }

    [[nodiscard]] static bool enabled() { return instance().enabled_; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    class Scope final {
      public:
        Scope() = default;
        Scope(PerfTrace* owner, const char* name, std::uint64_t id)
            : owner_(owner), name_(name), id_(id), tid_(threadId()), start_(Clock::now()) {}
        ~Scope() {
            if (owner_ != nullptr) owner_->recordSpan(name_, start_, Clock::now(), id_, tid_);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept { *this = std::move(other); }
        Scope& operator=(Scope&& other) noexcept {
            if (this != &other) {
                if (owner_ != nullptr) owner_->recordSpan(name_, start_, Clock::now(), id_, tid_);
                owner_ = std::exchange(other.owner_, nullptr);
                name_ = other.name_;
                id_ = other.id_;
                tid_ = other.tid_;
                start_ = other.start_;
            }
            return *this;
        }

      private:
        PerfTrace* owner_ = nullptr;
        const char* name_ = "";
        std::uint64_t id_ = 0;
        std::uint64_t tid_ = 0;
        Clock::time_point start_{};
    };

    [[nodiscard]] Scope scope(const char* staticName, std::uint64_t id = 0) {
        return enabled_ ? Scope{this, staticName, id} : Scope{};
    }

    void counter(const char* staticName, double value, std::uint64_t id = 0) {
        if (enabled_) record(staticName, 'C', timestampUs(), 0.0, threadId(), id, value);
    }

    // Writes one complete (X) trace event. It is suitable for frame start-to-
    // start pacing spans and externally recovered GPU intervals when both ends
    // are expressed in this recorder's Clock domain. Do not fabricate a CPU/GPU
    // clock conversion merely to use this API.
    void recordSpan(const char* staticName, Clock::time_point start, Clock::time_point end,
                    std::uint64_t id = 0) {
        recordSpan(staticName, start, end, id, threadId());
    }

    void setThreadName(const char* staticName) {
        if (enabled_) record("thread_name", 'M', timestampUs(), 0.0, threadId(), 0, 0.0, staticName);
    }

    // Returns false when disabled or if opening/writing the requested file fails.
    // Calls are serialized so a capture has one complete JSON document.
    [[nodiscard]] bool flush() {
        if (!enabled_) return false;
        std::lock_guard flushGuard{flushMutex_};
        // Seal before copying. Subsequent producer calls are intentionally
        // dropped, so a repeated flush writes the same complete capture.
        if (!sealed_.exchange(true, std::memory_order_seq_cst)) {
            sealedAtUs_.store(timestampUs(), std::memory_order_relaxed);
        }
        while (activeWriters_.load(std::memory_order_seq_cst) != 0U) {
            std::this_thread::yield();
        }
        const std::size_t recorded = reserved_.load(std::memory_order_acquire);
        const auto snapshotEnd = events_.begin() + static_cast<std::ptrdiff_t>(recorded);
        std::vector<Event> snapshot{events_.begin(), snapshotEnd};
        if (!metadataFrozen_) {
            frozenRecorded_ = snapshot.size();
            frozenReserved_ = recorded;
            frozenDropped_ = dropped_.load(std::memory_order_relaxed);
            frozenContentionDropped_ = 0;
            frozenFiltered_ = filtered_.load(std::memory_order_relaxed);
            frozenEndUs_ = sealedAtUs_.load(std::memory_order_relaxed);
            metadataFrozen_ = true;
        }
        std::sort(snapshot.begin(), snapshot.end(), [](const Event& a, const Event& b) {
            return a.timestampUs < b.timestampUs;
        });

        std::ofstream output{outputPath_, std::ios::trunc};
        if (!output) {
            std::cerr << "PerfTrace: unable to open " << outputPath_ << " for writing\n";
            return false;
        }
        output << "{\n  \"displayTimeUnit\": \"ms\",\n  \"traceEvents\": [\n";
        bool first = true;
        const auto writeComma = [&] {
            if (!first) output << ",\n";
            first = false;
        };
        writeComma();
        output << "    {\"name\":\"process_name\",\"ph\":\"M\",\"pid\":" << processId() << ",\"tid\":0,"
               << "\"args\":{\"name\":\"mc_rebedrock\"}}";
        for (const Event& event : snapshot) {
            writeComma();
            output << "    {\"name\":\"" << escape(event.name) << "\",\"cat\":\"mc\",\"ph\":\""
                   << event.phase << "\",\"ts\":" << std::fixed << std::setprecision(3)
                   << event.timestampUs << ",\"pid\":" << processId() << ",\"tid\":" << event.threadId;
            if (event.phase == 'X') output << ",\"dur\":" << event.durationUs;
            output << ",\"args\":{\"id\":" << event.id;
            if (event.phase == 'C') output << ",\"value\":" << event.value;
            if (event.phase == 'M') output << ",\"name\":\"" << escape(event.label) << "\"";
            output << "}}";
        }
        writeComma();
        output << "    {\"name\":\"perf_trace_metadata\",\"ph\":\"M\",\"pid\":" << processId() << ",\"tid\":0,\"args\":{"
               << "\"maxEvents\":" << maxEvents_ << ",\"recorded\":" << frozenRecorded_
               << ",\"reserved\":" << frozenReserved_
               << ",\"dropped\":" << frozenDropped_
               << ",\"contentionDropped\":" << frozenContentionDropped_
               << ",\"systemEpochUs\":" << systemEpochUs_
               << ",\"captureDurationUs\":" << std::max(0.0, frozenEndUs_ - captureStartUs_)
               << ",\"windowStartUs\":" << captureStartUs_
               << ",\"windowEndUs\":"
               << (std::isfinite(captureEndUs_) ? std::to_string(captureEndUs_) : "null")
               << ",\"filtered\":" << frozenFiltered_
               << ",\"capturedSpanUs\":" << capturedSpanUs(snapshot)
               << ",\"truncated\":" << (frozenDropped_ != 0U ? "true" : "false")
               << "}}\n  ]\n}\n";
        output.flush();
        if (!output) {
            std::cerr << "PerfTrace: failed while writing " << outputPath_ << "\n";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t maxEvents() const { return maxEvents_; }

  private:
    struct Event final {
        const char* name = "";
        char phase = 'I';
        double timestampUs = 0.0;
        double durationUs = 0.0;
        std::uint64_t threadId = 0;
        std::uint64_t id = 0;
        double value = 0.0;
        const char* label = "";
    };

    struct ClockAnchor final { Clock::time_point steady = Clock::now(); double systemUs =
        std::chrono::duration<double, std::micro>(std::chrono::system_clock::now().time_since_epoch()).count(); };
    [[nodiscard]] static const ClockAnchor& anchor() { static const ClockAnchor value; return value; }
    [[nodiscard]] static const Clock::time_point& epoch() {
        return anchor().steady;
    }
    [[nodiscard]] static double systemEpochUs() { return anchor().systemUs; }
    [[nodiscard]] static double timestampUs() {
        return std::chrono::duration<double, std::micro>(Clock::now() - epoch()).count();
    }
    [[nodiscard]] static std::uint64_t threadId() {
        static std::atomic<std::uint64_t> next{1};
        thread_local const std::uint64_t id = next.fetch_add(1U, std::memory_order_relaxed);
        return id;
    }
    [[nodiscard]] static int processId() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }
    [[nodiscard]] static std::filesystem::path pathFromEnvironment() {
        const char* value = std::getenv("MC_REBEDROCK_PERF_TRACE");
        return value != nullptr && *value != '\0' ? std::filesystem::path{value} : std::filesystem::path{};
    }
    [[nodiscard]] static std::size_t maxEventsFromEnvironment() {
        const char* value = std::getenv("MC_REBEDROCK_PERF_TRACE_MAX_EVENTS");
        if (value == nullptr || *value == '\0' || *value == '-') return kDefaultMaxEvents;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (end == value || *end != '\0' || parsed == 0U) return kDefaultMaxEvents;
        return static_cast<std::size_t>(std::min<unsigned long long>(
            parsed, static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())));
    }
    [[nodiscard]] static double secondsFromEnvironment(const char* variable) {
        const char* value = std::getenv(variable);
        if (value == nullptr || *value == '\0') return 0.0;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0 ? parsed : 0.0;
    }
    [[nodiscard]] static double capturedSpanUs(const std::vector<Event>& events) {
        const Event* firstEvent = nullptr;
        for (const Event& event : events) {
            if (event.phase == 'X' || event.phase == 'C') { firstEvent = &event; break; }
        }
        if (firstEvent == nullptr) return 0.0;
        double first = firstEvent->timestampUs;
        double last = first;
        for (const Event& event : events) {
            if (event.phase == 'X' || event.phase == 'C') {
                first = std::min(first, event.timestampUs);
                last = std::max(last, event.timestampUs + event.durationUs);
            }
        }
        return std::max(0.0, last - first);
    }
    static std::string escape(std::string_view text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (char rawCharacter : text) {
            const unsigned char character = static_cast<unsigned char>(rawCharacter);
            switch (character) {
              case '"': escaped += "\\\""; break;
              case '\\': escaped += "\\\\"; break;
              case '\b': escaped += "\\b"; break;
              case '\f': escaped += "\\f"; break;
              case '\n': escaped += "\\n"; break;
              case '\r': escaped += "\\r"; break;
              case '\t': escaped += "\\t"; break;
              default:
                if (character < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[(character >> 4U) & 0x0fU];
                    escaped += hex[character & 0x0fU];
                } else {
                    escaped += static_cast<char>(character);
                }
            }
        }
        return escaped;
    }
    void recordSpan(const char* name, Clock::time_point start, Clock::time_point end,
                    std::uint64_t id, std::uint64_t tid) {
        if (!enabled_ || sealed_.load(std::memory_order_seq_cst)) return;
        const double startUs = std::chrono::duration<double, std::micro>(start - epoch()).count();
        const double durationUs = std::max(0.0, std::chrono::duration<double, std::micro>(end - start).count());
        record(name, 'X', startUs, durationUs, tid, id, 0.0);
    }
    void record(const char* name, char phase, double timestamp, double duration, std::uint64_t tid,
                std::uint64_t id, double value, const char* label = "") {
        if (sealed_.load(std::memory_order_seq_cst)) {
            // Explicit flush seals the capture. Later events are deliberately
            // ignored rather than mutating metadata after the file is written.
            return;
        }
        activeWriters_.fetch_add(1U, std::memory_order_seq_cst);
        if (sealed_.load(std::memory_order_seq_cst)) {
            activeWriters_.fetch_sub(1U, std::memory_order_seq_cst);
            return;
        }
        if (phase != 'M' && (timestamp < captureStartUs_ || timestamp + duration > captureEndUs_)) {
            filtered_.fetch_add(1U, std::memory_order_relaxed);
            activeWriters_.fetch_sub(1U, std::memory_order_seq_cst);
            return;
        }
        std::size_t current = reserved_.load(std::memory_order_relaxed);
        while (current < maxEvents_ && !reserved_.compare_exchange_weak(
                   current, current + 1U, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
        if (current >= maxEvents_) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            activeWriters_.fetch_sub(1U, std::memory_order_seq_cst);
            return;
        }
        events_[current] = {name == nullptr ? "" : name, phase, timestamp, duration, tid, id, value,
                            label == nullptr ? "" : label};
        activeWriters_.fetch_sub(1U, std::memory_order_seq_cst);
    }

    std::filesystem::path outputPath_;
    const std::size_t maxEvents_;
    bool enabled_;
    std::atomic<std::size_t> reserved_{0};
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> contentionDropped_{0};
    std::atomic<std::size_t> activeWriters_{0};
    std::atomic<std::size_t> filtered_{0};
    std::atomic<bool> sealed_{false};
    std::atomic<double> sealedAtUs_{0.0};
    bool metadataFrozen_ = false;
    std::size_t frozenRecorded_ = 0, frozenReserved_ = 0, frozenDropped_ = 0;
    std::size_t frozenContentionDropped_ = 0, frozenFiltered_ = 0;
    double frozenEndUs_ = 0.0;
    const double systemEpochUs_;
    const double captureStartUs_;
    const double captureEndUs_;
    std::mutex flushMutex_;
    std::vector<Event> events_;
};

} // namespace mc::diag
