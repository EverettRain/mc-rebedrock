#include "gameplay/PlayerInteraction.hpp"

#include "gameplay/EnchantmentCombat.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/ItemUse.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"
#include "world/WorldMutationService.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
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
    const auto& enchanting = session.openEnchantingTable();
    gameplay::ScreenContext context;
    context.screen = session.openContainerScreen();
    context.chest = session.openChest();
    context.furnace = furnace.has_value()
                          ? gameplay::FurnacePosition{furnace->x, furnace->y, furnace->z}
                          : gameplay::FurnacePosition{};
    context.enchantingTable = enchanting.value_or(glm::ivec3{0});
    context.gameMode = session.gameMode();
    context.creativeInventoryTab = true;
    return context;
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

// EQ-1: ArmorItem#use — right-clicking with an armor piece in hand swaps it
// into its matching equipment slot: the selected stack goes to the (now
// empty, or previously occupied) armor slot and whatever was worn there
// (nothing, the first time) comes back into the hand. `armorSlotOf`/canEquip
// are the same routing Item.hpp/Inventory.hpp expose to ScreenHandler's
// slot-click filter, so a right-click equip and a drag-and-drop equip can
// never disagree about which slot a given piece belongs in (sabotage③'s
// target: this must never consult anything BUT armorSlotOf). Returns whether
// a swap happened, so the caller knows to swing the arm and stop — vanilla's
// ArmorItem#use returns InteractionResultHolder.success unconditionally
// once armor is in hand, whether or not a slot was already occupied.
bool tryAutoEquipArmor(GameSession& session) {
    const ItemStack selected = session.inventory().selectedStack();
    if (!isArmor(selected.item)) {
        return false;
    }
    const EquipmentSlot target = armorSlotOf(selected.item);
    ItemStack& wornSlot = session.equipment().mutableSlot(target);
    // The swap: whatever was worn (possibly nothing) becomes the held stack,
    // the held stack becomes worn. replaceSelected is the same
    // ItemUsage#method_30012 primitive the bucket-fill path already uses for
    // "the hand's held stack becomes a different single stack in place".
    const ItemStack previouslyWorn = wornSlot;
    wornSlot = selected;
    session.inventory().replaceSelected(previouslyWorn);
    return true;
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
        session.events().publish(
            SoundEvent{open ? SoundEventKind::BlockOpen : SoundEventKind::BlockClose,
                       glm::vec3{lower} + glm::vec3{0.5F, 1.0F, 0.5F}, clickedState.block()});
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
        session.events().publish(
            SoundEvent{next.open() ? SoundEventKind::BlockOpen : SoundEventKind::BlockClose,
                       glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block()});
        return true;
    }
    // AR-B3: TrapDoorBlock#useWithoutItem — a plain right-click flips OPEN,
    // one cell, no atomic partner to keep in sync (unlike the door above).
    if (model == world::BlockModel::TrapDoor) {
        GameplayMutationSink sink{world, session};
        const bool open = !clickedState.open();
        session.worldMutations().setBlock(world, {clicked.x, clicked.y, clicked.z},
                                          clickedState.withOpen(open),
                                          world::MutationFlags::All,
                                          world::MutationCause::PlayerPlace, sink);
        session.events().publish(
            SoundEvent{open ? SoundEventKind::BlockOpen : SoundEventKind::BlockClose,
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
    // ButtonBlock#playSound: a press is click_on. `heavy` carries the on state.
    session.events().publish(SoundEvent{SoundEventKind::BlockClick,
                                        glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block(),
                                        nullptr, 1.0F, /*heavy(on)=*/true});
    return true;
}

// LeverBlock#useWithoutItem: a right-click flips POWERED and notifies neighbours
// (the redstone update fans out through MutationFlags::All, as the button's does)
// — the lever previously had emission tables but no interaction to toggle it, so
// it could never actually be thrown. The click sound's `heavy` carries the new
// on state (LeverBlock#playSound picks the pitch from it). Returns whether a
// toggle happened, matching pressButton/toggleDoorOrGate.
[[nodiscard]] bool toggleLever(GameSession& session, world::World& world, glm::ivec3 clicked) {
    const auto clickedState = world.state(clicked.x, clicked.y, clicked.z);
    if (clickedState.block() != world::Block::Lever) {
        return false;
    }
    const bool on = !clickedState.powered();
    GameplayMutationSink sink{world, session};
    session.worldMutations().setBlock(world, {clicked.x, clicked.y, clicked.z},
                                      clickedState.withPowered(on), world::MutationFlags::All,
                                      world::MutationCause::PlayerPlace, sink);
    session.events().publish(SoundEvent{SoundEventKind::BlockClick,
                                        glm::vec3{clicked} + glm::vec3{0.5F}, clickedState.block(),
                                        nullptr, 1.0F, /*heavy(on)=*/on});
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
        // The plate now has empty collision (BasePressurePlateBlock#
        // getCollisionShape), so an entity standing on a plate rests on the
        // block *below* the plate and its feet sit at the plate cell's floor
        // (that support block's top) plus kGroundOffset — i.e. inside the plate
        // cell. The plate is therefore the feet's own cell, floor(feet.y), the
        // same cell BasePressurePlateBlock's TOUCH_AABB (a box at the plate
        // position) tests entity boxes against. The old `feet.y - 0.05` probe
        // assumed the plate lifted the entity 1/16 above the cell floor, which
        // only held while the plate still had collision.
        return glm::ivec3{static_cast<int>(std::floor(feet.x)),
                          static_cast<int>(std::floor(feet.y)),
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
    // BasePressurePlateBlock#checkPressed: on a signal change, write the new
    // POWERED state (flags 2, matching vanilla's level.setBlock(pos, state, 2) —
    // clients only, no built-in neighbour fan-out from the write itself) and then
    // explicitly call updateNeighbours, which — unlike the write's own
    // NotifyNeighbors fan-out — notifies *both* the plate's own six neighbours
    // AND the six neighbours of the block the plate sits on
    // (BasePressurePlateBlock#updateNeighbours: `level.updateNeighborsAt(pos, ...)`
    // + `level.updateNeighborsAt(pos.below(), ...)`). That second call is what
    // lets a wire or repeater sitting directly under the solid block the plate
    // rests on react — a cell the plate's own six neighbours never reach.
    const auto setPowered = [&](glm::ivec3 cell, bool powered) {
        const auto state = world.state(cell.x, cell.y, cell.z);
        if (state.powered() == powered) {
            return;
        }
        GameplayMutationSink sink{world, session};
        session.worldMutations().setBlock(world, {cell.x, cell.y, cell.z}, state.withPowered(powered),
                                          world::MutationFlags::NotifyClients,
                                          world::MutationCause::PlayerPlace, sink);
        session.worldMutations().updateNeighborsAt({cell.x, cell.y, cell.z}, sink);
        session.worldMutations().updateNeighborsAt({cell.x, cell.y - 1, cell.z}, sink);
        // BasePressurePlateBlock#playOnSound/playOffSound: a plate clicks on when
        // pressed and off when released (block.<plate>.click_on/click_off), the
        // same BlockClick event a button uses, `heavy` carrying the on state.
        session.events().publish(SoundEvent{SoundEventKind::BlockClick,
                                            glm::vec3{cell} + glm::vec3{0.5F}, state.block(),
                                            nullptr, 1.0F, /*heavy(on)=*/powered});
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
    // A UseItemOn/UseItem command is only ever enqueued on the button's
    // GLFW_PRESS edge (UseItemStop is the release) — so seeing either one this
    // tick IS a fresh press, never a held-button repeat. That is exactly the
    // edge auto-equip needs: ArmorItem#use fires once per right-click, not
    // once per tick the button stays down (the 4-tick repeat below is for
    // placement/bucket items, which DO want to keep firing while held).
    bool freshUsePress = false;
    // Consume the queued inputs in order.
    for (const auto& command : commands) {
        std::visit(
            [&](const auto& specific) {
                using T = std::decay_t<decltype(specific)>;
                if constexpr (std::is_same_v<T, PlayerAction>) {
                    handleDestroyCommand(session, world, host, specific);
                } else if constexpr (std::is_same_v<T, UseItemOn>) {
                    using_ = true;
                    freshUsePress = true;
                    latestUse_ = specific;
                } else if constexpr (std::is_same_v<T, UseItem>) {
                    using_ = true;
                    freshUsePress = true;
                    latestUse_.reset();
                } else if constexpr (std::is_same_v<T, UseItemStop>) {
                    using_ = false;
                    // RW-1: BowItem#onStoppedUsing — the release edge fires
                    // exactly once, here, whether or not a draw was actually
                    // active (releaseBow no-ops when it was not). The aim
                    // reads this tick's own staged copy of the look direction
                    // (primaryPlayer().playerInput, refreshed once at the top
                    // of GameSession::tick before this call runs) rather than
                    // the render thread's live stagedInput, so every system
                    // that reads "which way is the player looking" this tick
                    // agrees, the same consistency PlayerController::tick's
                    // own read of playerInput already relies on.
                    session.releaseBow(kPrimaryPlayerId, session.primaryPlayer().playerInput.lookDirection,
                                       host);
                } else if constexpr (std::is_same_v<T, ClickSlot>) {
                    // A container/inventory slot click executes on the server
                    // tick, resolved against the open container (26.1's
                    // AbstractContainerMenu) and routed by ScreenHandler.
                    const auto& click = specific;
                    gameplay::ScreenContext context = buildScreenContext(session);
                    // The active creative tab is client-only UI state; the click
                    // carries it so the creative delete-on-shift-click branch
                    // (an item-category tab) is distinguishable from the
                    // Inventory tab's ordinary hotbar<->main swap.
                    context.creativeInventoryTab = click.creativeInventoryTab;
                    gameplay::SlotView slot;
                    slot.kind = click.kind;
                    slot.index = click.slotIndex;
                    slot.storage = gameplay::ScreenHandler::resolveSlotStorage(
                        session, context, click.kind, click.slotIndex);
                    gameplay::ScreenHandler::click(
                        session, context, slot,
                        static_cast<gameplay::InventoryMouseButton>(click.button),
                        click.shiftHeld);
                } else if constexpr (std::is_same_v<T, ClickEnchantOption>) {
                    // EnchantmentMenu#clickMenuButton. Everything it decides —
                    // the level threshold, the level and lapis spend, applying
                    // the enchantments, rerolling the seed — is gameplay, so the
                    // client only ever says which of the three bars was pressed.
                    static_cast<void>(session.purchaseEnchantment(specific.optionIndex));
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
    // use with food (or, AR-A3, milk) in hand starts (or keeps) the vanilla
    // 32-tick meal/drink, independently of the 4-tick rightClickDelay;
    // attacking cancels it. startsUseTimeline covers both — GameSession picks
    // the Eat/Drink animation and the hunger-vs-clear-effects finish behaviour
    // from the held item itself.
    const auto& selectedStack = session.inventory().selectedStack();
    const bool foodInHand = startsUseTimeline(selectedStack.item);
    const bool targetedContainer = latestUse_.has_value() && !latestUse_->entity &&
                                   isContainerBlock(world, latestUse_->block);
    const bool plantable = latestUse_.has_value() && !latestUse_->entity &&
                           aimsAtPlantableFarmland(world, *latestUse_, selectedStack);
    // EQ-1: ArmorItem#use, on the press edge only (armor equips once per
    // click, not every tick the button stays down like a bucket's repeated
    // placement). A container target opens the container instead — the same
    // "the block gets first refusal" ordering the eat/plant checks above use
    // — and an entity target goes through performUseOnEntity's own switch,
    // never this generic path.
    const bool targetedEntity = latestUse_.has_value() && latestUse_->entity;
    if (freshUsePress && !targetedContainer && !targetedEntity && !session.eating()) {
        if (tryAutoEquipArmor(session)) {
            session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
        }
    }
    // AR-CX0: a creature interaction (shear/dye/milk/feed) fires once on the
    // press edge, exactly like vanilla's Entity#interact — one right-click, one
    // mobInteract. Unlike block placement/bucket (the 4-tick held repeat gate
    // at the bottom of this tick), a mob interaction must NOT depend on `using_`
    // still being set: a fast click whose press and release land in the same
    // server tick's command batch would clear `using_` before that gate runs,
    // dropping the interaction entirely. Driving it off freshUsePress here (and
    // excluding entity targets from the held repeat gate below) makes it a true
    // one-shot: it fires this tick whether or not the release already arrived,
    // and never double-fires or repeats while held.
    if (freshUsePress && targetedEntity && !session.eating()) {
        performUseOnEntity(session, world, *latestUse_);
    }
    // RW-1: BowItem#use — a fresh right-click with a bow in hand starts the
    // draw, on the same press edge auto-equip uses (a bow held down must not
    // re-issue startUsing every tick — PlayerActionState::startUsing already
    // refuses to restart an active use, but gating on freshUsePress here keeps
    // the intent explicit and matches vanilla's use() firing once per click).
    // Survival needs a real arrow somewhere in the inventory before the draw
    // even begins (BowItem.java's `if (!user.abilities.creativeMode && !bl)
    // return TypedActionResult.fail(...)`); creative always may draw. A
    // container/entity target takes right-click priority the same way the
    // eat/plant/armor checks already give it first refusal.
    const bool bowInHand = isBow(selectedStack.item);
    const bool drawingBow =
        session.playerActions().use.active && session.playerActions().use.animation == UseAnimation::Bow;
    if (freshUsePress && bowInHand && !targetedContainer && !targetedEntity && !drawingBow &&
        !session.eating()) {
        if (session.gameMode() == GameMode::Creative ||
            session.inventory().findFirstArrowSlot().has_value()) {
            session.beginDrawingBow(kPrimaryPlayerId, host);
        }
    }
    if (using_ && foodInHand && !targetedContainer && !plantable && !session.eating()) {
        session.beginEating(kPrimaryPlayerId, selectedStack.item, host);
    } else if (session.eating() &&
               (!using_ || !foodInHand || selectedStack.item != session.eatingKind() ||
                targetedContainer)) {
        session.cancelEating(kPrimaryPlayerId, host);
    }
    // Attacking interrupts an in-progress meal or bow draw, the same
    // LivingEntity#clearActiveItem an attack swing triggers in vanilla for
    // any active use item, not just food.
    if (destroying_) {
        if (session.eating()) {
            session.cancelEating(kPrimaryPlayerId, host);
        }
        if (drawingBow) {
            session.playerActions().stopUsing();
        }
    }

    // The continuous dig, once per tick while the attack is held.
    if (destroying_ && destroyTarget_.has_value()) {
        continueDig(session, world);
    }

    // The repeated use, once per tick while held (vanilla's 4-tick
    // rightClickDelay lives here now, not in the renderer). A bow draw owns
    // the use timeline exclusively — like eating, it must not also re-enter
    // the block-placement ladder below while held.
    //
    // AR-CX0: entity targets are deliberately excluded — a mob interaction is a
    // one-shot already dispatched on the press edge above, so re-entering it
    // here would both double-fire the same click (two shears, two dye spent)
    // and wrongly repeat it every 4 ticks the button stays down. Only block
    // placement/bucket keep the held repeat semantics.
    const bool heldEntity = latestUse_.has_value() && latestUse_->entity;
    if (using_ && latestUse_.has_value() && !heldEntity && session.serverTick() >= nextUseTick_ &&
        !session.eating() && !drawingBow) {
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
            const float baseDamage =
                toolType(weapon) == ToolType::None ? 1.0F : attributes.attackDamage;
            // ENCH-1: the outgoing melee weapon enchants (Sharpness/Smite/Bane of
            // Arthropods/Knockback/Fire Aspect), read off the held weapon and the
            // target's category before the hit lands — PlayerEntity#attack reads
            // EnchantmentHelper.getAttackDamage/getKnockback/getFireAspect the
            // same way, ahead of the target.damage() call.
            const auto* target = session.worldEntities().byIdConst(action.entityId);
            const bool targetUndead = target != nullptr && target->type != nullptr &&
                target->type->isUndead();
            const bool targetArthropod = target != nullptr && target->type != nullptr &&
                target->type->isArthropod();
            const float damage = baseDamage +
                meleeDamageEnchantBonus(weapon, targetUndead, targetArthropod);
            const float extraKnockback = meleeKnockbackEnchantBonus(weapon);
            const int fireAspectSeconds = meleeFireAspectSeconds(weapon);
            const std::uint8_t baneLevel =
                targetArthropod ? enchantmentLevel(weapon, EnchantmentId::BaneOfArthropods) : 0U;
            const glm::vec3 eye = session.player().eyePosition();
            session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Break, 6U);
            if (session.worldEntities().hurt(action.entityId, damage, eye,
                                             ActorReference::player(), DamageType::EntityAttack,
                                             extraKnockback)) {
                if (fireAspectSeconds > 0) {
                    session.worldEntities().setOnFire(action.entityId, fireAspectSeconds);
                }
                // DamageEnchantment#onTargetDamaged (typeIndex==2): Bane of
                // Arthropods lands Slowness IV on an arthropod target for a
                // level-scaled duration. Drawn from the target's own
                // reproducible RNG stream (see applyBaneOfArthropodsSlowness),
                // never the wall clock.
                if (baneLevel > 0U) {
                    session.worldEntities().applyBaneOfArthropodsSlowness(action.entityId,
                                                                          baneLevel);
                }
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
    // AR-A2: a creature hit never reaches the block ladder below — shearing/
    // feeding a mob has nothing to do with the aimed-at block cell (use.block
    // is unset for an entity hit; see UseItemOn's comment).
    if (use.entity) {
        performUseOnEntity(session, world, use);
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
         pressButton(session, world, use.block) || toggleLever(session, world, use.block))) {
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
    case BlockInteraction::OpenEnchantingTable:
        // ENCH-2: no block entity to back-fill (the menu is on the player), so
        // opening is just "remember which table, scan its shelves, derive the
        // three offers". The scan happens here rather than on the tick loop's
        // first pass so the very first frame of the screen already shows real
        // costs instead of three blanks.
        session.openEnchantingContainer(world, use.block);
        session.events().publish(ClientActionEvent{ClientActionEventKind::OpenContainer,
                                                   ContainerScreen::EnchantingTable, use.block,
                                                   true});
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
        case ItemUseAction::PlaceFire: {
            // AR-CX4-b: FlintAndSteelItem#useOn — write Fire into the adjacent
            // cell (already resolved to a replaceable, survivable target by
            // igniteWithFlintAndSteel). Unlike PlaceBlock, the flint and steel is
            // not consumed as a stack; in survival it pays one durability point,
            // mirroring HoeItem's Till cost handling above.
            const auto block = placeTarget;
            GameplayMutationSink sink{world, session};
            if (session.worldMutations()
                    .setBlock(world, {block.x, block.y, block.z}, itemUse.state,
                              world::MutationFlags::All, world::MutationCause::PlayerPlace, sink)
                    .changed) {
                // FlintAndSteelItem#useOn plays item.flintandsteel.use (the fizz),
                // not the fire block's place sound.
                session.events().publish(SoundEvent{SoundEventKind::FlintAndSteelUse,
                                                    glm::vec3{block} + glm::vec3{0.5F}});
                session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
                if (session.gameMode() == GameMode::Survival) {
                    if (session.damageHeldTool(kPrimaryPlayerId, ToolUse::Ignite, 0.0F)) {
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

void PlayerInteraction::performUseOnEntity(GameSession& session, world::World&,
                                           const UseItemOn& use) {
    const bool interactDebug = std::getenv("MC_REBEDROCK_INTERACT_DEBUG") != nullptr;
    const SimpleEntity* target = session.worldEntities().byIdConst(use.entityId);
    if (target == nullptr || target->dead()) {
        // The server half of the trace: the command arrived but named an entity
        // the authoritative side no longer has (or already dead) — the interaction
        // is dropped here, not in the shear/dye branches below.
        if (interactDebug) {
            std::cout << "[interact] server performUseOnEntity id=" << use.entityId
                      << " -> NO TARGET (null or dead), interaction dropped" << std::endl;
        }
        return;
    }
    const auto& selectedStack = session.inventory().selectedStack();
    if (interactDebug) {
        const std::string_view heldName =
            selectedStack.item != nullptr ? std::string_view{selectedStack.item->identifier.path}
                                          : std::string_view{"<block/empty>"};
        std::cout << "[interact] server performUseOnEntity id=" << use.entityId
                  << " species=" << target->kind().id().path << " held=" << heldName
                  << " dyeable=" << (target->kind().dyeable() ? 1 : 0)
                  << " sheared=" << (target->sheared ? 1 : 0) << std::endl;
    }

    // Sheep#mobInteract: shears win over the tempt-feed branch below (a shears
    // stack is never a species' tempt item, so the two never actually compete,
    // but vanilla's own dispatch checks shears first and this mirrors that
    // order for AR-A3/AR-A4 to extend safely).
    if (selectedStack.item == &items::Shears) {
        if (!session.worldEntities().shear(use.entityId)) {
            // Sabotage anchor ③: an already-sheared (or baby, or dead) sheep
            // must not drop wool or spend durability — bail before either.
            if (interactDebug) {
                std::cout << "[interact]   SHEARS branch: shear() refused (already "
                             "sheared / baby / not shearable)" << std::endl;
            }
            return;
        }
        if (interactDebug) {
            std::cout << "[interact]   SHEARS branch: sheared OK, dropping wool" << std::endl;
        }
        // Sheep.json (26.1): 1-3 wool from a shear, tinted by the sheep's dye
        // colour (Sheep#dropFromShearing: `getColor()` selects the wool block).
        // DYE-2: woolBlockFor maps the entity's authoritative DyeColor to its
        // wool Block via a constexpr table — a white sheep still drops white
        // wool (the default a freshly-spawned sheep carries), a dyed one drops
        // its colour, with no per-colour branch here. The loot table's own RNG
        // stream does not exist in this build, so the roll (and the drop's
        // scatter angle) uses the same deterministic per-tick stream every other
        // world edit in this file already draws from.
        auto& rng = session.lootRandomState();
        const auto woolCount = static_cast<std::uint8_t>(1U + mc::rng::nextInt(rng, 3U));
        const world::Block woolBlock = items::woolBlockFor(target->color);
        const ItemStack woolStack{woolBlock, woolCount, blockItemFor(woolBlock)};
        // Sheep#shear plays entity.sheep.shear.
        session.events().publish(SoundEvent{SoundEventKind::Shear, target->position});
        const float angle = mc::rng::nextFloat(rng) * 6.28318530718F;
        session.spawnItemEntity(target->position + glm::vec3{0.0F, target->dimensions().height * 0.5F, 0.0F},
                                woolStack,
                                glm::vec3{std::cos(angle), 0.15F, std::sin(angle)} * 0.1F);
        session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
        // Sheep#mobInteract: `itemStack.hurtAndBreak(1, ...)` — a flat one point,
        // unlike the sword-vs-mining-tool AttackEntity table (damageHeldTool's
        // ToolUse enum has no "flat one point" case, so this calls
        // damageSelected directly rather than stretching that table for shears).
        if (session.gameMode() == GameMode::Survival && session.inventory().damageSelected(1U)) {
            session.events().publish(SoundEvent{SoundEventKind::ItemBreak,
                                                session.player().eyePosition()});
        }
        return;
    }

    // DYE-1: SheepEntity#mobInteract's DyeItem branch — a dye right-clicked on a
    // dyeable creature recolours it. Runs after shears (vanilla checks shears
    // first) and before the milk/feed branches below; a dye stack is never a
    // milk bucket or a species' tempt item, so the branches never actually
    // compete, but this mirrors vanilla's dispatch order. worldEntities().dye
    // owns the gate (dyeable species, colour actually changing) and returns
    // whether the colour changed; only then is the arm swung and — survival
    // only — one dye spent. A same-colour dye or a non-dyeable target no-ops
    // (nothing swung, nothing consumed), which is sabotage anchor ②'s contract.
    if (const auto dyeColor = dyeColorForItem(selectedStack.item)) {
        const bool changed = session.worldEntities().dye(use.entityId, *dyeColor);
        if (interactDebug) {
            std::cout << "[interact]   DYE branch: dye() " << (changed ? "recoloured" : "no-op")
                      << " (non-dyeable species or same colour)" << std::endl;
        }
        if (changed) {
            session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
            // DyeItem: `if (!player.abilities.creativeMode) itemStack.decrement(1)`
            // — creative keeps its dye, survival spends exactly one. Sabotage
            // anchor ③ is this consume gate.
            if (session.gameMode() == GameMode::Survival) {
                static_cast<void>(session.inventory().consumeSelected());
            }
        }
        // A dye click never falls through to milking/feeding, whether or not the
        // colour changed — the held item is a dye, not a bucket or tempt item.
        return;
    }

    // AbstractCow#mobInteract (26.1): an empty bucket right-clicked on a
    // non-baby cow returns a milk bucket. Gated on the target's species (the
    // manifest row's EntityType, compared by its stable address — the same
    // pointer-identity idiom the tempt/breeding check below uses for
    // BreedingProfile) rather than a generic capability bit: milking has no
    // vanilla analogue on any other species, so unlike shear/tempt this stays
    // a one-species check instead of a data table entry.
    if (selectedStack.item == &items::Bucket &&
        &target->kind() == &entities::builtinSpecies("cow") && !target->baby()) {
        // Sabotage anchor ①(non-empty-bucket half): only the plain empty
        // Bucket item reaches here — WaterBucket/LavaBucket are distinct Item
        // registrations, so a full bucket simply never matches this branch
        // and produces no milk.
        session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
        // BucketItem#getEmptiedStack / ItemUtils#createFilledResult: survival
        // spends the empty bucket for the milk bucket; creative pours without
        // spending anything (hasInfiniteMaterials keeps the original empty
        // bucket in hand, mirroring the water/lava CollectX branches in
        // performUse). No cooldown — unlike shears, vanilla's cow milking has
        // no durability or timer, so the same bucket can be emptied again
        // next click once refilled.
        //
        // selectedStack is a reference into the same slot replaceSelected
        // writes, so the original empty-bucket stack must be snapshotted by
        // value first — restoring `selectedStack` after the write would just
        // hand the milk bucket back to itself.
        const ItemStack originalStack = selectedStack;
        session.inventory().replaceSelected({world::Block::Air, 1U, &items::MilkBucket});
        if (restoresHeldStack(session.gameMode())) {
            // Sabotage anchor ①(creative half): creative must keep its empty
            // bucket rather than pocket a free milk bucket.
            session.inventory().replaceSelected(originalStack);
        }
        return;
    }

    // Animal#mobInteract's feed branch: the held stack must match this
    // creature's own tempt item (BreedingProfile.temptItem — the parameterized
    // check TemptGoal itself uses, see sameItem's callers), not a hardcoded
    // wheat check, so AR-A3/AR-A4's cow/chicken need only set their own
    // breeding profile to reach this same code path. Sabotage anchor ② is this
    // exact comparison.
    const auto& breeding = target->kind().breeding();
    if (target->kind().breedable() && !breeding.temptItem.empty() &&
        sameItem(selectedStack, breeding.temptItem)) {
        // Animal#mobInteract splits the feed into two exclusive halves (its own
        // two if-blocks): an adult (age == 0) that can fall in love enters love;
        // a baby (age < 0) instead has its growth sped up by
        // getSpeedUpSecondsWhenFeeding(-age). Only one fires per feed, and each
        // consumes one food item + swings the arm on success. A baby never
        // enters love and an adult never "ages up" — the two branches never
        // overlap. Whether it went into love or grew, on either success the item
        // is spent (usePlayerItem in both vanilla arms).
        const SimpleEntity& creature = *target;
        bool fed = false;
        if (creature.age == 0) {
            // Adult half: setInLove (breed cooldown blocks a just-bred adult, so
            // it returns false and no item is spent then).
            fed = session.worldEntities().setInLove(use.entityId);
        } else if (creature.baby()) {
            // Baby half (#9): speed growth by the vanilla formula on the ticks
            // still to go (-age). ageUp returns false if it somehow could not
            // move (never for a real baby), so no item is wasted.
            const int seconds = getSpeedUpSecondsWhenFeeding(-creature.age);
            fed = session.worldEntities().ageUp(use.entityId, seconds);
        }
        if (fed) {
            session.playerActions().swingHand(InteractionHand::Main, SwingAnimation::Use, 6U);
            if (session.gameMode() == GameMode::Survival) {
                static_cast<void>(session.inventory().consumeSelected());
            }
        }
    }
}

} // namespace mc::gameplay
