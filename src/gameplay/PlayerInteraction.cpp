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
#include "world/BlockShape.hpp"
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
// The block a held stack would place, or Air for a non-block item. A legacy
// block stack carries the block directly with a null item pointer.
[[nodiscard]] world::Block heldPlacementBlock(const ItemStack& stack) {
    if (const auto* blockItem = asBlockItem(stack.item)) {
        return blockItem->block();
    }
    return stack.item == nullptr ? stack.block : world::Block::Air;
}

// F2's "破坏留水" rule: what a cell becomes after its block is removed. A
// submerged block leaves the water source it was carrying behind rather than
// air (SimpleWaterloggedBlock's implicit behaviour: `updateShape`/removal
// never touches the fluid layer, so the water that was already "in" the cell
// is simply what remains once the solid shape is gone) — sabotage #2's target
// is exactly this function returning plain Air instead.
[[nodiscard]] world::BlockState breakResidue(world::BlockState broken) {
    if (broken.submergedFluid() == world::SubmergedFluid::Water) {
        return world::BlockState{world::Block::Water,
                                 world::defaultOrientation(world::Block::Water), 0U};
    }
    return world::BlockState{};
}

// SlabBlock#canBeReplaced: right-clicking an existing single slab with the same
// slab merges the two into a double. Without a sub-cell hit fraction the gesture
// is read from the clicked face — completing a bottom slab from above or a top
// slab from below — plus the case where the placement cell already holds the
// same single slab. Returns the cell to rewrite and the double state, or nothing
// when no merge applies (the caller then runs ordinary placement).
[[nodiscard]] std::optional<std::pair<glm::ivec3, world::BlockState>> slabMergeTarget(
    world::World& world, world::Block held, const UseItemOn& use) {
    if (!world::isSlab(held)) {
        return std::nullopt;
    }
    const auto asDouble = [&](glm::ivec3 cell) {
        return std::pair{cell, world::BlockState{held}.withSlabPortion(
                                   world::SlabPortion::Double)};
    };
    // The clicked cell: complete the slab from its open side. SlabBlock#
    // canBeReplaced completes a bottom slab from its top face or the upper half
    // of a side, and a top slab from its bottom face or the lower half of a side.
    const auto clicked = world.state(use.block.x, use.block.y, use.block.z);
    if (clicked.block() == held) {
        const auto portion = clicked.slabPortion();
        const bool above = use.hitPosition.y - static_cast<float>(use.block.y) > 0.5F;
        const bool horizontal = world::isHorizontal(use.face);
        if ((portion == world::SlabPortion::Bottom &&
             (use.face == world::BlockOrientation::Up || (above && horizontal))) ||
            (portion == world::SlabPortion::Top &&
             (use.face == world::BlockOrientation::Down || (!above && horizontal)))) {
            return asDouble(use.block);
        }
    }
    // The placement cell already holds the matching single slab (stacking a slab
    // into a cell whose complementary half is filled).
    const auto target = world.state(use.adjacent.x, use.adjacent.y, use.adjacent.z);
    if (target.block() == held && target.slabPortion() != world::SlabPortion::Double) {
        return asDouble(use.adjacent);
    }
    return std::nullopt;
}

bool aimsAtPlantableFarmland(world::World& world, const UseItemOn& use,
                             const ItemStack& selectedStack) {
    const auto* held = selectedStack.item;
    if (held == nullptr || cropForSeedItem(held) == world::Block::Air) {
        return false;
    }
    const glm::ivec3 below{use.adjacent.x, use.adjacent.y - 1, use.adjacent.z};
    return world::isFarmland(world.block(below.x, below.y, below.z));
}

