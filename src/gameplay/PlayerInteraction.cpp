#include "gameplay/PlayerInteraction.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/ItemUse.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockState.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"
#include "world/WorldMutationService.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace mc::gameplay {

namespace {

// Whether the click would open a container rather than use the held item.
bool isContainerBlock(world::World& world, const glm::ivec3& pos) {
    const auto block = world.block(pos.x, pos.y, pos.z);
    return block == world::Block::CraftingTable || block == world::Block::Furnace ||
           block == world::Block::Chest;
}

// The open-container context the slot commands resolve their storages against,
// rebuilt from the session's container state for each command.
gameplay::ScreenContext buildScreenContext(GameSession& session) {
    const auto& furnace = session.openFurnace();
    return {session.openContainerScreen(), session.openChest(),
            furnace.has_value() ? gameplay::FurnacePosition{furnace->x, furnace->y, furnace->z}
                                : gameplay::FurnacePosition{},
            session.gameMode(), /*creativeInventoryTab*/ true};
}

// Carrot and potato are both food and seed: right-clicking farmland with one
// plants the crop, and the plant wins over eating.
bool aimsAtPlantableFarmland(world::World& world, const UseItemOn& use,
                             const ItemStack& selectedStack) {
    const auto* held = selectedStack.item;
    if (held == nullptr || cropForSeedItem(held) == world::Block::Air) {
        return false;
    }
    const glm::ivec3 below{use.adjacent.x, use.adjacent.y - 1, use.adjacent.z};
    return world::isFarmland(world.block(below.x, below.y, below.z));
}

} // namespace

