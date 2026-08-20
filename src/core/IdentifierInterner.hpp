#pragma once

// Interns Identifiers into dense IdentifierIds.
//
// An Identifier is two string_views; comparing keys on a hot path means two
// string compares. Interning maps each distinct `space:path` to a small integer
// once, so afterwards code compares an IdentifierId (a `uint16`) instead. The
// Registry uses this as its name -> id "flat map": the interner hands out a
// dense IdentifierId, and the registry keeps a `vector<Id>` indexed straight by
// it, no hashing on the lookup that matters.
//
// The interner owns a stable copy of every key string. That is the hardening
// the raw Identifier lacks: an Identifier built from vanilla string literals has
// static-lifetime views, but one parsed from a command line or a save-file
// palette is backed by a temporary. Copying the key here means a returned
// canonical Identifier stays valid for the interner's whole life regardless of
// where the caller's string came from.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::core {

class IdentifierInterner final {
  public:
    // Returns the id for `id`, assigning a fresh one the first time it is seen.
    // The same Identifier always interns to the same IdentifierId.
    IdentifierId intern(const Identifier& id) {
        std::string composed = key(id);
        if (auto existing = lookup_.find(composed); existing != lookup_.end())
            return existing->second;
        const auto next = IdentifierId::of(static_cast<IdentifierId::Value>(entries_.size()));
        // Node-based map: the key string keeps its address after insertion, so
        // the canonical Identifier below can view straight into it.
        auto [slot, inserted] = lookup_.emplace(std::move(composed), next);
        entries_.push_back(split(slot->first));
        return next;
    }

    // Looks the id up without interning it; returns an invalid id when the
    // Identifier was never registered.
    [[nodiscard]] IdentifierId find(const Identifier& id) const {
        if (auto existing = lookup_.find(key(id)); existing != lookup_.end())
            return existing->second;
        return IdentifierId::invalid();
    }

    // The canonical Identifier for an interned id, viewing the interner's own
    // stable storage. Aborts on an id this interner never handed out.
    [[nodiscard]] const Identifier& identifier(IdentifierId id) const {
        return entries_.at(id.index());
    }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

  private:
    static std::string key(const Identifier& id) {
        std::string composed{id.space};
        composed.push_back(':');
        composed.append(id.path);
        return composed;
    }

    // Turns a stored `space:path` back into two views into that same string.
    static Identifier split(std::string_view stored) {
        const std::size_t separator = stored.find(':');
        // key() always writes the colon, so this branch is defensive only.
        if (separator == std::string_view::npos) return Identifier{std::string_view{}, stored};
        return Identifier{stored.substr(0, separator), stored.substr(separator + 1U)};
    }

    std::unordered_map<std::string, IdentifierId> lookup_;
    std::vector<Identifier> entries_; // indexed by IdentifierId::index()
};

} // namespace mc::core
