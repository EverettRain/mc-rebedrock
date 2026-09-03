#pragma once

// W-9: the redstone lamp's sink decision, source-derived from RedstoneLampBlock.
// LIT is both the lamp's output and its edge memory, so the two callers — the
// block-update notification and the delayed scheduled tick — have to agree
// exactly on what "the signal disagrees with the lamp" means. They read it from
// here rather than each restating the comparison.
//
// The asymmetry is the whole behaviour: ON is immediate, OFF is four gameticks
// later and re-checked when it fires, so a signal that flickers off and back on
// inside the window never blinks the lamp.
//
//   RedstoneLampBlock.java:35-46  neighborChanged: isLit != hasNeighborSignal
//                                 -> lit ? scheduleTick(pos, this, 4)
//                                        : setBlock(cycle(LIT), 2)
//   RedstoneLampBlock.java:52-54  tick: LIT && !hasNeighborSignal -> cycle(LIT)

namespace mc::gameplay::redstone {

// gt, RedstoneLampBlock#neighborChanged's `level.scheduleTick(pos, this, 4)`.
inline constexpr int kLampOffDelay = 4;

// What a lamp holding `lit` should do about a cell whose incoming signal is
// `signal`. Named rather than returning a bare bool pair so the two branches at
// the call sites cannot drift apart.
enum class LampEdge : unsigned char {
    None,      // isLit == hasNeighborSignal: vanilla's guard rejects it
    LightNow,  // unlit and powered: write LIT immediately
    ScheduleOff, // lit and unpowered: schedule the delayed extinction
};

[[nodiscard]] inline constexpr LampEdge lampEdge(bool lit, bool signal) {
    if (lit == signal) {
        return LampEdge::None;
    }
    return lit ? LampEdge::ScheduleOff : LampEdge::LightNow;
}

// RedstoneLampBlock#tick's own guard: the scheduled extinction only lands if the
// lamp is still lit and the signal is still gone when it fires.
[[nodiscard]] inline constexpr bool lampTickTurnsOff(bool lit, bool signal) {
    return lit && !signal;
}

} // namespace mc::gameplay::redstone