void PlayerInteraction::tick(GameSession& session, world::World& world, SimulationHost& host,
                             std::vector<GameCommand> commands) {
    // `host` is retained in the public signature for headless compatibility;
    // presentation effects now cross the thread boundary exclusively through
    // the session event queue.
    static_cast<void>(host);
    // Consume the queued inputs in order.
    for (const auto& command : commands) {
        std::visit(
            [&](const auto& specific) {
                using T = std::decay_t<decltype(specific)>;
                if constexpr (std::is_same_v<T, PlayerAction>) {
                    handleDestroyCommand(session, world, host, specific);
                } else if constexpr (std::is_same_v<T, UseItemOn>) {
                    using_ = true;
                    latestUse_ = specific;
                } else if constexpr (std::is_same_v<T, UseItem>) {
                    using_ = true;
                    latestUse_.reset();
                } else if constexpr (std::is_same_v<T, UseItemStop>) {
                    using_ = false;
                } else if constexpr (std::is_same_v<T, ClickSlot>) {
                    // A container/inventory slot click executes on the server
                    // tick, resolved against the open container (26.1's
                    // AbstractContainerMenu) and routed by ScreenHandler.
                    const auto& click = specific;
                    const gameplay::ScreenContext context = buildScreenContext(session);
                    gameplay::SlotView slot;
                    slot.kind = click.kind;
                    slot.index = click.slotIndex;
                    slot.storage = gameplay::ScreenHandler::resolveSlotStorage(
                        session, context, click.kind, click.slotIndex);
                    gameplay::ScreenHandler::click(
                        session, context, slot,
                        static_cast<gameplay::InventoryMouseButton>(click.button),
                        click.shiftHeld);
                } else if constexpr (std::is_same_v<T, ClickCreativeItem>) {
                    session.inventory().clickCreativeItem(
                        specific.catalogStack, specific.button, specific.shiftHeld);
                } else if constexpr (std::is_same_v<T, ClearCursor>) {
                    session.inventory().clearCursorStack();
                } else if constexpr (std::is_same_v<T, DropCursor>) {
                    session.dropCursorStack(specific.lookDirection);
                } else if constexpr (std::is_same_v<T, DropSelected>) {
                    session.dropSelectedStack(specific.wholeStack, specific.lookDirection);
                } else if constexpr (std::is_same_v<T, DragDistribute>) {
                    // QUICK_CRAFT: resolve each swept slot to its storage against
                    // the open container, then let the inventory distribute the
                    // cursor stack across them.
                    std::vector<ItemStack*> targets;
                    targets.reserve(specific.targets.size());
                    const gameplay::ScreenContext context = buildScreenContext(session);
                    for (const auto& ref : specific.targets) {
                        if (auto* storage = gameplay::ScreenHandler::resolveSlotStorage(
                                session, context, ref.kind, ref.index);
                            storage != nullptr) {
                            targets.push_back(storage);
                        }
                    }
                    session.inventory().dragDistribute(targets, specific.button);
                } else if constexpr (std::is_same_v<T, PickupAll>) {
                    // PICKUP_ALL: the double-click gather over every matching
                    // slot in the screen, resolved the same way.
                    std::vector<ItemStack*> sources;
                    sources.reserve(specific.targets.size());
                    const gameplay::ScreenContext context = buildScreenContext(session);
                    for (const auto& ref : specific.targets) {
                        if (auto* storage = gameplay::ScreenHandler::resolveSlotStorage(
                                session, context, ref.kind, ref.index);
                            storage != nullptr) {
                            sources.push_back(storage);
                        }
                    }
                    session.inventory().gatherAllIntoCursor(sources);
                } else if constexpr (std::is_same_v<T, SwapSlot>) {
                    // Hotbar selection is authoritative gameplay state.
                    session.inventory().selectHotbar(specific.index);
                }
                // ChatCommand belongs to its own subsystem and is consumed there,
                // not here.
            },
            command);
    }

    // The eat decision, from the held use state and the current hand: a held
    // use with food in hand starts (or keeps) the vanilla 32-tick meal,
    // independently of the 4-tick rightClickDelay; attacking cancels it.
    const auto& selectedStack = session.inventory().selectedStack();
    const bool foodInHand = isFood(selectedStack.item);
    const bool targetedContainer =
        latestUse_.has_value() && isContainerBlock(world, latestUse_->block);
    const bool plantable =
        latestUse_.has_value() && aimsAtPlantableFarmland(world, *latestUse_, selectedStack);
    if (using_ && foodInHand && !targetedContainer && !plantable && !session.eating()) {
        session.beginEating(kPrimaryPlayerId, selectedStack.item, host);
    } else if (session.eating() &&
               (!using_ || !foodInHand || selectedStack.item != session.eatingKind() ||
                targetedContainer)) {
        session.cancelEating(kPrimaryPlayerId, host);
    }
    if (destroying_ && session.eating()) {
        session.cancelEating(kPrimaryPlayerId, host);
    }

    // The continuous dig, once per tick while the attack is held.
    if (destroying_ && destroyTarget_.has_value()) {
        continueDig(session, world);
    }

    // The repeated use, once per tick while held (vanilla's 4-tick
    // rightClickDelay lives here now, not in the renderer).
    if (using_ && latestUse_.has_value() && session.serverTick() >= nextUseTick_ &&
        !session.eating()) {
        performUse(session, world, *latestUse_);
        nextUseTick_ = session.serverTick() + 4U;
    }
}

void PlayerInteraction::handleDestroyCommand(GameSession& session, world::World& world,
                                             SimulationHost& host, const PlayerAction& action) {
    // The destroy decision never touches the world directly; the swing, the
    // entity hit and the dig all live on the session or the host.
    static_cast<void>(world);
    switch (action.kind) {
    case PlayerAction::Kind::StartDestroy:
        if (action.entity) {
            // Minecraft#doAttack: one click, one hit on the creature the ray
            // reached. Player#getAttackDamage: one point bare-handed, otherwise
            // the tool's own attack damage. Creative deals the same damage; only
            // the durability and exhaustion side effects stay survival-only.
            const auto& weapon = session.inventory().selectedStack();
            const auto attributes = toolAttributes(toolType(weapon), toolTier(weapon));
            const float damage =
                toolType(weapon) == ToolType::None ? 1.0F : attributes.attackDamage;
            const glm::vec3 eye = session.player().eyePosition();
            session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Break, 6U);
            if (session.worldEntities().hurt(action.entityId, damage, eye)) {
                if (session.gameMode() == GameMode::Survival) {
                    session.vitals().addExhaustion(0.1F);
                    if (session.damageHeldTool(kPrimaryPlayerId, ToolUse::AttackEntity, 0.0F)) {
                        session.events().publish(
                            SoundEvent{SoundEventKind::ItemBreak, eye});
                    }
                }
            }
            if (session.eating()) {
                session.cancelEating(kPrimaryPlayerId, host);
            }
            return;
        }
        // A block (or an empty swing) starts the dig. Between Start and Abort
        // the tick continues it; a new Start with a moved crosshair re-arms it.
        destroying_ = true;
        if (action.block.has_value()) {
            destroyTarget_ = *action.block;
            miningStartedTick_ = session.serverTick();
            lastMiningSoundTick_ = -1;
        } else {
            destroyTarget_.reset();
        }
        session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Break, 6U);
        if (session.eating()) {
            session.cancelEating(kPrimaryPlayerId, host);
        }
        return;
    case PlayerAction::Kind::AbortDestroy:
    case PlayerAction::Kind::StopDestroy:
        destroying_ = false;
        destroyTarget_.reset();
        lastMiningSoundTick_ = -1;
        return;
    }
}

