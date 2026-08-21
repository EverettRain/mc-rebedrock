#pragma once

// The byte form of a chunk's scheduled ticks, the persistence half of W-2.
//
// This is what makes the scheduler's model provably lossless against JE's
// `block_ticks`/`fluid_ticks`: a SavedTick is `(type, pos, delay, priority)` with
// `delay` relative to game time, exactly JE's record, and encode/decode here is a
// straight little-endian frame over SaveStream's primitives. The world save
// format is not bumped by this header — wiring these bytes into world.dat is the
// separate save-interop milestone; what lands now is the codec and the round-trip
// guarantee it carries, so redstone's pending ticks can survive a save the day a
// converter or per-chunk writer calls it.

#include "gameplay/ChunkTickScheduler.hpp"
#include "persistence/SaveStream.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace mc::persistence {

// Little-endian: u32 count, then per tick { u8 task, i32 x, i32 y, i32 z,
// i32 delay, i8 priority }. The order of the list is meaningful — it is the
// drain order, and it is what carries subTickOrder across the save.
inline void appendSavedTicks(std::vector<std::uint8_t>& bytes,
                             std::span<const gameplay::SavedTick> ticks) {
    appendInteger(bytes, static_cast<std::uint32_t>(ticks.size()));
    for (const gameplay::SavedTick& tick : ticks) {
        appendInteger(bytes, static_cast<std::uint8_t>(tick.type));
        appendInteger(bytes, tick.position.x);
        appendInteger(bytes, tick.position.y);
        appendInteger(bytes, tick.position.z);
        appendInteger(bytes, tick.delay);
        appendInteger(bytes, static_cast<std::int8_t>(tick.priority));
    }
}

[[nodiscard]] inline std::vector<gameplay::SavedTick> readSavedTicks(
    std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    const auto count = readInteger<std::uint32_t>(bytes, cursor);
    std::vector<gameplay::SavedTick> ticks;
    ticks.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        gameplay::SavedTick tick;
        const auto task = readInteger<std::uint8_t>(bytes, cursor);
        if (task >= gameplay::kTickTaskCount) {
            throw std::runtime_error("Saved tick names a task outside the schedule");
        }
        tick.type = static_cast<gameplay::TickTask>(task);
        tick.position.x = readInteger<std::int32_t>(bytes, cursor);
        tick.position.y = readInteger<std::int32_t>(bytes, cursor);
        tick.position.z = readInteger<std::int32_t>(bytes, cursor);
        tick.delay = readInteger<std::int32_t>(bytes, cursor);
        tick.priority = static_cast<gameplay::TickPriority>(readInteger<std::int8_t>(bytes, cursor));
        ticks.push_back(tick);
    }
    return ticks;
}

} // namespace mc::persistence
