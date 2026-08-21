#pragma once

// W-4a: the headless redstone verification harness. It stacks a thin circuit
// builder on the real runtime — GameSession + WorldMutationService +
// GameplayMutationSink + a recording SimulationHost, exactly the double
// mutation_flow_test uses — and drives it one real gametick at a time
// (WorldSimulation::tick + the scheduler drain). Every wiring edit and every
// input goes through WorldMutationService, so the neighbour updater and the
// scheduler fire in the true order; nothing here is a redstone loop of its own.
//
// A FixtureScript turns a source-derived per-gametick table (redstone-reference/)
// into an executable assertion, and runFixture reports the first divergence as
// (gametick, probe, expected vs actual) so a redstone timing bug is located, not
// merely detected.

#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/RedstoneSignal.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace mc::test::redstone {

// A SimulationHost that records nothing of interest — the harness cares about
// world state, not the mesh/audio side effects. Every pure-virtual is a no-op.
struct RecordingHost final : mc::gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

[[nodiscard]] inline mc::world::BlockOrientation orientationOf(gameplay::redstone::Direction dir) {
    using gameplay::redstone::Direction;
    switch (dir) {
    case Direction::North:
        return mc::world::BlockOrientation::North;
    case Direction::East:
        return mc::world::BlockOrientation::East;
    case Direction::South:
        return mc::world::BlockOrientation::South;
    case Direction::West:
        return mc::world::BlockOrientation::West;
    case Direction::Up:
        return mc::world::BlockOrientation::Up;
    case Direction::Down:
        return mc::world::BlockOrientation::Down;
    }
    return mc::world::BlockOrientation::North;
}

class RedstoneCircuit final {
  public:
    explicit RedstoneCircuit(mc::world::BlockPos origin = {0, 64, 0}) : origin_(origin) {
        session_.setEventHost(host_);
        for (int cx = -2; cx <= 2; ++cx) {
            for (int cz = -2; cz <= 2; ++cz) {
                world_.setChunk({cx, cz}, mc::world::Chunk{});
            }
        }
    }

    // Wiring, all through the real placement/mutation pipeline.
    RedstoneCircuit& place(mc::world::BlockPos rel, mc::world::BlockState state) {
        const auto pos = absolute(rel);
        mc::gameplay::GameplayMutationSink sink{world_, session_};
        static_cast<void>(session_.worldMutations().setBlock(
            world_, pos, state, mc::world::MutationFlags::All,
            mc::world::MutationCause::PlayerPlace, sink));
        session_.drainEvents();
        return *this;
    }
    RedstoneCircuit& solid(mc::world::BlockPos rel) {
        return place(rel, mc::world::BlockState{mc::world::Block::Stone});
    }
    RedstoneCircuit& torch(mc::world::BlockPos rel, bool lit = true) {
        return place(rel, mc::world::BlockState{mc::world::Block::RedstoneTorch}.withLit(lit));
    }
    // `connectedDir` is LeverBlock.getConnectedDirection: the side the lever
    // strongly powers, pointing away from the block it hangs on.
    RedstoneCircuit& lever(mc::world::BlockPos rel, gameplay::redstone::Direction connectedDir,
                           bool on = false) {
        return place(rel, mc::world::BlockState{mc::world::Block::Lever, orientationOf(connectedDir)}
                              .withPowered(on));
    }
    // `facing` is the repeater's FACING == its input side (getInputSignal reads
    // one step along it). `delay` is 1-4 ticks.
    RedstoneCircuit& repeater(mc::world::BlockPos rel, gameplay::redstone::Direction facing,
                              int delay = 1) {
        return place(rel, mc::world::BlockState{mc::world::Block::Repeater, orientationOf(facing)}
                              .withRepeaterDelay(delay));
    }
    // `facing` is the comparator's FACING == its back (input) side.
    RedstoneCircuit& comparator(mc::world::BlockPos rel, gameplay::redstone::Direction facing,
                                bool subtract) {
        return place(rel, mc::world::BlockState{mc::world::Block::Comparator, orientationOf(facing)}
                              .withComparatorSubtract(subtract));
    }
    // A block of redstone: a constant weak source, useful as a comparator's back
    // or side input (place to power, place Air over it to remove).
    RedstoneCircuit& redstoneBlock(mc::world::BlockPos rel) {
        return place(rel, mc::world::BlockState{mc::world::Block::RedstoneBlock});
    }
    RedstoneCircuit& clear(mc::world::BlockPos rel) {
        return place(rel, mc::world::BlockState{mc::world::Block::Air});
    }
    RedstoneCircuit& wire(mc::world::BlockPos rel) {
        return place(rel, mc::world::BlockState{mc::world::Block::RedstoneWire});
    }
    // `facing` is the observer's watched side.
    RedstoneCircuit& observer(mc::world::BlockPos rel, gameplay::redstone::Direction facing) {
        return place(rel, mc::world::BlockState{mc::world::Block::Observer, orientationOf(facing)});
    }
    // `facing` is the push direction.
    RedstoneCircuit& piston(mc::world::BlockPos rel, gameplay::redstone::Direction facing,
                            bool sticky = false) {
        const auto block = sticky ? mc::world::Block::StickyPiston : mc::world::Block::Piston;
        return place(rel, mc::world::BlockState{block, orientationOf(facing)});
    }
    // `connectedDir` is the side the button hangs against, as for a lever.
    RedstoneCircuit& button(mc::world::BlockPos rel, gameplay::redstone::Direction connectedDir) {
        return place(rel,
                     mc::world::BlockState{mc::world::Block::StoneButton, orientationOf(connectedDir)});
    }