void PlayerInteraction::continueDig(GameSession& session, world::World& world) {
    const glm::ivec3 blockPos = *destroyTarget_;
    // The swing repeats once per tick while the dig continues; PlayerActionState
    // only restarts the arc past halfway, which is the vanilla cadence.
    session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Break, 6U);
    if (session.gameMode() == GameMode::Creative) {
        if (session.serverTick() >= nextCreativeBreakTick_) {
            applyBreak(session, world, blockPos);
            nextCreativeBreakTick_ = session.serverTick() + 5U;
        }
        return;
    }
    // Minecraft#continueAttack accumulates destroy progress per tick, so the dig
    // lands after a whole number of ticks and a zero-hardness block is gone on
    // the very tick it starts.
    const auto target = world.block(blockPos.x, blockPos.y, blockPos.z);
    const float duration = miningSeconds(target, session.inventory().selectedStack(),
                                         session.player().inWater(),
                                         !session.player().onGround());
    const auto durationTicks = static_cast<std::uint64_t>(std::ceil(
        static_cast<double>(duration) * world::DayNightCycle::kTicksPerSecond));
    const bool done = session.serverTick() - miningStartedTick_ >= durationTicks;
    // The hit sound repeats every four ticks, the same cadence the frame timer
    // used, now counted in ticks.
    if (!done && (lastMiningSoundTick_ < 0 ||
                  static_cast<std::int64_t>(session.serverTick()) - lastMiningSoundTick_ >= 4)) {
        session.events().publish(SoundEvent{SoundEventKind::BlockHit,
                                            glm::vec3{blockPos} + glm::vec3{0.5F}, target});
        lastMiningSoundTick_ = static_cast<std::int64_t>(session.serverTick());
    }
    if (done) {
        applyBreak(session, world, blockPos);
        miningStartedTick_ = session.serverTick();
        lastMiningSoundTick_ = -1;
    }
}

void PlayerInteraction::applyBreak(GameSession& session, world::World& world,
                                   const glm::ivec3& block) {
    const auto brokenBlock = world.block(block.x, block.y, block.z);
    // Creative breaks nothing loose: the drop is vetoed with the flag rather
    // than by skipping the service, so the two modes still take the identical
    // mutation path.
    const bool survival = session.gameMode() == GameMode::Survival;
    const world::MutationFlags breakFlags =
        survival ? world::MutationFlags::All
                 : (world::MutationFlags::All | world::MutationFlags::SuppressDrops);
    GameplayMutationSink sink{world, session};
    sink.setDropTool(session.inventory().selectedStack());
    if (!world::isFluid(brokenBlock) &&
        (session.gameMode() == GameMode::Creative ||
         world::blockDefinition(brokenBlock).hardness >= 0.0F) &&
        session.worldMutations()
            .setBlock(world, {block.x, block.y, block.z}, world::BlockState{},
                      breakFlags, world::MutationCause::PlayerBreak, sink)
            .changed) {
        session.events().publish(SoundEvent{SoundEventKind::BlockBreak,
                                            glm::vec3{block} + glm::vec3{0.5F}, brokenBlock});
        session.events().publish(ParticleEvent{ParticleEventKind::BlockBreak,
                                               glm::vec3{block}, brokenBlock});
        if (survival) {
            // Player#destroyBlock adds a flat exhaustion per broken block.
            session.vitals().addExhaustion(0.005F);
            if (session.damageHeldTool(kPrimaryPlayerId, ToolUse::BreakBlock,
                                       world::blockDefinition(brokenBlock).hardness)) {
                session.events().publish(
                    SoundEvent{SoundEventKind::ItemBreak, session.player().eyePosition()});
            }
        }
        miningStartedTick_ = session.serverTick();
        lastMiningSoundTick_ = -1;
        nextCreativeBreakTick_ = session.serverTick() + 5U;
    }
}

