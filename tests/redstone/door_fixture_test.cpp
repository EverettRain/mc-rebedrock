#include "redstone/RedstoneHarness.hpp"

// AR-B4-3 fixture: the door as a redstone SINK. Before this node the sink was a
// single hardcoded identity (`block == Block::OakTrapdoor`), so a door had no
// redstone behaviour whatsoever — a lever beside one did nothing at all. This
// pins the ported DoorBlock#neighborChanged:
//
//   signal = hasNeighborSignal(pos) || hasNeighborSignal(other half)
//   if (signal != POWERED) setBlock(OPEN=signal, POWERED=signal, flags 2)
//
// Two things beyond "it opens" matter here and are asserted per gametick:
//   1. Same-gametick reaction. Like the trapdoor, a door is not a scheduled
//      component: the lever's own updateNeighbours call reaches it inside the
//      same tick, so OPEN flips at t=5, not t=6.
//   2. Both halves move together. The signal is read over both halves and both
//      cells are written, so a lever that only touches the lower half must
//      still open the top one. A door standing half-open is the visible bug
//      this exists to prevent.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

void requireHalves(RedstoneCircuit& circuit, BlockPos lower, bool open, const char* when) {
    const BlockPos upper{lower.x, lower.y + 1, lower.z};
    const auto lowerState = circuit.state(lower);
    const auto upperState = circuit.state(upper);
    // The two halves are still one door...
    if (lowerState.block() != mc::world::Block::OakDoor ||
        upperState.block() != mc::world::Block::OakDoor) {
        std::fprintf(stderr, "door fixture (%s): a half stopped being a door\n", when);
        std::abort();
    }
    if (lowerState.isDoorUpperHalf() || !upperState.isDoorUpperHalf()) {
        std::fprintf(stderr, "door fixture (%s): the halves swapped\n", when);
        std::abort();
    }
    // ...and they agree, on OPEN and on the POWERED edge-memory behind it.
    if (lowerState.open() != open || upperState.open() != open) {
        std::fprintf(stderr, "door fixture (%s): expected OPEN=%d, got lower=%d upper=%d\n", when,
                     static_cast<int>(open), static_cast<int>(lowerState.open()),
                     static_cast<int>(upperState.open()));
        std::abort();
    }
    if (lowerState.powered() != open || upperState.powered() != open) {
        std::fprintf(stderr, "door fixture (%s): POWERED did not track OPEN\n", when);
        std::abort();
    }
}

} // namespace

int main() {
    RedstoneCircuit circuit;
    // A wall for the lever, and the door beside it. The lever is level with the
    // door's LOWER half only — the upper half is two cells from it and is never
    // notified, which is precisely why the sink reads the signal over both
    // halves and writes both.
    circuit.solid({-1, 0, -1});
    circuit.lever({-1, 0, 0}, Direction::South, false).door({0, 0, 0}, Direction::North);

    const BlockPos leverPos{-1, 0, 0};
    const BlockPos lowerPos{0, 0, 0};
    const BlockPos upperPos{0, 1, 0};

    FixtureScript script;
    // Both halves are probed. `lit()` reads POWERED here (the door declares it
    // since AR-B4-2), which the sink writes together with OPEN.
    script.probes = {Probe{lowerPos, Probe::Lit}, Probe{upperPos, Probe::Lit}};
    script.events = {
        {5, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, true); }},
        {10, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, false); }},
    };
    script.expected = {
        {0, {0, 0}},
        {4, {0, 0}},
        {5, {1, 1}}, // lever on -> both halves open the same gametick
        {6, {1, 1}},
        {9, {1, 1}},
        {10, {0, 0}}, // lever off -> both halves close the same gametick
        {11, {0, 0}},
    };
    runFixture(circuit, script);
    requireHalves(circuit, lowerPos, false, "after lever off");

    // The same door driven from the UPPER half instead: a lever level with the
    // top must open the bottom, which is the other direction of the same
    // both-halves rule and would pass trivially if the sink only ever looked
    // upward from the notified cell.
    RedstoneCircuit fromTop;
    fromTop.solid({-1, 1, -1});
    fromTop.lever({-1, 1, 0}, Direction::South, false).door({0, 0, 0}, Direction::North);
    const BlockPos topLever{-1, 1, 0};
    FixtureScript topScript;
    topScript.probes = {Probe{lowerPos, Probe::Lit}, Probe{upperPos, Probe::Lit}};
    topScript.events = {
        {5, [topLever](RedstoneCircuit& c) { c.setLever(topLever, true); }},
        {10, [topLever](RedstoneCircuit& c) { c.setLever(topLever, false); }},
    };
    topScript.expected = {
        {0, {0, 0}},
        {5, {1, 1}},
        {6, {1, 1}},
        {10, {0, 0}},
        {11, {0, 0}},
    };
    runFixture(fromTop, topScript);
    requireHalves(fromTop, lowerPos, false, "after top lever off");

    // The both-halves *read*, which neither script above actually needs: in both
    // of them the notified half is the one beside the lever, so it sees the
    // signal itself. The union only earns its keep when a half is notified while
    // the signal sits at the other one — and getting that wrong does not merely
    // fail to open the door, it slams an open door shut.
    //
    // Two levers, one beside each half. Open the door from the top lever, then
    // pulse the bottom one on and off again. The off-pulse notifies the LOWER
    // half at a moment when the lower half's own neighbours carry nothing but
    // the top lever is still holding the door open. Reading only the notified
    // half's neighbours yields signal=false against POWERED=true — a phantom
    // falling edge — and the door closes under a lever that is still on.
    {
        RedstoneCircuit both;
        both.solid({-1, 0, -1});
        both.solid({-1, 1, -1});
        both.lever({-1, 0, 0}, Direction::South, false)
            .lever({-1, 1, 0}, Direction::South, false)
            .door({0, 0, 0}, Direction::North);
        const BlockPos bottomLever{-1, 0, 0};
        const BlockPos topLeverPos{-1, 1, 0};

        both.setLever(topLeverPos, true);
        requireHalves(both, lowerPos, true, "top lever holds it open");

        both.setLever(bottomLever, true);
        requireHalves(both, lowerPos, true, "both levers on");

        // The discriminating step.
        both.setLever(bottomLever, false);
        requireHalves(both, lowerPos, true, "bottom lever released, top still on");

        // And releasing the last one really does close it, so the assertion
        // above is not just "the door never closes".
        both.setLever(topLeverPos, false);
        requireHalves(both, lowerPos, false, "both levers off");
    }

    // An iron door reacts to redstone exactly as an oak one does — the sink is
    // dispatched on the model, so every door in the roster is wired at once.
    // (Whether a *hand* can open it is BlockSetType's business, AR-B4-5.)
    RedstoneCircuit iron;
    iron.solid({-1, 0, -1});
    iron.lever({-1, 0, 0}, Direction::South, false)
        .door({0, 0, 0}, Direction::North, mc::world::Block::IronDoor);
    iron.setLever(leverPos, true);
    if (!iron.state(lowerPos).open() || !iron.state(upperPos).open()) {
        std::fprintf(stderr, "door fixture: an iron door ignored its redstone signal\n");
        std::abort();
    }

    // A door is a sink, never a source: it must not have entered the emission
    // tables along the way.
    using mc::gameplay::redstone::isSignalSource;
    static_assert(!isSignalSource(mc::world::Block::OakDoor));
    static_assert(!isSignalSource(mc::world::Block::IronDoor));

    return 0;
}
