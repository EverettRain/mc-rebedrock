#pragma once

// Session-level client intents (stage C, §8.2 / D0): actions a player takes that
// change their session state rather than the world in front of them — respawning
// from the death screen, switching game mode. Before the client/server split the
// renderer called GameSession::respawn()/setGameMode() straight, which a
// cross-process client (no session of its own) cannot; these carry the intent
// over the channel so the server applies it authoritatively.
//
// They are kept out of the GameCommand variant on purpose: a GameCommand's frame
// tag is its variant index (0..12 today), and the snapshot/event/entity codecs
// hardcode tags right after it (13, 14, ...). Growing GameCommand would collide
// with those. So SessionCommand is its own message category with its own frame
// tag range, exactly as MovementInput and the entity snapshot are — one wire, one
// header, distinct tags.

#include "gameplay/GameMode.hpp"

#include <cstdint>
#include <variant>

namespace mc::gameplay {

// The death-screen respawn button: put the player back at its spawn. No payload —
// which player is the connection's, resolved server-side (kPrimaryPlayerId today).
struct Respawn final {
    [[nodiscard]] friend bool operator==(const Respawn&, const Respawn&) = default;
};

// A game-mode switch (creative/survival/…). The server applies it authoritatively
// and re-derives the movement gates (flight/sprint) from it on the next input.
struct SetGameMode final {
    GameMode mode = GameMode::Survival;
    [[nodiscard]] friend bool operator==(const SetGameMode&, const SetGameMode&) = default;
};

// One session intent, discriminated. Its frame tag is the variant index plus a
// base offset (see kSessionCommandTagBase in the codec) so it never overlaps the
// command/snapshot/event/entity/movement tags.
using SessionCommand = std::variant<Respawn, SetGameMode>;

}  // namespace mc::gameplay
