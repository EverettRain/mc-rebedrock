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

    // Probes.
    [[nodiscard]] bool lit(mc::world::BlockPos rel) const {
        const auto pos = absolute(rel);
        return world_.state(pos.x, pos.y, pos.z).lit();
    }
    [[nodiscard]] int power(mc::world::BlockPos rel) const {
        const auto pos = absolute(rel);
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

} // namespace mc::test::redstone
