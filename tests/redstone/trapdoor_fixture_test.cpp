#include "redstone/RedstoneHarness.hpp"

// W-signal fixture: the oak trapdoor as a redstone SINK (never a source — it is
// not in isSignalSource/the emission tables). TrapDoorBlock.neighborChanged
// reacts synchronously, in the same block-update pass, unlike a torch's
// scheduled 2gt toggle: a lever toggling on powers the trapdoor's mount cell,
// notifyRedstoneComponent reads getBestNeighborSignal and flips OPEN (and
// POWERED, its edge-memory) the very same gametick the lever's own
// updateNeighbours call lands. Lever off closes it again, same-tick.
// Sabotage③'s target: a trapdoor that ignores its neighbour signal (no sink
// reaction at all) stays closed forever.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // A wall to hang the lever on; the trapdoor is mounted directly beside it,
    // in the lever's strongly-charged FACING cell, so it is on the lever's own
    // six neighbours (no wire hop needed to prove the sink reaction).
    circuit.solid({-1, 0, -1});
    circuit.lever({-1, 0, 0}, Direction::South, false).trapdoor({0, 0, 0}, Direction::North);

    const BlockPos leverPos{-1, 0, 0};
    const BlockPos trapdoorPos{0, 0, 0};

    FixtureScript script;
    // `lit()` reads POWERED when the block has that axis (it now does, as the
    // sink's edge-memory), which tracks OPEN 1:1 in this fixture (both are set
    // to `signal` together in the same write) — checked directly below too.
    script.probes = {Probe{trapdoorPos, Probe::Lit}};
    script.events = {
        {5, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, true); }},
        {10, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, false); }},
    };
    // Same-gametick reaction: the lever's own updateNeighbours call at t=5/t=10
    // reaches the trapdoor synchronously, so POWERED (and OPEN) flip the same
    // tick the event fires, not one gametick later the way a scheduled
    // component (a torch's 2gt toggle) would.
    script.expected = {
        {0, {0}},
        {5, {1}}, // lever on -> trapdoor opens immediately
        {6, {1}},
        {10, {0}}, // lever off -> trapdoor closes immediately
        {11, {0}},
    };
    runFixture(circuit, script);

    // OPEN itself, read directly rather than through the POWERED-aliasing
    // probe, at the two settled ticks.
    circuit.advance(0); // no-op; state is already settled from the script above
    if (circuit.state(trapdoorPos).open()) {
        std::fprintf(stderr, "trapdoor fixture: expected OPEN=false after lever off\n");
        std::abort();
    }

    // The trapdoor's own POWERED (edge-memory) tracks the signal too, and it is
    // not a signal source itself — confirms it never entered the emission
    // tables even though it now reacts to one.
    using mc::gameplay::redstone::isSignalSource;
    static_assert(!isSignalSource(mc::world::Block::OakTrapdoor));

    // AR-B4-3: every trapdoor in the roster, not just the oak one. The sink used
    // to be `block == Block::OakTrapdoor`, so these five were inert — a lever
    // beside a spruce or iron trapdoor did nothing. Dispatch is on the model
    // now, so this list is a regression guard against sliding back to a single
    // identity, and it is parameterised precisely so a seventh trapdoor is
    // covered by adding one line rather than by remembering to.
    for (const mc::world::Block kind :
         {mc::world::Block::OakTrapdoor, mc::world::Block::SpruceTrapdoor,
          mc::world::Block::JungleTrapdoor, mc::world::Block::IronTrapdoor,
          mc::world::Block::OxidizedCopperTrapdoor,
          mc::world::Block::WaxedOxidizedCopperTrapdoor}) {
        // Every one of them really is a TrapDoor model — otherwise this loop
        // would be asserting nothing about the dispatch it is guarding.
        if (mc::world::blockDefinition(kind).model != mc::world::BlockModel::TrapDoor) {
            std::fprintf(stderr, "trapdoor fixture: %s is not a TrapDoor model\n",
                         mc::world::blockDefinition(kind).identifier.path.data());
            std::abort();
        }
        RedstoneCircuit each;
        each.solid({-1, 0, -1});
        each.lever({-1, 0, 0}, Direction::South, false).trapdoor({0, 0, 0}, Direction::North, kind);
        const BlockPos pos{0, 0, 0};
        if (each.state(pos).open()) {
            std::fprintf(stderr, "trapdoor fixture: %s started open\n",
                         mc::world::blockDefinition(kind).identifier.path.data());
            std::abort();
        }
        each.setLever({-1, 0, 0}, true);
        if (!each.state(pos).open() || !each.state(pos).powered()) {
            std::fprintf(stderr, "trapdoor fixture: %s ignored its redstone signal\n",
                         mc::world::blockDefinition(kind).identifier.path.data());
            std::abort();
        }
        each.setLever({-1, 0, 0}, false);
        if (each.state(pos).open()) {
            std::fprintf(stderr, "trapdoor fixture: %s stayed open after the lever fell\n",
                         mc::world::blockDefinition(kind).identifier.path.data());
            std::abort();
        }
    }

    return 0;
}
