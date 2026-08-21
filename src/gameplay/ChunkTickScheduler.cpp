#include "gameplay/ChunkTickScheduler.hpp"

#include <functional>

namespace mc::gameplay {
namespace {

// Matching World's chunk addressing: a floor division, so negative coordinates
// bucket the same way the world does rather than truncating towards zero.
[[nodiscard]] int floorDivide(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

} // namespace

std::size_t SimulationPositionHash::operator()(const SimulationPosition& position) const noexcept {
    std::size_t seed = std::hash<int>{}(position.x);
    seed ^= std::hash<int>{}(position.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<int>{}(position.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

bool ChunkTickScheduler::Bucket::empty() const {
    for (const auto& entries : tasks) {
        if (!entries.empty()) {
            return false;
        }
    }
    return true;
}

ChunkTickScheduler::ChunkKey ChunkTickScheduler::keyOf(SimulationPosition position) {
    return {floorDivide(position.x, world::kChunkWidth),
            floorDivide(position.z, world::kChunkDepth)};
}

bool ChunkTickScheduler::schedule(TickTask task, SimulationPosition position,
                                  std::uint64_t dueTick, bool allowDuplicates,
                                  TickPriority priority) {
    const auto index = static_cast<std::size_t>(task);
    auto& bucket = chunks_[keyOf(position)];
    if (!allowDuplicates && !bucket.queued[index].insert(position).second) {
        return false;
    }
    if (allowDuplicates) {
        bucket.queued[index].insert(position);
    }
    bucket.tasks[index].push_back({position, dueTick, priority, nextSequence_++});
    ++totals_[index];
    return true;
}

void ChunkTickScheduler::remove(TickTask task, SimulationPosition position) {
    const auto index = static_cast<std::size_t>(task);
    const auto found = chunks_.find(keyOf(position));
    if (found == chunks_.end()) {
        return;
    }
    auto& entries = found->second.tasks[index];
    // Only the oldest entry for the cell goes: a duplicate-allowing task may
    // hold several, and each is a separate unit of work.
    for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
        if (entry->position == position) {
            entries.erase(entry);
            --totals_[index];
            break;
        }
    }
    const bool stillQueued = std::ranges::any_of(
        entries, [position](const ScheduledTick& entry) { return entry.position == position; });
    if (!stillQueued) {
        found->second.queued[index].erase(position);
    }
    if (found->second.empty()) {
        chunks_.erase(found);
    }
}

bool ChunkTickScheduler::contains(TickTask task, SimulationPosition position) const {
    const auto found = chunks_.find(keyOf(position));
    return found != chunks_.end() &&
           found->second.queued[static_cast<std::size_t>(task)].contains(position);
}

void ChunkTickScheduler::forgetChunk(int chunkX, int chunkZ) {
    const auto found = chunks_.find(ChunkKey{chunkX, chunkZ});
    if (found == chunks_.end()) {
        return;
    }
    for (std::size_t index = 0; index < kTickTaskCount; ++index) {
        totals_[index] -= found->second.tasks[index].size();
    }
    chunks_.erase(found);
}

std::vector<std::pair<int, int>> ChunkTickScheduler::scheduledChunks() const {
    std::vector<std::pair<int, int>> result;
    result.reserve(chunks_.size());
    for (const auto& [chunk, bucket] : chunks_) {
        result.emplace_back(chunk.x, chunk.z);
    }
    return result;
}

namespace {

// A pending tick paired with the task it belongs to — ScheduledTick alone does
// not carry its task, since the task is the bucket it lives in.
struct TaskedTick final {
    TickTask task = TickTask::FallingBlock;
    ScheduledTick tick;
};

void collectBucket(const std::array<std::vector<ScheduledTick>, kTickTaskCount>& tasks,
                   std::vector<TaskedTick>& out) {
    for (std::size_t index = 0; index < kTickTaskCount; ++index) {
        for (const ScheduledTick& tick : tasks[index]) {
            out.push_back({static_cast<TickTask>(index), tick});
        }
    }
}

[[nodiscard]] std::vector<SavedTick> toSavedTicks(std::vector<TaskedTick>& collected,
                                                  std::uint64_t gameTime) {
    // Drain order, so subTickOrder can be dropped and rebuilt from list position
    // on import.
    std::ranges::sort(collected, [](const TaskedTick& left, const TaskedTick& right) {
        return ChunkTickScheduler::drainOrder(left.tick, right.tick);
    });
    std::vector<SavedTick> saved;
    saved.reserve(collected.size());
    for (const TaskedTick& entry : collected) {
        const auto delay = static_cast<std::int32_t>(static_cast<std::int64_t>(entry.tick.dueTick) -
                                                     static_cast<std::int64_t>(gameTime));
        saved.push_back({entry.task, entry.tick.position, delay, entry.tick.priority});
    }
    return saved;
}

} // namespace

std::vector<SavedTick> ChunkTickScheduler::exportSavedTicks(std::uint64_t gameTime) const {
    std::vector<TaskedTick> collected;
    for (const auto& [chunk, bucket] : chunks_) {
        collectBucket(bucket.tasks, collected);
    }
    return toSavedTicks(collected, gameTime);
}

std::vector<SavedTick> ChunkTickScheduler::exportSavedTicks(int chunkX, int chunkZ,
                                                            std::uint64_t gameTime) const {
    std::vector<TaskedTick> collected;
    const auto found = chunks_.find(ChunkKey{chunkX, chunkZ});
    if (found != chunks_.end()) {
        collectBucket(found->second.tasks, collected);
    }
    return toSavedTicks(collected, gameTime);
}

void ChunkTickScheduler::importSavedTicks(std::uint64_t gameTime,
                                          std::span<const SavedTick> ticks) {
    // List position becomes subTickOrder: exportSavedTicks emitted them already
    // sorted by drainOrder, so scheduling them in sequence rebuilds that order.
    // Falling blocks keep their duplicate-allowing behaviour on the way back in.
    for (const SavedTick& tick : ticks) {
        const auto dueTick = static_cast<std::uint64_t>(static_cast<std::int64_t>(gameTime) +
                                                        tick.delay);
        static_cast<void>(schedule(tick.type, tick.position, dueTick,
                                    tick.type == TickTask::FallingBlock, tick.priority));
    }
}

void ChunkTickScheduler::clear() {
    chunks_.clear();
    totals_.fill(0U);
    nextSequence_ = 0U;
}

} // namespace mc::gameplay