void PlayerInteraction::performUse(GameSession& session, world::World& world,
                                   const UseItemOn& use) {
    if (session.eating()) {
        return;
    }
    const auto interactedBlock = world.block(use.block.x, use.block.y, use.block.z);
    const glm::ivec3 placeTarget = use.adjacent;
    // The creative protection is centralised in the game mode: save the held
    // stack, run the item, restore it — the reason the empty bucket stopped
    // being replaced.
    const bool infiniteMaterials = restoresHeldStack(session.gameMode());
    const auto preservedStack = session.inventory().selectedStack();
    // ServerPlayerGameMode#useItemOn's first decision: sneaking with an item in
    // hand builds against the block, an untouched container opens.
    const auto decision = decideBlockInteraction(
        world::blockDefinition(interactedBlock).container, session.player().sneaking(),
        !session.inventory().selectedStack().empty());
    switch (decision.interaction) {
    case BlockInteraction::OpenCraftingTable:
        session.openContainer(ContainerScreen::CraftingTable);
        session.events().publish(ClientActionEvent{ClientActionEventKind::OpenContainer,
                                                   ContainerScreen::CraftingTable});
        break;
    case BlockInteraction::OpenFurnace:
        // A furnace placed before block entities existed (or loaded from an
        // older save) has no entity yet; opening it is where we notice and
        // back-fill one so it can hold items and smelt.
        static_cast<void>(session.furnaceSystem().findOrCreate(
            {use.block.x, use.block.y, use.block.z}));
        session.openContainer(ContainerScreen::Furnace, std::nullopt, use.block);
        session.events().publish(ClientActionEvent{ClientActionEventKind::OpenContainer,
                                                   ContainerScreen::Furnace, use.block, true});
        break;
    case BlockInteraction::OpenChest:
        if (session.openChestContainer(
                ChestPosition{use.block.x, use.block.y, use.block.z})) {
            session.events().publish(ClientActionEvent{ClientActionEventKind::OpenContainer,
                                                       ContainerScreen::Chest, use.block, true});
        }
        break;
    case BlockInteraction::UseItem: {
        // Item#useOn: the held item decides what right-clicking does, resolved
        // by its own class; the side effects (world edit, audio, animation) are
        // applied below from the answer.
        const world::PlacementContext placement{use.block, placeTarget, use.face,
                                                use.lookDirection};
        const auto& selectedStack = session.inventory().selectedStack();
        const ItemUseResult itemUse = selectedStack.item != nullptr
                                          ? itemUseOn(selectedStack.item, world, placement)
                                          : legacyBlockStackUseOn(selectedStack, world, placement);
        switch (itemUse.action) {
        case ItemUseAction::Nothing:
            break;
        case ItemUseAction::CollectWater: {
            const auto block = use.block;
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, world::BlockState{},
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::Splash,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Air, nullptr, 0.5F});
                session.events().publish(ParticleEvent{
                    ParticleEventKind::WaterSplash,
                    glm::vec3{block} + glm::vec3{0.5F, 0.7F, 0.5F}});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                // The empty bucket becomes a full water bucket in hand.
                session.inventory().replaceSelected({world::Block::Air, 1U, &items::WaterBucket});
            }
            break;
        }
        case ItemUseAction::PlaceWater: {
            const auto block = placeTarget;
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z},
                              world::BlockState{world::Block::Water,
                                                world::defaultOrientation(world::Block::Water),
                                                0U},
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::Splash,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Air, nullptr, 1.0F});
                session.events().publish(ParticleEvent{
                    ParticleEventKind::WaterSplash,
                    glm::vec3{block} + glm::vec3{0.5F, 1.0F, 0.5F}});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                // BucketItem#getEmptiedStack: survival reverts the full bucket to
                // an empty one; creative keeps pouring without spending it.
                if (session.gameMode() == GameMode::Survival) {
                    session.inventory().replaceSelected({world::Block::Air, 1U, &items::Bucket});
                }
            }
            break;
        }
        case ItemUseAction::CollectLava: {
            const auto block = use.block;
            GameplayMutationSink sink{world, session};
            if (world.block(block.x, block.y, block.z) == world::Block::Lava &&
                session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, world::BlockState{},
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::BlockBreak,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Lava});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                session.inventory().replaceSelected({world::Block::Air, 1U, &items::LavaBucket});
            }
            break;
        }
        case ItemUseAction::PlaceLava: {
            const auto block = placeTarget;
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, world::BlockState{world::Block::Lava},
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Lava});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    session.inventory().replaceSelected({world::Block::Air, 1U, &items::Bucket});
                }
            }
            break;
        }
        case ItemUseAction::SpawnEntity: {
            // The egg dispatches to its species' EntityType through the stored
            // supplier. A creature whose model is not ready simply is not drawn.
            if (const auto* spawnEgg = asSpawnEgg(selectedStack.item)) {
                const auto& eggType = spawnEgg->entityType();
                const auto block = placeTarget;
                const glm::vec3 spawnPosition{static_cast<float>(block.x) + 0.5F,
                                              static_cast<float>(block.y) + 0.02F,
                                              static_cast<float>(block.z) + 0.5F};
                if (EntitySystem::canOccupy(world, spawnPosition, eggType.dimensions())) {
                    session.worldEntities().spawn(spawnPosition, eggType);
                    session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use,
                                                      6U);
                    if (session.gameMode() == GameMode::Survival) {
                        static_cast<void>(session.inventory().consumeSelected());
                    }
                }
            }
            break;
        }
        case ItemUseAction::PlaceBlock: {
            const auto block = placeTarget;
            const auto existingBlock = world.block(block.x, block.y, block.z);
            const world::Block placedBlock = itemUse.state.block();
            GameplayMutationSink sink{world, session};
            if (world::isRenderable(placedBlock) && world::isReplaceable(existingBlock) &&
                (!world::hasCollision(placedBlock) ||
                 (!session.player().intersectsBlock(block.x, block.y, block.z) &&
                  !session.worldEntities().intersectsBlock(block.x, block.y, block.z))) &&
                session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, itemUse.state,
                              world::MutationFlags::All, world::MutationCause::PlayerPlace, sink)
                    .changed) {
                // The chest's and furnace's block entities are created by the
                // sink's onBlockEntityReplaced, so placing one is no special case.
                session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    placedBlock});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    static_cast<void>(session.inventory().consumeSelected());
                }
            }
            break;
        }
        case ItemUseAction::TilGround: {
            // HoeItem#useOn converts the clicked block in place (dirt and grass
            // become farmland, coarse dirt becomes dirt again). The tool is not
            // consumed; in survival it pays one durability.
            const auto block = use.block;
            const auto existing = world.block(block.x, block.y, block.z);
            const world::Block tilled = itemUse.state.block();
            GameplayMutationSink sink{world, session};
            if (world::isRenderable(tilled) && existing != tilled &&
                session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, itemUse.state,
                              world::MutationFlags::All, world::MutationCause::PlayerPlace, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                                    glm::vec3{block} + glm::vec3{0.5F}, tilled});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    if (session.damageHeldTool(kPrimaryPlayerId, ToolUse::Till,
                                               world::blockDefinition(existing).hardness)) {
                        session.events().publish(SoundEvent{SoundEventKind::ItemBreak,
                                                            session.player().eyePosition()});
                    }
                }
            }
            break;
        }
        }
        break;
    }
    }
    if (infiniteMaterials) {
        session.inventory().replaceSelected(preservedStack);
    }
}

} // namespace mc::gameplay