// AR-B2: DoorBlock#useWithoutItem / FenceGateBlock#useWithoutItem — a plain
// right-click on the block itself (not suppressed by sneaking-with-item-in-
// hand, checked by the caller) flips OPEN. A door writes both halves so they
// never observe different OPEN values; a gate also snaps its FACING to face
// the player when opening from closed (FenceGateBlock#useWithoutItem: "if
// closed and the player faces the opposite way, turn to face them first"),
// which is why an untouched gate always swings open toward whoever opens it.
// Returns whether a toggle actually happened, so the caller knows to swing/
// play a sound and skip every other interaction this click could have been.
[[nodiscard]] bool toggleDoorOrGate(GameSession& session, world::World& world,
                                    glm::ivec3 clicked, world::BlockOrientation playerFacing) {
    const auto clickedState = world.state(clicked.x, clicked.y, clicked.z);
    const auto model = world::blockDefinition(clickedState.block()).model;
    if (model == world::BlockModel::Door) {
        const bool upper = clickedState.isDoorUpperHalf();
        const glm::ivec3 lower{clicked.x, clicked.y - (upper ? 1 : 0), clicked.z};
        const glm::ivec3 upperCell{lower.x, lower.y + 1, lower.z};
        const auto lowerState = world.state(lower.x, lower.y, lower.z);
        const bool open = !lowerState.open();
        GameplayMutationSink sink{world, session};
        session.worldMutations().setBlock(world, {lower.x, lower.y, lower.z},
                                          lowerState.withOpen(open), world::MutationFlags::All,
                                          world::MutationCause::PlayerPlace, sink);
        const auto upperState = world.state(upperCell.x, upperCell.y, upperCell.z);
        if (upperState.block() == clickedState.block()) {
            session.worldMutations().setBlock(world, {upperCell.x, upperCell.y, upperCell.z},
                                              upperState.withOpen(open),
                                              world::MutationFlags::All,
                                              world::MutationCause::PlayerPlace, sink);
        }
        session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                            glm::vec3{lower} + glm::vec3{0.5F, 1.0F, 0.5F},
                                            clickedState.block()});
        return true;
    }
    if (model == world::BlockModel::FenceGate) {
        GameplayMutationSink sink{world, session};
        world::BlockState next = clickedState;
        if (clickedState.open()) {
            next = next.withOpen(false);
        } else {
            // FenceGateBlock#useWithoutItem: only re-faces when the gate's
            // current facing is exactly opposite the player's — an already
            // aligned or perpendicular gate keeps its facing.
            if (clickedState.orientation() == world::oppositeOrientation(playerFacing)) {
                next = next.with(playerFacing);
            }
            next = next.withOpen(true);
        }
        session.worldMutations().setBlock(world, {clicked.x, clicked.y, clicked.z}, next,
                                          world::MutationFlags::All,
                                          world::MutationCause::PlayerPlace, sink);
        session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                            glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block()});
        return true;
    }
    // AR-B3: TrapDoorBlock#useWithoutItem — a plain right-click flips OPEN,
    // one cell, no atomic partner to keep in sync (unlike the door above).
    if (model == world::BlockModel::TrapDoor) {
        GameplayMutationSink sink{world, session};
        session.worldMutations().setBlock(world, {clicked.x, clicked.y, clicked.z},
                                          clickedState.withOpen(!clickedState.open()),
                                          world::MutationFlags::All,
                                          world::MutationCause::PlayerPlace, sink);
        session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                            glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block()});
        return true;
    }
    return false;
}

// AR-B3: ButtonBlock#useWithoutItem — press: POWERED true immediately, and
// the release is left to the existing WorldSimulation::scheduleButtonRelease
// / dispatchRedstoneTick(StoneButton) timer (wired in the W-4/5 redstone
// slice, never previously reachable because nothing called this). Re-pressing
// an already-pressed button is a no-op (ButtonBlock#useWithoutItem returns
// CONSUME without re-scheduling when already POWERED), matching vanilla's
// "does not restart the timer" behaviour. Returns whether a press actually
// happened, the same reporting shape toggleDoorOrGate uses.
[[nodiscard]] bool pressButton(GameSession& session, world::World& world, glm::ivec3 clicked) {
    const auto clickedState = world.state(clicked.x, clicked.y, clicked.z);
    if (world::blockDefinition(clickedState.block()).model != world::BlockModel::Button) {
        return false;
    }
    if (clickedState.powered()) {
        return false; // already pressed: no re-trigger, matching vanilla
    }
    GameplayMutationSink sink{world, session};
    session.worldMutations().setBlock(world, {clicked.x, clicked.y, clicked.z},
                                      clickedState.withPowered(true), world::MutationFlags::All,
                                      world::MutationCause::PlayerPlace, sink);
    session.worldSimulation().scheduleButtonRelease({clicked.x, clicked.y, clicked.z});
    session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                        glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block()});
    return true;
}

} // namespace

