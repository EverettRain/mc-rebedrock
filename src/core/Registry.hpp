#pragma once

// A generic content registry: the single source of runtime identity for one
// kind of content.
//
// Shape (DOD): a `vector<Def>` indexed straight by the dense id it hands out
// (deref = one subscript, zero heap indirection), an interner that maps
// Identifier -> a dense IdentifierId, and a `vector<Id>` indexed by that
// IdentifierId as the name -> id "flat map". A reference to content is the id
// value; the registry is the only thing that turns it back into a Def.
//
// Lifecycle: every registry walks a three-phase state machine so "add content"
// becomes "hang a definition on a phase", not "edit an enum and a switch".
//
//   Bootstrap  -> External              -> Freeze
//   built-in      datapack / mod / etc.    world load onward:
//   content       content                  ids are final, no more registration
//
//   * Built-in content registers first (registerBuiltin, Bootstrap only) so the
//     vanilla-mirroring ids stay stable and the remap surface is smallest.
//   * External content registers after (registerExternal, External only).
//   * Freeze locks the table; any registration afterwards aborts.
//
// Registering in the wrong phase, registering a name twice, or dereferencing an
// invalid id all abort rather than silently hand back the wrong content — the
// failure a save/network identity bug would otherwise become. These aborts are
// the assertions the tests fork against.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/IdentifierInterner.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::core {

enum class RegistryPhase : std::uint8_t {
    Bootstrap,
    External,
    Freeze,
};

[[noreturn]] inline void registryAbort(const char* message) {
    std::fputs("registry fatal: ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

template <typename Def, typename Id>
class Registry final {
  public:
    // Registers built-in content. Legal only in the Bootstrap phase; the id is
    // assigned in registration order, so built-in ids are stable across runs.
    Id registerBuiltin(const Identifier& name, Def definition) {
        requirePhase(RegistryPhase::Bootstrap, "registerBuiltin outside Bootstrap phase");
        return insert(name, std::move(definition));
    }

    // Registers external (datapack / mod) content. Legal only in the External
    // phase, which opens after every built-in has claimed its id.
    Id registerExternal(const Identifier& name, Def definition) {
        requirePhase(RegistryPhase::External, "registerExternal outside External phase");
        return insert(name, std::move(definition));
    }

    // Files an extra name that resolves to an already-registered id — a
    // block's `minecraft:` alias beside its `rebedrock:` key, say. Legal in any
    // phase before Freeze.
    void alias(const Identifier& aliasName, Id target) {
        if (phase_ == RegistryPhase::Freeze)
            registryAbort("alias after Freeze");
        if (!inRange(target))
            registryAbort("alias targets an id this registry never assigned");
        bindName(aliasName, target);
    }

    // Advances Bootstrap -> External. Anything else (already External, already
    // Freeze) aborts, so the "built-in first, external second" order cannot be
    // skipped or replayed.
    void beginExternal() {
        if (phase_ != RegistryPhase::Bootstrap)
            registryAbort("beginExternal from a phase other than Bootstrap");
        phase_ = RegistryPhase::External;
    }

    // Locks the registry. Legal from Bootstrap (a build with no external
    // content) or External; freezing twice aborts.
    void freeze() {
        if (phase_ == RegistryPhase::Freeze)
            registryAbort("freeze when already frozen");
        phase_ = RegistryPhase::Freeze;
    }

    [[nodiscard]] RegistryPhase phase() const { return phase_; }

    // Resolves a key to its id, or an invalid id when nothing owns that name.
    // Never inserts, never aborts — an unknown name is an expected answer here.
    [[nodiscard]] Id byName(const Identifier& name) const {
        const IdentifierId interned = names_.find(name);
        if (!interned.valid() || interned.index() >= byName_.size())
            return Id::invalid();
        return byName_[interned.index()];
    }

    // Same, parsing a `space:path` string. A bare `path` (no colon) matches any
    // namespace by walking the registered names — a boundary convenience for
    // commands and tests, never a hot path.
    [[nodiscard]] Id byName(std::string_view text) const {
        const Identifier parsed = Identifier::parse(text);
        if (!parsed.space.empty())
            return byName(parsed);
        for (std::size_t index = 0; index < canonicalName_.size(); ++index) {
            if (names_.identifier(canonicalName_[index]).matches(text))
                return Id::of(static_cast<typename Id::Value>(index));
        }
        return Id::invalid();
    }

    // Dereferences an id to its definition. Aborts on an invalid or
    // out-of-range id: callers hold ids they got from this registry, so a bad
    // one is a bug, not a lookup miss.
    [[nodiscard]] const Def& get(Id id) const {
        if (!inRange(id))
            registryAbort("get on an id this registry never assigned");
        return definitions_[id.index()];
    }

    // The canonical (primary) Identifier an id was registered under, viewing the
    // interner's stable storage.
    [[nodiscard]] const Identifier& identifier(Id id) const {
        if (!inRange(id))
            registryAbort("identifier() on an id this registry never assigned");
        return names_.identifier(canonicalName_[id.index()]);
    }

    [[nodiscard]] std::size_t size() const { return definitions_.size(); }

  private:
    Id insert(const Identifier& name, Def definition) {
        if (definitions_.size() >= static_cast<std::size_t>(Id::kInvalidValue))
            registryAbort("registry is full (would collide with the invalid-id sentinel)");
        const auto id = Id::of(static_cast<typename Id::Value>(definitions_.size()));
        const IdentifierId primary = bindName(name, id);
        definitions_.push_back(std::move(definition));
        canonicalName_.push_back(primary);
        return id;
    }

    // Points a name (primary or alias) at an id. A name already bound to some id
    // is a collision — two pieces of content claiming one key — and aborts
    // rather than silently overwriting the earlier binding.
    IdentifierId bindName(const Identifier& name, Id target) {
        const IdentifierId interned = names_.intern(name);
        if (interned.index() < byName_.size() && byName_[interned.index()].valid())
            registryAbort("duplicate registry name");
        if (interned.index() >= byName_.size())
            byName_.resize(interned.index() + 1U, Id::invalid());
        byName_[interned.index()] = target;
        return interned;
    }

    void requirePhase(RegistryPhase expected, const char* message) const {
        if (phase_ != expected)
            registryAbort(message);
    }

    [[nodiscard]] bool inRange(Id id) const {
        return id.valid() && id.index() < definitions_.size();
    }

    IdentifierInterner names_;
    std::vector<Id> byName_;                // IdentifierId::index() -> content Id
    std::vector<Def> definitions_;          // Id::index() -> Def
    std::vector<IdentifierId> canonicalName_; // Id::index() -> primary IdentifierId
    RegistryPhase phase_ = RegistryPhase::Bootstrap;
};

} // namespace mc::core
