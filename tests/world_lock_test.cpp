#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldLock.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// P3 Step 4: the reader/writer section around the shared World.
//
// This is the one part of the threading work that can be exercised headlessly,
// so it is worth doing properly: the test actually runs a writer and several
// readers concurrently and checks that no reader ever observes a half-applied
// write. Under ThreadSanitizer it also fails outright if the guards are wrong,
// which is the check that matters most and the one a soak run cannot give.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"world_lock_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

} // namespace

int main() {
    using namespace mc;

    // --- Several readers may hold the world at once; a writer excludes them
    // all. The counters make the exclusion observable rather than assumed. ---
    {
        world::WorldLock lock;
        std::atomic<int> activeReaders{0};
        std::atomic<int> activeWriters{0};
        std::atomic<int> maxConcurrentReaders{0};
        std::atomic<bool> violation{false};

        const auto reader = [&] {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                const auto guard = lock.read();
                const int readers = ++activeReaders;
                std::this_thread::yield();
                int previousMax = maxConcurrentReaders.load();
                while (readers > previousMax &&
                       !maxConcurrentReaders.compare_exchange_weak(previousMax, readers)) {
                }
                // No writer may be inside while a reader is.
                if (activeWriters.load() != 0) {
                    violation = true;
                }
                --activeReaders;
            }
        };
        const auto writer = [&] {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                const auto guard = lock.write();
                ++activeWriters;
                // Neither another writer nor any reader may be inside. The
                // yield widens the critical section on purpose: with an
                // instantaneous body a broken lock still passes most of the
                // time, which is exactly how a concurrency test fools itself.
                for (int spin = 0; spin < 4; ++spin) {
                    if (activeWriters.load() != 1 || activeReaders.load() != 0) {
                        violation = true;
                    }
                    std::this_thread::yield();
                }
                --activeWriters;
            }
        };

        std::vector<std::thread> threads;
        threads.emplace_back(writer);
        for (int index = 0; index < 3; ++index) {
            threads.emplace_back(reader);
        }
        for (auto& thread : threads) {
            thread.join();
        }
        REQUIRE(!violation.load());
        // Sanity: the readers really did overlap, or the exclusion check above
        // proved nothing.
        REQUIRE(maxConcurrentReaders.load() > 1);
    }

    // --- A multi-cell write is atomic to readers. This is the property that
    // makes the section belong at the call site rather than inside World: a
    // reader sampling two blocks must not see one from before the write and the
    // other from after. ---
    {
        world::World world;
        world.setChunk({0, 0}, world::Chunk{});
        world::WorldLock lock;
        std::atomic<bool> stop{false};
        std::atomic<bool> torn{false};

        // The writer keeps eight cells in agreement, flipping them together.
        std::thread writer{[&] {
            bool stone = true;
            while (!stop.load()) {
                const auto guard = lock.write();
                const auto block = stone ? world::Block::Stone : world::Block::Dirt;
                // Written one at a time with a yield in the middle so the
                // half-applied state is actually observable to a reader that
                // slipped in — a tight loop over eight cells is too fast to
                // catch a missing lock reliably.
                for (int index = 0; index < 8; ++index) {
                    static_cast<void>(world.setBlock(index, 5, 0, block));
                    if (index == 3) {
                        std::this_thread::yield();
                    }
                }
                stone = !stone;
            }
        }};
        // A reader must always find all eight the same.
        std::thread reader{[&] {
            for (int iteration = 0; iteration < 20000; ++iteration) {
                const auto guard = lock.read();
                const auto first = world.block(0, 5, 0);
                for (int index = 1; index < 8; ++index) {
                    if (world.block(index, 5, 0) != first) {
                        torn = true;
                    }
                }
            }
        }};
        reader.join();
        stop = true;
        writer.join();
        REQUIRE(!torn.load());
    }

    return 0;
}
