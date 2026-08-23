// The dedicated server entry point (stage C, §8.2 / D7). Links the runtime, not
// render or Vulkan — the binary that proves the authoritative simulation runs
// headless. It creates or loads a world and ticks it at 20 TPS until interrupted,
// saving on the way out.
//
// Usage:
//   dedicated_server [--save-dir DIR] [--world NAME] [--seed N]
//                    [--view-distance N] [--ticks N] [--port N]
//                    [--resource-dir DIR]
//
// --ticks N runs exactly N ticks then saves and exits (bounded run / smoke test);
// omitted, it runs continuously until SIGINT/SIGTERM. --port enables the current
// single-client TCP listener; without it the server runs locally/headlessly.
// --resource-dir (PACK-1) is this build's resources/ directory; when given,
// loadWorld scans and rebuilds the open save's <save>/datapacks/ (recipes,
// loot, tags, entity attributes, biome spawn tables) the same way single-
// player does — proving the authoritative per-save data-pack path headless.
// Omitted, the server never touches those tables (pre-PACK-1 behaviour).

#include "gameplay/GameMode.hpp"
#include "server/DedicatedServer.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested{false};

void requestStop(int) { g_stopRequested.store(true); }

// Parses an unsigned value after a flag, or leaves the target untouched and
// reports the error.
template <typename Integer>
bool parseInteger(std::string_view text, Integer& out) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    Integer value{};
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path saveDir = "world-saves";
    std::string worldName = "dedicated";
    std::uint64_t seed = 0U;
    int viewDistance = 8;
    long long tickLimit = -1;  // negative: run until interrupted
    long long listenPort = -1;  // negative: local-only, no listener
    std::filesystem::path resourceDir;  // empty: no per-save data-pack rebuild (PACK-1)

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto next = [&]() -> std::string_view {
            return index + 1 < argc ? std::string_view{argv[++index]} : std::string_view{};
        };
        if (arg == "--save-dir") {
            saveDir = std::string{next()};
        } else if (arg == "--world") {
            worldName = std::string{next()};
        } else if (arg == "--seed") {
            if (!parseInteger(next(), seed)) {
                std::cerr << "dedicated_server: --seed needs an unsigned integer\n";
                return 1;
            }
        } else if (arg == "--view-distance") {
            if (!parseInteger(next(), viewDistance) || viewDistance < 1) {
                std::cerr << "dedicated_server: --view-distance needs a positive integer\n";
                return 1;
            }
        } else if (arg == "--ticks") {
            if (!parseInteger(next(), tickLimit) || tickLimit < 0) {
                std::cerr << "dedicated_server: --ticks needs a non-negative integer\n";
                return 1;
            }
        } else if (arg == "--port") {
            if (!parseInteger(next(), listenPort) || listenPort < 0 || listenPort > 65535) {
                std::cerr << "dedicated_server: --port needs a value in 0..65535\n";
                return 1;
            }
        } else if (arg == "--resource-dir") {
            resourceDir = std::string{next()};
        } else {
            std::cerr << "dedicated_server: unknown argument '" << arg << "'\n";
            return 1;
        }
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    std::filesystem::create_directories(saveDir);
    mc::server::DedicatedServer server{saveDir, viewDistance, resourceDir};

    // Open the world by name if it already exists, else create it fresh.
    if (server.loadWorldByName(worldName)) {
        std::cout << "[server] loaded world '" << worldName << "'\n";
    } else {
        server.createAndLoadWorld(worldName, seed, mc::gameplay::GameMode::Survival);
        std::cout << "[server] created world '" << worldName << "' (seed " << seed << ")\n";
    }
    // Force-load only the player's own column so startup is bounded; the rest of
    // the view-distance area streams in over the first ticks (the worker generates
    // it in the background, tickOnce drains it).
    server.ensureSpawnAreaLoaded(/*radius=*/0);

    // If a port was given, listen and wait for one client to connect and complete
    // the login handshake before ticking (the world runs authoritative for that
    // connection over TCP). Without --port the server runs the world locally.
    if (listenPort >= 0) {
        server.openListener(static_cast<std::uint16_t>(listenPort));
        std::cout << "[server] listening on 127.0.0.1:" << server.listenPort()
                  << ", waiting for a client...\n";
        const auto handshake = server.acceptConnection();
        if (!handshake.ok()) {
            std::cerr << "[server] client rejected: " << handshake.reason << "\n";
            return 1;
        }
        std::cout << "[server] client connected (protocol " << handshake.peerProtocolVersion
                  << ")\n";
    }

    std::cout << "[server] view distance " << viewDistance << " chunks; "
              << "spawn column loaded (" << server.loadedChunkCount() << " chunks). "
              << (tickLimit >= 0 ? "Running " + std::to_string(tickLimit) + " ticks."
                                 : "Running until interrupted (Ctrl-C).")
              << "\n";

    constexpr auto kTickPeriod = std::chrono::milliseconds{50};  // 20 TPS
    long long ticks = 0;
    auto nextStatus = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!g_stopRequested.load() && (tickLimit < 0 || ticks < tickLimit)) {
        // A client that connected and then dropped ends the run (this skeleton
        // serves one connection); a local-only run has no connection to watch.
        if (listenPort >= 0 && !server.hasConnection()) {
            std::cout << "[server] client disconnected.\n";
            break;
        }
        const auto tickStart = std::chrono::steady_clock::now();
        server.tickOnce();
        ++ticks;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus) {
            std::cout << "[server] tick " << ticks << ", " << server.loadedChunkCount()
                      << " chunks, " << (server.serverResidentBytes() / 1024U) << " KiB resident\n";
            nextStatus = now + std::chrono::seconds{5};
        }
        // Pace to 20 TPS when the tick finished early; skip the sleep in a
        // bounded run so a smoke test does not wait out its whole duration.
        if (tickLimit < 0) {
            std::this_thread::sleep_until(tickStart + kTickPeriod);
        }
    }

    server.save();
    std::cout << "[server] saved and stopped after " << ticks << " ticks.\n";
    return 0;
}
