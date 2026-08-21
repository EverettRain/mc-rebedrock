#pragma once

// The runtime table of entity-type identities this build's registry cannot
// resolve — a creature a datapack or mod added and then removed. Java keeps such
// an entity as it was rather than dropping it from the world; this is the entity
// analogue of persistence::UnknownBlockTable.
//
// Shape (DOD): each distinct saved species name is interned once into a
// placeholder EntityType kept at a stable address (both the name string and the
// EntityType live in a std::deque so existing entries never move). The
// placeholder carries the original name as its id, an inert no-op AI, a
// zero-size box and the Misc category, so a SimpleEntity that points at it sits
// still in the world and never natural-spawns — and because the save writer
// reads the species back off `type->id().path`, the entity writes out under its
// original name unchanged. Re-adding the content makes the name resolve in the
// real registry again, so the next load produces the true creature instead of a
// placeholder.
//
// byId stays strict (an unknown name is a miss, so /summon cannot conjure a
// placeholder); only the save-restore path resolves through here, exactly like
// UnknownBlockTable is consulted only by the save writer.

#include "gameplay/entities/EntityType.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace mc::gameplay::entities {

// The immutable inert AI every placeholder shares: it installs no goals, so a
// species this build has no code for drives no behaviour.
class UnknownEntityAi final : public EntityAi {
  public:
    void configureBrain(MobBrain& brain) const override { static_cast<void>(brain); }
};

class UnknownEntityTable final {
  public:
    // Interns an unknown species name, deduplicating so repeated load/save cycles
    // of the same creature do not grow the table. Returns a placeholder whose
    // id().path equals `name`, kept at a stable address for the run.
    [[nodiscard]] const EntityType& intern(std::string_view name);

    [[nodiscard]] std::size_t size() const;

  private:
    mutable std::mutex mutex_;
    // Parallel, append-only, index-aligned. std::deque so a push never moves an
    // existing element: the EntityType's id views into names_[i], and a
    // SimpleEntity holds &types_[i].
    std::deque<std::string> names_;
    std::deque<EntityType> types_;
};

// The process-wide table every save/restore path shares, like the block one.
[[nodiscard]] UnknownEntityTable& unknownEntityTable();

} // namespace mc::gameplay::entities
