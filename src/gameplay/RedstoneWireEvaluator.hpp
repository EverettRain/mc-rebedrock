#pragma once

// The redstone-wire AC graph evaluator (W-5) — the performance path that
// replaces RedstoneWire.hpp's naive serial relaxation (kept as the ground-truth
// oracle). It treats a connected redstone_wire network as a graph and settles
// the whole network's final POWER in a single descending-power wavefront (Dial's
// algorithm), then the caller emits block updates once — matching Java's
// ExperimentalRedstoneWireEvaluator direction ("settle, then update") without
// copying either Java evaluator. Only the *output* is constrained: the POWER of
// every wire cell equals the serial fixed point, bit for bit (redstone-reference
// /fixtures/wire-heavy.md invariant ①). The algorithm is self-authored.
//
// Why a wavefront instead of the serial `while(changed)` relaxation: the wire
// law POWER[c] = max(blockSignal[c], max_{wire-neighbour n} POWER[n] - 1) is a
// monotone system whose unique fixed point is
//     POWER[c] = max_s max(0, blockSignal[s] - graphDistance(s, c)),
// i.e. a multi-source shortest-path where power drops by one per hop. Because the
// drop is exactly one and power is bounded to [0,15], Dial's 16-bucket wavefront
// finalises every cell in a single descending pass (O(cells)) — the serial
// relaxation revisits the whole network O(diameter) times (~O(cells^2) on a long
// run). Same fixed point, far less work.
//
// C++ performance governance (W-DESIGN §4.3): the wavefront queue, the cell
// table and the packed-int64 -> index map are reusable per-tick arena buffers
// (zero allocation once warm); coordinates are packed int64 (JE asLong encoding);
// signal queries go through the by-BlockId behaviour table in RedstoneSignal.hpp
// (no virtual dispatch). One evaluator instance is owned by WorldSimulation and
// reused across every wire tick.

#include "gameplay/RedstoneWire.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/World.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::gameplay::redstone {

// The deterministic neighbour traversal order — Java's
// Orientation.of(UP, NORTH, LEFT).getDirections(): with up locked to UP and a
// LEFT side bias, side = cross(front=NORTH, UP).opposite = WEST, so
// getDirections() = [front.opposite, front, side, side.opposite, up.opposite, up]
// = [South, North, West, East, Down, Up]. The AC path fixes `front` at NORTH
// rather than rotating it per hop as Java does: our POWER result is
// order-independent (unique fixed point), so a single fixed orientation is all
// the determinism the block-update emission order needs, and it stays
// self-authored rather than porting Java's 48-entry Orientation state machine.
// This order — distinct from RedstoneWire.hpp's serial kDirectionOffsets order —
// fixes the discovery order of the network and therefore the order downstream
// neighbour updates are emitted.
inline constexpr std::array<Direction, 6> kWireTraversalOrder{{
    Direction::South, Direction::North, Direction::West,
    Direction::East, Direction::Down, Direction::Up,
}};

