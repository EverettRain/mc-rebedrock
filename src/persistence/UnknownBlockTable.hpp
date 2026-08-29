#pragma once

// The runtime table of block identities this build's registry cannot resolve —
// content a datapack or mod placed and then removed. Java keeps such blocks as
// they were rather than dropping the cell to air; this is where that guarantee
// lives for the C++ save layer.
//
// Shape (DOD): each distinct (name, properties) an unknown palette entry carried
// is interned once into a sentinel BlockState whose raw id sits above the
// built-in state table (world::kFirstUnknownStateId and up). The sentinel travels
// through the chunk-section palette, the mesher and the save gatherer exactly
// like any other u16 state — and because every state-metadata accessor clamps an
// id past the table to block 0, the cell reads as inert air with no special case.
// Only the save writer treats it specially: it maps the sentinel back through
// this table to the original identifier and property bytes and writes them out
// unchanged. Re-adding the content makes the name resolve in the registry again,
// so the next load produces the real block instead of a placeholder.
//
// It is a process-wide singleton, like the block registry, so every write path —
// the full SaveRepository::save and the incremental saveChunk/saveChunks the
// unload worker calls — can resolve a sentinel without threading a table through
// every signature. Interning happens only when a save mentions content this build
// lacks, never on a hot path, so the guarding mutex costs nothing that matters.

#include "world/BlockState.hpp"
#include "world/BlockStateTable.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mc::persistence {

// One property name/value pair as it sat on disk, kept verbatim so an unknown
// block's state blob writes back byte-for-byte.
struct UnknownStateProperty final {
    std::string name;
    std::uint8_t value = 0U;
    [[nodiscard]] bool operator==(const UnknownStateProperty&) const = default;
};

// The original identity of a block the registry cannot resolve: the full
// "namespace:path" it was saved under, and every property it carried in the
// order they were written.
struct UnknownBlockState final {
    std::string name;
    std::vector<UnknownStateProperty> properties;
    [[nodiscard]] bool operator==(const UnknownBlockState&) const = default;
};

class UnknownBlockTable final {
  public:
    // Interns an unknown identity, deduplicating identical (name, properties) so
    // repeated load/save cycles of the same cell do not grow the table. Returns
    // the sentinel BlockState the rest of the engine carries in the cell's place.
    [[nodiscard]] world::BlockState intern(std::string name,
                                           std::vector<UnknownStateProperty> properties) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (std::size_t index = 0; index < records_.size(); ++index) {
            if (records_[index].name == name && records_[index].properties == properties) {
                return sentinelOf(index);
            }
        }
        if (static_cast<std::uint32_t>(world::kFirstUnknownStateId) + records_.size() > kMaxRawId) {
            // Tens of thousands of distinct unknown states in one world is not a
            // real save; refuse rather than wrap an id back into the built-in
            // table and silently alias a placeholder onto a real block.
            throw std::runtime_error("save references more unknown blocks than can be held");
        }
        const std::size_t index = records_.size();
        records_.push_back({std::move(name), std::move(properties)});
        return sentinelOf(index);
    }

    // Whether a state is an unknown-block placeholder rather than a real interned
    // state. A pure range test on the raw id, so it needs no lock.
    [[nodiscard]] bool isUnknown(world::BlockState state) const {
        return world::isUnknownStateId(state.rawId());
    }

    // The original identity behind a sentinel, by value so the caller never holds
    // a reference into a vector another thread might grow. Throws if the state is
    // not one this table interned.
    [[nodiscard]] UnknownBlockState record(world::BlockState state) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!world::isUnknownStateId(state.rawId())) {
            throw std::runtime_error("block state is not an unknown-block placeholder");
        }
        const std::size_t index =
            static_cast<std::size_t>(state.rawId() - world::kFirstUnknownStateId);
        if (index >= records_.size()) {
            throw std::runtime_error("unknown-block sentinel has no record");
        }
        return records_[index];
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return records_.size();
    }

  private:
    // The state id is a u32 now, so the placeholder space above the built-in
    // table runs to the full 32-bit ceiling (BlockStateTable keeps 0xFFFF0000 of
    // headroom below this for exactly these ids).
    static constexpr std::uint32_t kMaxRawId = 0xFFFFFFFFU;

    [[nodiscard]] static world::BlockState sentinelOf(std::size_t index) {
        return world::BlockState::fromRawId(
            static_cast<std::uint32_t>(world::kFirstUnknownStateId + index));
    }

    mutable std::mutex mutex_;
    std::vector<UnknownBlockState> records_;
};

// The process-wide table every save/load path shares.
[[nodiscard]] inline UnknownBlockTable& unknownBlockTable() {
    static UnknownBlockTable table;
    return table;
}

} // namespace mc::persistence
