#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/ServerPlayer.hpp"  // PlayerId / kPrimaryPlayerId (the source's player identity)
#include "gameplay/command/ArgumentType.hpp"
#include "world/Dimension.hpp"  // DimensionId — the real dimension identity (DIM0)

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>

namespace mc::gameplay::command {

// The dimension a command source runs in. This is now the real dense id from the
// DIM0 registry — the earlier overworld-only placeholder proxy is gone. A source
// still defaults to the Overworld (single-player has only it loaded so far), but
// `execute in <dimension>` (CMD5) and cross-dimension commands can carry the
// Nether/End through unchanged.
using Dimension = world::DimensionId;

// The op level a command requires, mirroring 1.16.1's Commands.LEVEL_* ladder.
// A command declares the level it needs (`.requires(...)`), and a source below
// that level is refused before its handler runs — the C++ equivalent of
// Brigadier's `requires(Predicate<S>)`, specialised to the one predicate that
// matters (op level) so the check is an integer compare, not a stored closure.
enum class PermissionLevel : std::uint8_t {
    All = 0,          // everyone: chat, help, client-only conveniences
    Moderators = 1,   // bypass spawn protection (LEVEL_MODERATORS)
    GameMasters = 2,   // gameplay commands: gamemode, gamerule, kill, give, time…
    Admins = 3,       // op/deop, world config (LEVEL_ADMINS)
    Owners = 4,       // ban/stop and the single-player host (LEVEL_OWNERS)
};

// True when a source holding `held` may run a command needing `required`.
[[nodiscard]] constexpr bool hasPermission(PermissionLevel held, PermissionLevel required) {
    return static_cast<std::uint8_t>(held) >= static_cast<std::uint8_t>(required);
}

// The higher of two levels — used to fold a command path's requirement (a child
// inherits its ancestors' declared level).
[[nodiscard]] constexpr PermissionLevel maxLevel(PermissionLevel left, PermissionLevel right) {
    return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
}

// Where a command's feedback goes, handed the executed result. Value-carried in
// the source so `execute as/at` (CMD5) inherits it by copy. Single-player points
// it at the chat HUD; a dedicated server broadcasts to ops. The gamerule gate
// (sendCommandFeedback) lives inside the concrete sink the runtime installs, not
// here — the source stays a dumb carrier. Null means "discard" (a headless
// dispatcher test that only inspects the return value).
using FeedbackSink = std::function<void(const CommandResult&)>;

// The execution context a command runs against, mirroring 1.16.1's
// CommandSourceStack but as a value-semantic POD, not an object with `withXxx`
// derivations: `execute as/at/positioned` (CMD5) is a copy with one field
// changed, which suits ReBedrock's data-oriented layout. Single-player builds it
// from the primary player at op4; multiplayer (S subtree) fills playerId and
// permissionLevel from the ServerPlayer that sent the command. Relative `~`
// coordinates and (future) `@s` resolve against this — see resolve().
struct CommandSource final {
    PlayerId playerId = kPrimaryPlayerId;
    glm::vec3 position{0.0F};
    Rotation2 rotation{};
    Dimension dimension = Dimension::Overworld;
    PermissionLevel permissionLevel = PermissionLevel::Owners;
    FeedbackSink feedback;  // null: feedback is discarded (return value only)
    // The executor: a player (default) or a world entity. `execute as <entity>`
    // (CMD5) rebinds it to a mob, so `@s` inside `run` resolves to that mob, not
    // the player. entityId is meaningful only while executorIsEntity is true.
    bool executorIsEntity = false;
    std::uint64_t entityId = 0;

    // The 1.16.1 CommandSourceStack.withXxx derivations, as value copies — the
    // whole point of the POD source: `execute as/at/positioned/rotated/in` is a
    // copy with one field changed, no object tree. `as` rebinds only the
    // executor (position stays, matching vanilla); `at` moves the position.
    [[nodiscard]] CommandSource withExecutorPlayer(PlayerId id) const {
        CommandSource copy = *this;
        copy.executorIsEntity = false;
        copy.playerId = id;
        return copy;
    }
    [[nodiscard]] CommandSource withExecutorEntity(std::uint64_t id) const {
        CommandSource copy = *this;
        copy.executorIsEntity = true;
        copy.entityId = id;
        return copy;
    }
    [[nodiscard]] CommandSource withPosition(glm::vec3 newPosition) const {
        CommandSource copy = *this;
        copy.position = newPosition;
        return copy;
    }
    [[nodiscard]] CommandSource withRotation(Rotation2 newRotation) const {
        CommandSource copy = *this;
        copy.rotation = newRotation;
        return copy;
    }
    [[nodiscard]] CommandSource withDimension(Dimension newDimension) const {
        CommandSource copy = *this;
        copy.dimension = newDimension;
        return copy;
    }
};

// The single place a `~`-relative Position3 becomes a concrete world position:
// each relative axis is the source's position plus the offset, each absolute
// axis the literal value. Every command that reads a Vec3 argument resolves it
// here instead of hand-writing the branch, so a coordinate never means two
// different things in two commands (1.16.1's Vec3Argument#getPosition against
// the source, collapsed to one function).
[[nodiscard]] inline glm::vec3 resolve(const Position3& position, const CommandSource& source) {
    return {
        position.relativeX ? source.position.x + static_cast<float>(position.x)
                           : static_cast<float>(position.x),
        position.relativeY ? source.position.y + static_cast<float>(position.y)
                           : static_cast<float>(position.y),
        position.relativeZ ? source.position.z + static_cast<float>(position.z)
                           : static_cast<float>(position.z),
    };
}

} // namespace mc::gameplay::command