// AR-B3: BasePressurePlateBlock's tick/entityInside collapsed into one
// per-tick pass over a bounded set of feet positions (the player's own, then
// every live creature's) — see the header comment for why this is bounded
// rather than a full loaded-chunk scan. `pressedPlates` is the *caller's*
// previous-tick set of plate cells found occupied (owned by GameSession, not
// module state — a free-standing mutable global would break test isolation
// and multi-session determinism) — a plain re-derive-from-scratch-and-diff
// each tick, the same "world/feet positions are the source of truth, no
// per-plate scheduled entry" approach wallConnectionsFor/stairShapeFor
// already take for their own neighbour state: the *new* covered set is
// computed fresh from this tick's feet positions, then diffed against the
// previous set — newly covered cells power on, cells that dropped out power
// off, and the caller's set is replaced with the new one for next tick's diff.
void tickPressurePlates(GameSession& session, world::World& world, glm::vec3 playerFeet,
                        std::span<const glm::vec3> creatureFeet,
                        std::vector<glm::ivec3>& pressedPlates) {
    const auto cellUnder = [](glm::vec3 feet) {
        return glm::ivec3{static_cast<int>(std::floor(feet.x)),
                          static_cast<int>(std::floor(feet.y - 0.05F)), // just under the feet
                          static_cast<int>(std::floor(feet.z))};
    };
    std::vector<glm::ivec3> covered;
    const auto addIfPlate = [&](glm::ivec3 cell) {
        const auto state = world.state(cell.x, cell.y, cell.z);
        if (world::blockDefinition(state.block()).model == world::BlockModel::PressurePlate &&
            std::find(covered.begin(), covered.end(), cell) == covered.end()) {
            covered.push_back(cell);
        }
    };
    addIfPlate(cellUnder(playerFeet));
    for (const glm::vec3 feet : creatureFeet) {
        addIfPlate(cellUnder(feet));
    }
    const auto setPowered = [&](glm::ivec3 cell, bool powered) {
        const auto state = world.state(cell.x, cell.y, cell.z);
        if (state.powered() == powered) {
            return;
        }
        GameplayMutationSink sink{world, session};
        session.worldMutations().setBlock(world, {cell.x, cell.y, cell.z}, state.withPowered(powered),
                                          world::MutationFlags::All,
                                          world::MutationCause::PlayerPlace, sink);
    };
    for (const auto& cell : covered) {
        setPowered(cell, true);
    }
    for (const auto& cell : pressedPlates) {
        if (std::find(covered.begin(), covered.end(), cell) == covered.end()) {
            setPowered(cell, false);
        }
    }
    pressedPlates = std::move(covered);
}

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
    const auto brokenState = world.state(block.x, block.y, block.z);
    const auto brokenBlock = brokenState.block();
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
            .setBlock(world, {block.x, block.y, block.z}, breakResidue(brokenState),
                      breakFlags, world::MutationCause::PlayerBreak, sink)
            .changed) {
        session.events().publish(SoundEvent{SoundEventKind::BlockBreak,
                                            glm::vec3{block} + glm::vec3{0.5F}, brokenBlock});
        session.events().publish(ParticleEvent{ParticleEventKind::BlockBreak,
                                               glm::vec3{block}, brokenBlock});
        // AR-B2: DoorBlock#playerWillDestroy — breaking either half removes
        // both. The other half's own drops are suppressed (DoublePlantBlock's
        // "prevent drop from bottom part" equivalent: vanilla only rolls the
        // clicked half's loot table, never double-counting the item), so this
        // second write goes through with SuppressDrops regardless of survival.
        if (world::blockDefinition(brokenBlock).model == world::BlockModel::Door) {
            const bool wasUpper = brokenState.isDoorUpperHalf();
            const glm::ivec3 otherHalf{block.x, block.y + (wasUpper ? -1 : 1), block.z};
            const auto otherState = world.state(otherHalf.x, otherHalf.y, otherHalf.z);
            if (otherState.block() == brokenBlock && otherState.isDoorUpperHalf() != wasUpper) {
                session.worldMutations().setBlock(
                    world, {otherHalf.x, otherHalf.y, otherHalf.z}, world::BlockState{},
                    breakFlags | world::MutationFlags::SuppressDrops,
                    world::MutationCause::PlayerBreak, sink);
            }
        }
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
    // AR-B2/AR-B3: DoorBlock/FenceGateBlock/TrapDoorBlock/ButtonBlock#
    // useWithoutItem all run ahead of the container decision below (each is
    // "the block itself answers the click, no item involved"), gated by the
    // identical sneaking-with-item-in-hand suppression a container obeys —
    // sneaking with something in hand always means "build against this
    // block".
    if (!blockInteractionSuppressed(session.player().sneaking(),
                                    !session.inventory().selectedStack().empty()) &&
        (toggleDoorOrGate(session, world, use.block, world::horizontalFacing(use.lookDirection)) ||
         pressButton(session, world, use.block))) {
        session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
        return;
    }
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
                                                use.hitPosition, use.lookDirection};
        const auto& selectedStack = session.inventory().selectedStack();
        // SlabBlock merge: two single slabs of the same kind become a double,
        // rewriting the cell that already holds a slab rather than placing into
        // an empty neighbour. This is its own path because the target cell is not
        // replaceable (it is a slab), so the ordinary PlaceBlock check rejects it.
        if (const auto merge = slabMergeTarget(world, heldPlacementBlock(selectedStack), use)) {
            const auto cell = merge->first;
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {cell.x, cell.y, cell.z}, merge->second,
                              world::MutationFlags::All, world::MutationCause::PlayerPlace, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                                    glm::vec3{cell} + glm::vec3{0.5F},
                                                    merge->second.block()});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    static_cast<void>(session.inventory().consumeSelected());
                }
            }
            break;
        }
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
        case ItemUseAction::CollectSubmergedWater: {
            // BucketPickup#pickupBlock, the SubmergedFluid branch (F2): the
            // clicked block keeps its identity and shape, only its axis clears.
            // A plain `with(SubmergedFluid, None)` on the *current* state is
            // used rather than reconstructing a fresh BlockState, so a slab's
            // other axes (SlabType) ride along unchanged.
            const auto block = use.block;
            const auto current = world.state(block.x, block.y, block.z);
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z},
                              current.withSubmergedFluid(world::SubmergedFluid::None),
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::Splash,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Air, nullptr, 0.5F});
                session.events().publish(ParticleEvent{
                    ParticleEventKind::WaterSplash,
                    glm::vec3{block} + glm::vec3{0.5F, 0.7F, 0.5F}});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                session.inventory().replaceSelected({world::Block::Air, 1U, &items::WaterBucket});
            }
            break;
        }
        case ItemUseAction::SubmergeBlock: {
            // LiquidBlockContainer#placeLiquid, the SubmergedFluid branch (F2):
            // wets the clicked block in place instead of replacing it.
            const auto block = use.block;
            const auto current = world.state(block.x, block.y, block.z);
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z},
                              current.withSubmergedFluid(world::SubmergedFluid::Water),
                              world::MutationFlags::All, world::MutationCause::Fluid, sink)
                    .changed) {
                session.events().publish(SoundEvent{SoundEventKind::Splash,
                                                    glm::vec3{block} + glm::vec3{0.5F},
                                                    world::Block::Air, nullptr, 1.0F});
                session.events().publish(ParticleEvent{
                    ParticleEventKind::WaterSplash,
                    glm::vec3{block} + glm::vec3{0.5F, 1.0F, 0.5F}});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    session.inventory().replaceSelected({world::Block::Air, 1U, &items::Bucket});
                }
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
            // Occupancy is checked against the block's real box, not a full cube,
            // so a slab drops into the empty half of the cell the player (or a
            // creature) is standing in instead of being rejected as blocked.
            const auto placedSpan = world::collisionSpan(itemUse.state);
            GameplayMutationSink sink{world, session};
            if (world::isRenderable(placedBlock) && world::isReplaceable(existingBlock) &&
                (!world::hasCollision(placedBlock) ||
                 (!session.player().intersectsBlock(block.x, block.y, block.z,
                                                    placedSpan.bottom, placedSpan.top) &&
                  !session.worldEntities().intersectsBlock(block.x, block.y, block.z,
                                                           placedSpan.bottom, placedSpan.top))) &&
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
        case ItemUseAction::PlaceDoor: {
            // AR-B2: DoorBlock#setPlacedBy — the lower half (already resolved:
            // facing/hinge decided) writes first, then the upper half
            // immediately after at pos+Up, sharing every axis except Half. Both
            // cells are checked for entity occupancy the same way PlaceBlock
            // does (a door's thin box still occupies real space), and the
            // second write is what makes this "atomic" in the sense the task
            // card asks for: nothing observes a lower half with no upper half,
            // because no tick boundary falls between the two setBlock calls —
            // this function runs to completion before the session is read
            // again (single-threaded gameplay tick, no yield point here).
            const auto lower = placeTarget;
            const glm::ivec3 upperCell{lower.x, lower.y + 1, lower.z};
            const auto upperState = itemUse.state.withDoorUpperHalf(true);
            const auto lowerSpan = world::collisionSpan(itemUse.state);
            const auto upperSpan = world::collisionSpan(upperState);
            const world::Block placedBlock = itemUse.state.block();
            const bool spaceFree =
                world::isReplaceable(world.block(lower.x, lower.y, lower.z)) &&
                world::isReplaceable(world.block(upperCell.x, upperCell.y, upperCell.z)) &&
                !session.player().intersectsBlock(lower.x, lower.y, lower.z, lowerSpan.bottom,
                                                   lowerSpan.top) &&
                !session.player().intersectsBlock(upperCell.x, upperCell.y, upperCell.z,
                                                   upperSpan.bottom, upperSpan.top) &&
                !session.worldEntities().intersectsBlock(lower.x, lower.y, lower.z,
                                                          lowerSpan.bottom, lowerSpan.top) &&
                !session.worldEntities().intersectsBlock(upperCell.x, upperCell.y, upperCell.z,
                                                          upperSpan.bottom, upperSpan.top);
            if (world::isRenderable(placedBlock) && spaceFree) {
                GameplayMutationSink sink{world, session};
                const bool lowerPlaced =
                    session.worldMutations()
                        .setBlock(world, {lower.x, lower.y, lower.z}, itemUse.state,
                                  world::MutationFlags::All, world::MutationCause::PlayerPlace, sink)
                        .changed;
                if (lowerPlaced) {
                    session.worldMutations().setBlock(
                        world, {upperCell.x, upperCell.y, upperCell.z}, upperState,
                        world::MutationFlags::All, world::MutationCause::PlayerPlace, sink);
                    session.events().publish(SoundEvent{SoundEventKind::BlockPlace,
                                                        glm::vec3{lower} + glm::vec3{0.5F},
                                                        placedBlock});
                    session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use,
                                                      6U);
                    if (session.gameMode() == GameMode::Survival) {
                        static_cast<void>(session.inventory().consumeSelected());
                    }
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
