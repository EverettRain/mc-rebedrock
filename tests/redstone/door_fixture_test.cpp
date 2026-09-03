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

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "door fixture: %s\n", what);
        std::abort();
    }
}

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

    // Both halves have to be *published*, not merely written. The world and the
    // client are two different questions here: a redstone sink writes through
    // its own sink inside notifyRedstoneComponent, and what reaches the renderer
    // is whatever a WorldEditEvent was published for. When the only synchronous
    // sink was the trapdoor — one cell — the publish site could get away with a
    // before/after compare of the one notified cell. A door writes two, and the
    // partner half is not the cell anyone was asked about, so it went
    // unpublished: correct in the world, half-open on screen.
    {
        RedstoneCircuit published;
        published.solid({-1, 0, -1});
        published.lever({-1, 0, 0}, Direction::South, false).door({0, 0, 0}, Direction::North);
        published.recordWorldEdits();
        published.clearPublishedEdits();
        published.setLever(leverPos, true);
        requireHalves(published, lowerPos, true, "published: lever on");
        if (!published.publishedEdit(lowerPos)) {
            std::fprintf(stderr, "door fixture: the lower half's change was never published\n");
            std::abort();
        }
        if (!published.publishedEdit(upperPos)) {
            std::fprintf(stderr,
                         "door fixture: the upper half changed in the world but no WorldEditEvent "
                         "was published for it — the client would render a half-open door\n");
            std::abort();
        }
        // ...and the same on the way back down, so the door does not get stuck
        // half-open on screen when it closes either.
        published.clearPublishedEdits();
        published.setLever(leverPos, false);
        requireHalves(published, lowerPos, false, "published: lever off");
        if (!published.publishedEdit(lowerPos) || !published.publishedEdit(upperPos)) {
            std::fprintf(stderr, "door fixture: closing published only one half\n");
            std::abort();
        }
    }

    // The synchronous-write list the fix above reads must not accumulate. Every
    // path that lets a sink append to it drains it; a path that forgets turns a
    // per-tick append into a vector that grows for the life of the session. A
    // scheduled component appends its own write there on every tick, so this is
    // exercised constantly rather than only by doors.
    {
        RedstoneCircuit busy;
        busy.solid({-1, 0, -1});
        busy.lever({-1, 0, 0}, Direction::South, false).door({0, 0, 0}, Direction::North);
        // A torch inverter as well as the door: the door exercises the
        // synchronous publish path, and the torch is a *scheduled* component, so
        // its 2gt toggle runs through dispatchRedstoneTick — the other drain,
        // and the one a purely synchronous circuit never reaches. The bench is
        // torch_inverter_fixture_test's, driven by its own lever.
        busy.solid({4, 0, 0})
            .torch({4, 1, 0}, /*lit=*/true)
            .lever({5, 0, 0}, Direction::East, /*on=*/false);
        const BlockPos torchLever{5, 0, 0};
        for (int i = 0; i < 8; ++i) {
            busy.setLever(leverPos, i % 2 == 0);
            busy.setLever(torchLever, i % 2 == 0);
            busy.advance(3); // long enough for the torch's 2gt toggle to fire
        }
        if (busy.pendingSynchronousWrites() != 0U) {
            std::fprintf(stderr,
                         "door fixture: %zu synchronous writes were recorded and never drained\n",
                         busy.pendingSynchronousWrites());
            std::abort();
        }
        requireHalves(busy, lowerPos, false, "after the drain loop");
    }

    // NOT covered here, deliberately: driving a door through a *repeater*. A
    // diode's scheduled write goes out with flags 2 and this build never wakes
    // the block in front of it, so no sink downstream of a repeater is ever
    // notified — measured with a trapdoor too, which has been a sink since
    // W-signal and predates every door change. That is a gap in the diode's
    // output propagation, not in this sink, and it is registered as such rather
    // than asserted here where it would read as a door defect.

    // AR-B4-5: an iron door refuses a hand and answers redstone, which is the
    // whole reason to build one. Both halves of that are asserted together —
    // "cannot be opened by hand" alone would also be satisfied by a door that
    // cannot be opened at all.
    {
        RedstoneCircuit iron;
        iron.solid({-1, 0, -1});
        iron.lever({-1, 0, 0}, Direction::South, false)
            .door({0, 0, 0}, Direction::North, mc::world::Block::IronDoor);
        require(!iron.openByHand(lowerPos), "an iron door must not answer a hand");
        require(!iron.state(lowerPos).open(), "and stays shut after the click");
        iron.setLever(leverPos, true);
        require(iron.state(lowerPos).open() && iron.state(upperPos).open(),
                "but redstone opens it, both halves");

        // The control: an oak door in the same place does answer a hand, so the
        // assertion above is about the material and not about the click path.
        RedstoneCircuit oak;
        oak.solid({-1, 0, -1});
        oak.lever({-1, 0, 0}, Direction::South, false).door({0, 0, 0}, Direction::North);
        require(oak.openByHand(lowerPos), "an oak door answers a hand");
        require(oak.state(lowerPos).open() && oak.state(upperPos).open(),
                "and both halves move");

        // Copper is not iron: metal, but hand-openable (BlockSetType.COPPER).
        RedstoneCircuit copper;
        copper.solid({-1, 0, -1});
        copper.lever({-1, 0, 0}, Direction::South, false)
            .door({0, 0, 0}, Direction::North, mc::world::Block::WaxedCopperDoor);
        require(copper.openByHand(lowerPos), "a copper door answers a hand");
    }

    // A door is a sink, never a source: it must not have entered the emission
    // tables along the way.
    using mc::gameplay::redstone::isSignalSource;
    static_assert(!isSignalSource(mc::world::Block::OakDoor));
    static_assert(!isSignalSource(mc::world::Block::IronDoor));

    return 0;
}
