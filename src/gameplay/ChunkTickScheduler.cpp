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
                                  std::uint64_t dueTick, bool allowDuplicates) {
    const auto index = static_cast<std::size_t>(task);
    auto& bucket = chunks_[keyOf(position)];
    if (!allowDuplicates && !bucket.queued[index].insert(position).second) {
        return false;
    }
    if (allowDuplicates) {
        bucket.queued[index].insert(position);
    }
    bucket.tasks[index].push_back({position, dueTick, nextSequence_++});
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

void ChunkTickScheduler::clear() {
    chunks_.clear();
    totals_.fill(0U);
    nextSequence_ = 0U;
}

} // namespace mc::gameplay