    // Press == ButtonBlock.press: set POWERED (with the lever's propagation) and
    // schedule the timed release.
    void pressButton(mc::world::BlockPos rel) {
        const auto pos = absolute(rel);
        const auto state = world_.state(pos.x, pos.y, pos.z);
        if (state.block() != mc::world::Block::StoneButton || state.powered()) {
            return;
        }
        const auto next = state.withPowered(true);
        mc::gameplay::GameplayMutationSink sink{world_, session_};
        static_cast<void>(session_.worldMutations().setBlock(
            world_, pos, next, mc::world::MutationFlags::All, mc::world::MutationCause::Command,
            sink));
        session_.worldMutations().updateNeighborsAt(gameplay::redstone::leverMountPos(next, pos),
                                                    sink);
        session_.worldSimulation().scheduleButtonRelease({pos.x, pos.y, pos.z});
        session_.drainEvents();
    }

    // The deterministic input primitive == LeverBlock.pull: toggle POWERED, then
    // updateNeighbours — the lever's own six neighbours (via the write) plus the
    // neighbours of the block it hangs on (the extra fan-out that reaches a torch
    // standing on that mount).
    void setLever(mc::world::BlockPos rel, bool on) {
        const auto pos = absolute(rel);
        const auto state = world_.state(pos.x, pos.y, pos.z);
        if (state.block() != mc::world::Block::Lever) {
            return;
        }
        const auto next = state.withPowered(on);
        mc::gameplay::GameplayMutationSink sink{world_, session_};
        static_cast<void>(session_.worldMutations().setBlock(
            world_, pos, next, mc::world::MutationFlags::All, mc::world::MutationCause::Command,
            sink));
        session_.worldMutations().updateNeighborsAt(gameplay::redstone::leverMountPos(next, pos),
                                                    sink);
        session_.drainEvents();
    }

    // One real gametick each: the runtime's WorldSimulation tick plus its
    // scheduler drain.
    void advance(int gameticks = 1) {
        for (int i = 0; i < gameticks; ++i) {
            static_cast<void>(session_.worldSimulation().tick(world_, true));
            session_.drainEvents();
            ++gameTime_;
        }
    }
    [[nodiscard]] std::uint64_t gameTime() const { return gameTime_; }

    // W-6: select how the redstone drain orders the due ticks. Serial (default)
    // is the ground truth; Island partitions and drains island-major. Set before
    // advancing; wiring and inputs go through the mutation service, not tick(), so
    // the mode only affects the scheduled-tick drain the lockstep compares.
    void setRedstoneDrainMode(mc::gameplay::WorldSimulation::RedstoneDrainMode mode) {
        session_.worldSimulation().setRedstoneDrainMode(mode);
    }
    // The islands the last advance's redstone drain partitioned into (0 in Serial
    // mode). Lets a lockstep assert the partition actually exposed parallelism.
    [[nodiscard]] std::size_t redstoneIslandCount() {
        return session_.worldSimulation().lastRedstoneIslandCount();
    }

    // Probes. `lit` reads a component's on/off state: POWERED for a diode or a
    // lever, LIT for a torch — the one probe redstone-reference tables use.
    [[nodiscard]] bool lit(mc::world::BlockPos rel) const {
        const auto pos = absolute(rel);
        const auto state = world_.state(pos.x, pos.y, pos.z);
        return state.has(mc::world::StateProperty::Powered) ? state.powered() : state.lit();
    }
    [[nodiscard]] int power(mc::world::BlockPos rel) const {
        const auto pos = absolute(rel);
        const auto state = world_.state(pos.x, pos.y, pos.z);
        // A wire (or comparator) carries its level in AnalogSignal; anywhere else
        // read the strongest signal reaching the cell.
        if (state.has(mc::world::StateProperty::AnalogSignal)) {
            return state.analogSignal();
        }
        return gameplay::redstone::getBestNeighborSignal(world_, pos);
    }
    [[nodiscard]] mc::world::BlockState state(mc::world::BlockPos rel) const {
        const auto pos = absolute(rel);
        return world_.state(pos.x, pos.y, pos.z);
    }

