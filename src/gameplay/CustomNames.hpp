#pragma once

// I-3: custom item/entity names — the storage decision behind the anvil's
// rename box (and, later, the name tag).
//
// The representation is split deliberately, and the two halves answer different
// questions:
//
//   * IN MEMORY a name is a `CustomNameId` — a u16 index into this session's
//     intern table. `ItemStack` has exactly four bytes of tail padding
//     (measured: sizeof stays 48 with the id, and would have been 80 with a
//     32-byte inline array that still could not hold vanilla's 50-character
//     limit), so a named stack costs the same as an unnamed one, everywhere:
//     every inventory, every chest, every item entity.
//
//   * ON DISK AND ON THE WIRE a name is the string itself, written behind a
//     one-byte "has a name" flag on the same sparse tail the enchantments ride.
//     Serialising the id instead would have needed a palette and a remap on
//     load; serialising the string means an id is never stable across a session
//     and never has to be — loading simply interns what it reads.
//
// The table is therefore session-scoped, monotonic and never freed: ids only
// have to stay valid while the world is open, and `clear()` runs on a world
// switch. There is no garbage to collect because there is no persistent id.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::gameplay {

// 0 means "no custom name" — the value every default-constructed stack and
// entity carries, so nothing has to be initialised to make it work.
using CustomNameId = std::uint16_t;
inline constexpr CustomNameId kNoCustomName = 0U;

// AnvilScreen's `this.name.setMaxLength(50)`. Characters, not bytes — the field
// enforces it (ui::kAnvilNameFieldRules), and this is the byte ceiling that
// follows from it (a codepoint is at most four bytes in UTF-8). Anything longer
// arriving from a save, the network or a command is refused rather than
// truncated mid-codepoint.
inline constexpr std::size_t kMaximumCustomNameCharacters = 50U;
inline constexpr std::size_t kMaximumCustomNameBytes = kMaximumCustomNameCharacters * 4U;

class CustomNameTable final {
  public:
    // Returns the id for `name`, adding it if this session has not seen it.
    // An empty name is not a name: it interns to kNoCustomName, which is how
    // "clear the custom name" is expressed everywhere.
    //
    // Returns kNoCustomName (i.e. drops the name) when the string is longer
    // than kMaximumCustomNameBytes or the table is full. Both are unreachable
    // through play — the field caps input at 50 characters and the id space
    // holds 65535 distinct names per session — so the fallback exists to keep a
    // corrupt save or a hostile packet from being a crash, not as a real path.
    [[nodiscard]] CustomNameId intern(std::string_view name);

    // The name behind an id, or an empty view for kNoCustomName and for any id
    // this session does not know (a stack that outlived a world switch, say).
    // Never throws: a caller displaying a name must not have to guard.
    [[nodiscard]] std::string_view nameOf(CustomNameId id) const;

    // A world switch. Ids are session-scoped, so the table starts empty for
    // each world; anything still holding an old id reads back as unnamed.
    void clear();

    [[nodiscard]] std::size_t size() const { return names_.size(); }

  private:
    // Index 0 is never used: it is the kNoCustomName sentinel, so `names_[id]`
    // needs no offset and an id is its own index.
    std::vector<std::string> names_{std::string{}};
    // Transparent hash + equality so intern() can look up a string_view without
    // materialising a std::string on every call — interning happens on every
    // stack read from a save, so the common case (already present) must not
    // allocate.
    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view text) const {
            return std::hash<std::string_view>{}(text);
        }
    };
    std::unordered_map<std::string, CustomNameId, TransparentHash, std::equal_to<>> ids_;
};

// The session's table. A singleton in the same shape as lootTable() /
// contentRegistry() / structureManager(): one per running game, cleared when the
// world changes (GameSession::resetWorldState).
[[nodiscard]] CustomNameTable& customNames();

// Convenience for the overwhelmingly common read: the name a stack/entity shows,
// or an empty view. Kept here rather than on ItemStack because Inventory.hpp
// must not depend on this table (it is a session service, not a value type).
[[nodiscard]] inline std::string_view customNameOf(CustomNameId id) {
    return customNames().nameOf(id);
}

} // namespace mc::gameplay
