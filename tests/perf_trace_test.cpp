#include "core/PerfTrace.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::size_t count(std::string_view text, std::string_view needle) {
    std::size_t result = 0;
    for (std::size_t position = 0; (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) ++result;
    return result;
}

} // namespace

int main() {
    using mc::diag::PerfTrace;
    const auto root = std::filesystem::temp_directory_path() / "mc_rebedrock_perf_trace_test.json";
    std::filesystem::remove(root);

    PerfTrace disabled;
    { auto scope = disabled.scope("never"); }
    disabled.counter("never", 1.0);
    assert(!disabled.flush());
    assert(!std::filesystem::exists(root));

    const auto concurrentRoot = std::filesystem::temp_directory_path() / "mc_rebedrock_perf_trace_concurrent_test.json";
    std::filesystem::remove(concurrentRoot);

    const auto flushingRoot = std::filesystem::temp_directory_path() / "mc_rebedrock_perf_trace_flushing_test.json";
    std::filesystem::remove(flushingRoot);
    PerfTrace flushing{flushingRoot, 100000U};
    std::atomic<int> flushingReady{0};
    std::atomic<int> calls{0};
    std::atomic<bool> flushingStart{false};
    std::vector<std::thread> flushingWorkers;
    for (int worker = 0; worker < 8; ++worker) {
        flushingWorkers.emplace_back([&] {
            flushingReady.fetch_add(1, std::memory_order_release);
            while (!flushingStart.load(std::memory_order_acquire)) {}
            for (int event = 0; event < 10000; ++event) {
                flushing.counter("worker_during_flush", event);
                calls.fetch_add(1, std::memory_order_release);
            }
        });
    }
    while (flushingReady.load(std::memory_order_acquire) != 8) {}
    flushingStart.store(true, std::memory_order_release);
    while (calls.load(std::memory_order_acquire) < 100) {}
    assert(flushing.flush());
    for (auto& worker : flushingWorkers) worker.join();
    const std::string flushingJson = readFile(flushingRoot);
    assert(count(flushingJson, "\"name\":\"\"") == 0U);
    assert(count(flushingJson, "\"name\":\"worker_during_flush\"") > 0U);
    assert(flushing.flush());
    assert(readFile(flushingRoot) == flushingJson);
    std::filesystem::remove(flushingRoot);
    PerfTrace concurrent{concurrentRoot, 2048U};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> concurrentWorkers;
    for (int worker = 0; worker < 8; ++worker) {
        concurrentWorkers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {}
            for (int event = 0; event < 64; ++event) concurrent.counter("worker_ok", event);
        });
    }
    while (ready.load(std::memory_order_acquire) != 8) {}
    start.store(true, std::memory_order_release);
    for (auto& worker : concurrentWorkers) worker.join();
    assert(concurrent.droppedEvents() == 0U);
    assert(concurrent.flush());
    const std::string concurrentJson = readFile(concurrentRoot);
    assert(count(concurrentJson, "\"name\":\"worker_ok\"") == 512U);
    assert(concurrent.flush());
    assert(readFile(concurrentRoot) == concurrentJson);
    std::filesystem::remove(concurrentRoot);

    const auto delayedRoot = std::filesystem::temp_directory_path() / "mc_rebedrock_perf_trace_delayed_test.json";
    std::filesystem::remove(delayedRoot);
    PerfTrace delayed{delayedRoot, 8U, 60.0, 60.0};
    delayed.counter("before_window", 1.0);
    assert(delayed.flush());
    const std::string delayedJson = readFile(delayedRoot);
    assert(delayedJson.find("\"filtered\":1") != std::string::npos);
    assert(delayedJson.find("before_window") == std::string::npos);
    std::filesystem::remove(delayedRoot);

    PerfTrace trace{root, 64U};
    trace.setThreadName("main");
    {
        auto outer = trace.scope("frame\"name", 17U);
        trace.counter("counter\nname", 3.5, 17U);
        { auto inner = trace.scope("nested", 17U); }
    }
    for (int event = 0; event < 80; ++event) trace.counter("capacity", event);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&trace, worker] {
            for (int event = 0; event < 32; ++event) {
                trace.counter("worker", static_cast<double>(event),
                              static_cast<std::uint64_t>(worker));
            }
        });
    }
    // Seal while producers exist: subsequent attempts are counted as drops, and
    // the second flush is defined to reproduce the same complete JSON document.
    assert(trace.flush());
    for (auto& worker : workers) worker.join();
    const std::string firstJson = readFile(root);
    assert(trace.flush());
    const std::string json = readFile(root);
    assert(json == firstJson);
    assert(json.starts_with("{\n"));
    assert(json.find("frame\\\"name") != std::string::npos);
    assert(json.find("counter\\nname") != std::string::npos);
    assert(json.find("\"value\":3.500") != std::string::npos);
    assert(json.find("\"id\":17") != std::string::npos);
    assert(json.find("\"perf_trace_metadata\"") != std::string::npos);
    assert(json.find("\"thread_name\"") != std::string::npos);
    assert(json.find("\"truncated\":true") != std::string::npos);
    // Metadata and process metadata are the only non-B/E/C events; event capacity is strict.
    assert(count(json, "\"ph\":\"") <= trace.maxEvents() + 2U);
    assert(trace.droppedEvents() > 0U);
    std::filesystem::remove(root);
}