  private:
    [[nodiscard]] mc::world::BlockPos absolute(mc::world::BlockPos rel) const {
        return {origin_.x + rel.x, origin_.y + rel.y, origin_.z + rel.z};
    }

    mc::world::World world_;
    mc::gameplay::GameSession session_;
    RecordingHost host_;
    mc::world::BlockPos origin_;
    std::uint64_t gameTime_ = 0;
};

// A fixture = a source-derived table made executable.
struct Probe final {
    mc::world::BlockPos rel;
    enum Kind { Lit, Power } kind = Lit;
};
struct ExpectedRow final {
    std::uint64_t gt = 0;
    std::vector<int> values; // aligned with probes
};
struct FixtureScript final {
    std::vector<Probe> probes;
    std::vector<std::pair<std::uint64_t, std::function<void(RedstoneCircuit&)>>> events;
    std::vector<ExpectedRow> expected;
};

[[nodiscard]] inline int readProbe(const RedstoneCircuit& circuit, const Probe& probe) {
    switch (probe.kind) {
    case Probe::Lit:
        return circuit.lit(probe.rel) ? 1 : 0;
    case Probe::Power:
        return circuit.power(probe.rel);
    }
    return 0;
}

// Runs a fixture: for each gametick that has an event or an expectation, apply
// the events at the start of that gametick (before probing, as Java applies
// input at the tick's head), then assert every probe. Reports the FIRST
// divergence with its gametick and probe, and asserts EVERY expected row, so a
// right end-state reached through a wrong intermediate tick is still caught.
inline void runFixture(RedstoneCircuit& circuit, const FixtureScript& script) {
    std::uint64_t last = 0;
    for (const auto& [gt, fn] : script.events) {
        last = std::max(last, gt);
    }
    for (const ExpectedRow& row : script.expected) {
        last = std::max(last, row.gt);
    }

    for (std::uint64_t gt = 0; gt <= last; ++gt) {
        if (gt != 0) {
            circuit.advance(1);
        }
        // Events at the head of this gametick (before its effects are probed).
        for (const auto& [eventGt, fn] : script.events) {
            if (eventGt == gt) {
                fn(circuit);
            }
        }
        for (const ExpectedRow& row : script.expected) {
            if (row.gt != gt) {
                continue;
            }
            for (std::size_t i = 0; i < script.probes.size(); ++i) {
                const int actual = readProbe(circuit, script.probes[i]);
                const int expected = i < row.values.size() ? row.values[i] : 0;
                if (actual != expected) {
                    std::fprintf(stderr,
                                 "redstone fixture diverged at gt=%llu, probe #%zu: expected %d, "
                                 "got %d\n",
                                 static_cast<unsigned long long>(gt), i, expected, actual);
                    std::abort();
                }
            }
        }
    }
}

// W-6 lockstep: run the same wiring and timed events under both redstone drain
// modes — Serial and Island — and assert every probe agrees bit for bit on every
// gametick. The island drain is only a reordering of the serial drain, so any
// divergence means the partition split a coupled pair (or ordered a shared cell
// wrong); this is the determinism gate the design makes CI. Returns the maximum
// island count the island run reached, so a caller can require the partition
// actually exposed parallelism (> 1) rather than passing vacuously with one
// island. `build` wires an empty circuit; `script.events`/`script.expected` fix
// the timeline (expected values are ignored here — the two runs are compared to
// each other, not to a reference table).
inline std::size_t runLockstep(const std::function<void(RedstoneCircuit&)>& build,
                               const FixtureScript& script) {
    RedstoneCircuit serial;
    RedstoneCircuit island;
    island.setRedstoneDrainMode(mc::gameplay::WorldSimulation::RedstoneDrainMode::Island);
    build(serial);
    build(island);

    std::uint64_t last = 0;
    for (const auto& [gt, fn] : script.events) {
        last = std::max(last, gt);
    }
    for (const ExpectedRow& row : script.expected) {
        last = std::max(last, row.gt);
    }

    std::size_t maxIslands = 0;
    for (std::uint64_t gt = 0; gt <= last; ++gt) {
        if (gt != 0) {
            serial.advance(1);
            island.advance(1);
            maxIslands = std::max(maxIslands, island.redstoneIslandCount());
        }
        for (const auto& [eventGt, fn] : script.events) {
            if (eventGt == gt) {
                fn(serial);
                fn(island);
            }
        }
        for (std::size_t i = 0; i < script.probes.size(); ++i) {
            const int serialValue = readProbe(serial, script.probes[i]);
            const int islandValue = readProbe(island, script.probes[i]);
            if (serialValue != islandValue) {
                std::fprintf(stderr,
                             "redstone lockstep diverged at gt=%llu, probe #%zu: serial %d, "
                             "island %d\n",
                             static_cast<unsigned long long>(gt), i, serialValue, islandValue);
                std::abort();
            }
        }
    }
    return maxIslands;
}

} // namespace mc::test::redstone
