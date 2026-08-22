#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/CommandSource.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <cassert>
#include <cstddef>
#include <glm/vec3.hpp>
#include <optional>
#include <string>

using mc::gameplay::CommandResult;
using mc::gameplay::command::CommandContext;
using mc::gameplay::command::CommandDispatcher;
using mc::gameplay::command::CommandSource;
using mc::gameplay::command::Dimension;
using mc::gameplay::command::hasPermission;
using mc::gameplay::command::kIntArgument;
using mc::gameplay::command::kTeleportDestinationArgument;
using mc::gameplay::command::maxLevel;
using mc::gameplay::command::PermissionLevel;
using mc::gameplay::command::Position3;
using mc::gameplay::command::resolve;

int main() {
    // Teleport-destination arguments validate entity ids against the registry.
    mc::gameplay::entities::registerBuiltinEntities();

    // --- resolve(): the single relative-coordinate evaluation -----------------
    {
        CommandSource source;
        source.position = {10.0F, 64.0F, -5.0F};
        // Absolute axes ignore the source entirely.
        const glm::vec3 absolute = resolve(Position3{1.0, 2.0, 3.0, false, false, false}, source);
        assert(absolute.x == 1.0F && absolute.y == 2.0F && absolute.z == 3.0F);
        // Relative axes add the source's position; a mix resolves per axis.
        const glm::vec3 relative = resolve(Position3{1.0, 0.0, 3.0, true, false, true}, source);
        assert(relative.x == 11.0F && relative.y == 0.0F && relative.z == -2.0F);
        // A bare `~` (zero offset) reproduces the source's own coordinate.
        const glm::vec3 here = resolve(Position3{0.0, 0.0, 0.0, true, true, true}, source);
        assert(here == source.position);
    }

    // --- permission helpers ---------------------------------------------------
    assert(hasPermission(PermissionLevel::Owners, PermissionLevel::GameMasters));
    assert(hasPermission(PermissionLevel::GameMasters, PermissionLevel::GameMasters));
    assert(!hasPermission(PermissionLevel::Moderators, PermissionLevel::GameMasters));
    assert(maxLevel(PermissionLevel::All, PermissionLevel::Admins) == PermissionLevel::Admins);
    assert(maxLevel(PermissionLevel::Owners, PermissionLevel::All) == PermissionLevel::Owners);

    CommandDispatcher dispatcher;

    // --- source threading: a handler reads who/where/dim/op -------------------
    glm::vec3 seenPosition{0.0F};
    double seenYaw = 0.0;
    auto seenLevel = PermissionLevel::All;
    auto seenDimension = Dimension::Overworld;
    bool sawSource = false;
    dispatcher.literal("whoami")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([&](const CommandContext& context) {
            assert(context.hasSource());
            const CommandSource& source = context.source();
            seenPosition = source.position;
            seenYaw = source.rotation.yaw;
            seenLevel = source.permissionLevel;
            seenDimension = source.dimension;
            sawSource = true;
            return CommandResult{true, "ok"};
        });

    CommandSource owner;
    owner.position = {3.0F, 4.0F, 5.0F};
    owner.rotation.yaw = 90.0;
    owner.rotation.pitch = -12.0;
    owner.dimension = Dimension::Overworld;
    owner.permissionLevel = PermissionLevel::Owners;
    assert(dispatcher.execute("/whoami", owner).success);
    assert(sawSource);
    assert(seenPosition.x == 3.0F && seenPosition.y == 4.0F && seenPosition.z == 5.0F);
    assert(seenYaw == 90.0);
    assert(seenLevel == PermissionLevel::Owners);
    assert(seenDimension == Dimension::Overworld);

    // --- permission gate ------------------------------------------------------
    CommandSource op1;
    op1.permissionLevel = PermissionLevel::Moderators;
    const CommandResult denied = dispatcher.execute("/whoami", op1);
    assert(!denied.success);
    assert(denied.message.find("permission") != std::string::npos);
    CommandSource op2;
    op2.permissionLevel = PermissionLevel::GameMasters;
    assert(dispatcher.execute("/whoami", op2).success); // exactly the required level passes

    // The required level is inherited down the subtree: a deeper argument leaf is
    // gated by the level declared on its ancestor literal, not just the literal.
    dispatcher.literal("cfg")
        .requiresLevel(PermissionLevel::GameMasters)
        .then("set")
        .argument("v", kIntArgument)
        .executes([&](const CommandContext&) { return CommandResult{true, "ok"}; });
    assert(!dispatcher.execute("/cfg set 5", op1).success); // op1 refused at the leaf
    assert(dispatcher.execute("/cfg set 5", op2).success);

    // The convenience execute(input) runs as the single-player owner, so an
    // op-gated command still passes when no source is supplied.
    assert(dispatcher.execute("/whoami").success);

    // --- relative coordinates converge on one resolve() -----------------------
    // Two independent commands both resolve `~` through resolve(); given the same
    // source they must produce byte-identical targets (no per-command hand-math).
    glm::vec3 firstTarget{0.0F};
    glm::vec3 secondTarget{0.0F};
    const auto resolvePos = [](glm::vec3& out) {
        return [&out](const CommandContext& context) {
            const auto position = context.find<Position3>("pos");
            assert(position.has_value());
            out = resolve(*position, context.source());
            return CommandResult{true, "ok"};
        };
    };
    dispatcher.literal("aa")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("pos", kTeleportDestinationArgument)
        .executes(resolvePos(firstTarget));
    dispatcher.literal("bb")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("pos", kTeleportDestinationArgument)
        .executes(resolvePos(secondTarget));
    CommandSource here;
    here.position = {100.0F, 70.0F, 100.0F};
    here.permissionLevel = PermissionLevel::Owners;
    assert(dispatcher.execute("/aa ~ ~5 ~", here).success);
    assert(dispatcher.execute("/bb ~ ~5 ~", here).success);
    assert(firstTarget == secondTarget); // same source, same resolve → same target
    assert(firstTarget.x == 100.0F && firstTarget.y == 75.0F && firstTarget.z == 100.0F);
    // Absolute coordinates never depend on the source (regression guard).
    assert(dispatcher.execute("/aa 1 2 3", here).success);
    assert(firstTarget.x == 1.0F && firstTarget.y == 2.0F && firstTarget.z == 3.0F);

    // --- feedback routing, gated by the sendCommandFeedback gamerule ----------
    // The sink stands in for the runtime's HUD sink: it suppresses a *success*
    // when feedback is off, but a failure always reports.
    bool feedbackEnabled = true;
    std::optional<CommandResult> lastFeedback;
    CommandSource sink;
    sink.permissionLevel = PermissionLevel::Owners;
    sink.feedback = [&](const CommandResult& result) {
        if (result.success && !feedbackEnabled) {
            return; // sendCommandFeedback == false silences successes
        }
        lastFeedback = result;
    };

    lastFeedback.reset();
    assert(dispatcher.execute("/whoami", sink).success);
    assert(lastFeedback.has_value() && lastFeedback->success); // success delivered when on

    feedbackEnabled = false;
    lastFeedback.reset();
    assert(dispatcher.execute("/whoami", sink).success); // command still succeeds
    assert(!lastFeedback.has_value());                   // but no HUD feedback

    // A failure reports through the sink even with feedback off.
    lastFeedback.reset();
    const CommandResult failure = dispatcher.execute("/nope", sink);
    assert(!failure.success);
    assert(lastFeedback.has_value() && !lastFeedback->success);

    // A permission denial is a failure and is likewise routed to the sink.
    lastFeedback.reset();
    sink.permissionLevel = PermissionLevel::Moderators;
    const CommandResult refused = dispatcher.execute("/whoami", sink);
    assert(!refused.success);
    assert(lastFeedback.has_value() && !lastFeedback->success);

    return 0;
}