class WireNetworkEvaluator final {
  public:
    // The whole connected wire network containing `start`, each cell paired with
    // its settled POWER, in deterministic discovery order (BFS from `start` over
    // kWireTraversalOrder). Empty when `start` is not a wire. The returned
    // reference is valid until the next solve() on this evaluator.
    [[nodiscard]] const std::vector<WirePower>& solve(const world::World& world,
                                                      world::BlockPos start) {
        network_.clear();
        if (!isWire(world, start)) {
            return network_;
        }

        floodNetwork(world, start);
        seedAndSettle(world);

        network_.reserve(cells_.size());
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            network_.push_back({cells_[i], power_[i]});
        }
        return network_;
    }

  private:
    // A packed-int64 -> dense-index map: linear probing with per-solve generation
    // stamping, so a reset is O(1) (bump the generation) and the table's storage
    // is reused across ticks. Grows by rebuilding from the cell list, which is
    // rare once the table is sized to the busiest network seen.
    struct Slot final {
        std::int64_t key = 0;
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
    };

    void resetMap() {
        ++generation_;
        if (generation_ == 0) {
            // Wrapped: clear stamps so nothing stale reads as live, then step off 0.
            for (Slot& slot : table_) {
                slot.generation = 0;
            }
            generation_ = 1;
        }
    }

    void rebuildMap(std::size_t minSlots) {
        std::size_t size = table_.empty() ? 64U : table_.size();
        while (size < minSlots * 2U) {
            size *= 2U;
        }
        table_.assign(size, Slot{});
        mask_ = size - 1U;
        generation_ = 1;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(cells_.size()); ++i) {
            insertFresh(packBlockPos(cells_[i]), i);
        }
    }

    // Insert into a table known to have room and not to already hold `key`.
    void insertFresh(std::int64_t key, std::uint32_t index) {
        std::size_t slot = static_cast<std::size_t>(hash(key)) & mask_;
        while (table_[slot].generation == generation_) {
            slot = (slot + 1U) & mask_;
        }
        table_[slot] = Slot{key, index, generation_};
    }

    // The index for `key`, or -1. Probes only live (current-generation) slots.
    [[nodiscard]] std::int64_t find(std::int64_t key) const {
        if (table_.empty()) {
            return -1;
        }
        std::size_t slot = static_cast<std::size_t>(hash(key)) & mask_;
        while (table_[slot].generation == generation_) {
            if (table_[slot].key == key) {
                return static_cast<std::int64_t>(table_[slot].index);
            }
            slot = (slot + 1U) & mask_;
        }
        return -1;
    }

    [[nodiscard]] static std::uint64_t hash(std::int64_t key) {
        // A fixed integer finaliser (splitmix64) — packed positions are dense in
        // the low bits, so a plain mask would collide neighbouring cells.
        std::uint64_t x = static_cast<std::uint64_t>(key) + 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    // Collect the connected wire cells into `cells_` (discovery order) with the
    // reused index map. cells_ doubles as the BFS queue.
    void floodNetwork(const world::World& world, world::BlockPos start) {
        cells_.clear();
        if (table_.size() < 64U) {
            rebuildMap(32U);
        } else {
            resetMap();
        }

        appendCell(packBlockPos(start), start);
        for (std::size_t head = 0; head < cells_.size(); ++head) {
            const world::BlockPos current = cells_[head];
            for (const Direction dir : kWireTraversalOrder) {
                const world::BlockPos neighbor = relative(current, dir);
                if (!isWire(world, neighbor)) {
                    continue;
                }
                const std::int64_t key = packBlockPos(neighbor);
                if (find(key) >= 0) {
                    continue;
                }
                appendCell(key, neighbor);
            }
        }
    }

    void appendCell(std::int64_t key, world::BlockPos pos) {
        const auto index = static_cast<std::uint32_t>(cells_.size());
        // Keep the table under a 0.7 load factor so probes stay short.
        if (static_cast<std::size_t>(index) * 10U >= table_.size() * 7U) {
            cells_.push_back(pos);
            rebuildMap(cells_.size());
            return;
        }
        cells_.push_back(pos);
        insertFresh(key, index);
    }

    // Seed each cell with its non-wire block signal, then run the descending
    // wavefront to the fixed point.
    void seedAndSettle(const world::World& world) {
        const std::size_t count = cells_.size();
        power_.assign(count, 0);
        for (std::size_t b = 0; b < buckets_.size(); ++b) {
            buckets_[b].clear();
        }
        for (std::size_t i = 0; i < count; ++i) {
            const int signal = wireBlockSignal(world, cells_[i]);
            power_[i] = signal;
            buckets_[static_cast<std::size_t>(signal)].push_back(static_cast<std::uint32_t>(i));
        }

        // Wavefront: finalise levels 15..1. A cell relaxes its wire neighbours to
        // its own power minus one; because power only descends along edges and we
        // walk levels top-down, a cell popped at level p already holds its final
        // power (stale bucket entries — a cell later raised to a higher level —
        // are skipped by the power_ guard).
        for (int level = 15; level >= 1; --level) {
            std::vector<std::uint32_t>& bucket = buckets_[static_cast<std::size_t>(level)];
            for (std::size_t k = 0; k < bucket.size(); ++k) {
                const std::uint32_t i = bucket[k];
                if (power_[i] != level) {
                    continue; // stale: raised past this level by another path
                }
                const int candidate = level - 1;
                if (candidate == 0) {
                    continue;
                }
                const world::BlockPos cell = cells_[i];
                for (const Direction dir : kWireTraversalOrder) {
                    const std::int64_t neighborKey =
                        packBlockPos(relative(cell, dir));
                    const std::int64_t j = find(neighborKey);
                    if (j < 0) {
                        continue;
                    }
                    const auto ji = static_cast<std::size_t>(j);
                    if (candidate > power_[ji]) {
                        power_[ji] = candidate;
                        buckets_[static_cast<std::size_t>(candidate)].push_back(
                            static_cast<std::uint32_t>(ji));
                    }
                }
            }
        }
    }

    // Reused per-tick arena buffers.
    std::vector<world::BlockPos> cells_;
    std::vector<int> power_;
    std::array<std::vector<std::uint32_t>, 16> buckets_;
    std::vector<Slot> table_;
    std::size_t mask_ = 0;
    std::uint32_t generation_ = 0;
    std::vector<WirePower> network_;
};

} // namespace mc::gameplay::redstone
