#include "gameplay/GameSession.hpp"

#include "gameplay/ArmorEnchantment.hpp"
#include "gameplay/BlockEntityTicker.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EnchantmentMining.hpp"
#include "gameplay/DimensionTransfer.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/RangedEnchantment.hpp"
#include "gameplay/StatusEffect.hpp"

#include "world/DayNightCycle.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace mc::gameplay {

namespace {
constexpr float kInfiniteDamage = std::numeric_limits<float>::infinity();

// Floor division toward negative infinity, so a cross-dimension query at a
// negative world coordinate lands on the correct chunk (a plain `/ 16` truncates
// toward zero and would misplace the boundary chunk).
[[nodiscard]] int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && (remainder < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// EQ-2: LivingEntity#getArmor / #getAttributeValue(GENERIC_ARMOR_TOUGHNESS) —
// the defender's armor points and toughness are an ADDITION sum of each
// equipped ArmorItem's per-slot modifier across the four armor slots (never
// the offhand, which is not armor). Zero for a naked player, matching vanilla.
struct ArmorTotals final {
    float armor = 0.0F;
    float toughness = 0.0F;
};

[[nodiscard]] ArmorTotals sumEquippedArmor(const EquipmentSlots& equipment) {
    ArmorTotals totals;
    for (const EquipmentSlot slot : kArmorSlots) {
        const Item* item = equipment.equippedArmor(slot).item;
        totals.armor += static_cast<float>(armorValue(item));
        totals.toughness += armorToughness(item);
    }
    return totals;
}

// LivingEntity#damageArmor's default body deals `max(1, amount / 4)` to
// EVERY equipped armor piece on a hit the armor stage actually reduced
// (ArmorItem#damage is 1 per point of that shared cost, split across however
// many armor pieces are worn — vanilla's own doc comment on the override:
// "each point of damage is randomly assigned to a piece"; the base
// LivingEntity implementation this project follows applies it to all four
// uniformly, `float f = amount / 4.0F; if (f < 1.0F) f = 1.0F;` then a call
// per slot). Reuses the same break-at-threshold arithmetic
// Inventory::damageSelected already uses for a worn-down tool, just indexed
// through EquipmentSlots instead of the hotbar-selected slot.
void damageEquippedArmor(EquipmentSlots& equipment, float damage) {
    const auto cost = static_cast<std::uint16_t>(std::max(1.0F, std::floor(damage / 4.0F)));
    for (const EquipmentSlot slot : kArmorSlots) {
        ItemStack& piece = equipment.mutableSlot(slot);
        const std::uint16_t maximumDamage = itemMaximumDamage(piece);
        if (piece.empty() || maximumDamage == 0U) {
            continue;
        }
        if (piece.damage + cost > maximumDamage) {
            piece = {};
            continue;
        }
        piece.damage = static_cast<std::uint16_t>(piece.damage + cost);
    }
}

// EQ-4: ThornsEnchantment#onUserDamaged spends its durability on the SINGLE
// enchanted piece that fired (ItemStack#damage(amount)), not divided across
// every worn piece the way a hit's own armor wear (damageEquippedArmor above)
// is — so this is a targeted variant: `cost` points of damage on `slot`,
// breaking the piece if it crosses its maximum, exactly as
// Inventory::damageSelected / damageEquippedArmor break a worn-out item.
void damageThornsArmorPiece(EquipmentSlots& equipment, EquipmentSlot slot, float amount) {
    ItemStack& piece = equipment.mutableSlot(slot);
    const std::uint16_t maximumDamage = itemMaximumDamage(piece);
    if (piece.empty() || maximumDamage == 0U) {
        return;
    }
    const auto cost = static_cast<std::uint16_t>(std::max(1.0F, std::floor(amount)));
    if (piece.damage + cost > maximumDamage) {
        piece = {};
        return;
    }
    piece.damage = static_cast<std::uint16_t>(piece.damage + cost);
}
} // namespace

GameSession::GameSession() {
    auto initialSnapshots = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(initialSnapshots);
    storeSnapshotBundle(
        std::shared_ptr<const RenderSnapshots>{std::move(initialSnapshots)});
    // The single local player lives in the slot map; the primary id is always
    // present. (The initializer-list form would call primaryPlayer() before the
    // map was constructed, so the body emplaces the player instead.)
    players_.emplace(kPrimaryPlayerId,
                     ServerPlayer{glm::vec3{24.0F, 78.0F - PlayerController::kEyeHeight, 24.0F}});
    // A fresh world opens at morning, the same tick the old single clock seeded
    // itself with.
    clocks_.setTotalTicks(world::ClockId::Overworld,
                          static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));
    attachGameRuleHandlers();
    // AR-B4-6: teach the redstone simulation how to read a container. The
    // block entities live here, not in the simulation, so this is the one place
    // that can answer it — and the simulation stays free of any notion of what a
    // chest is (WorldSimulation::AnalogOutputFn).
    worldSimulation_.setAnalogOutputSource(&GameSession::analogOutputAt, this);
}

int GameSession::analogOutputAt(const void* context, world::BlockPos pos) {
    const auto* session = static_cast<const GameSession*>(context);
    // A "fill fraction" summed over the container's slots, exactly Java's
    // `sum(count / maxStackSize)`; redstoneSignalFromContainer then divides by
    // the slot count and discretises. -1 means "not a container at all", which
    // is what lets an *empty* chest still override the comparator's other input
    // while a stone block does not.
    if (const auto* chest = session->chestSystem_.find(pos); chest != nullptr) {
        float fill = 0.0F;
        for (const auto& slot : chest->items) {
            if (slot.count == 0U) {
                continue;
            }
            fill += static_cast<float>(slot.count) /
                    static_cast<float>(itemMaximumStackSize(slot));
        }
        return redstone::redstoneSignalFromContainer(
            fill, static_cast<int>(ChestBlockEntity::kSlotCount));
    }
    if (const auto* furnace = session->furnaceSystem_.find(pos); furnace != nullptr) {
        // AbstractFurnaceBlockEntity is a three-slot Container (input, fuel,
        // output), read by the same formula.
        float fill = 0.0F;
        for (const auto* slot : {&furnace->input, &furnace->fuel, &furnace->output}) {
            if (slot->count == 0U) {
                continue;
            }
            fill += static_cast<float>(slot->count) /
                    static_cast<float>(itemMaximumStackSize(*slot));
        }
        return redstone::redstoneSignalFromContainer(fill, 3);
    }
    return -1;
}

// Every public entry that takes a SimulationHost binds it before doing
// anything, because the events raised inside must reach *that* host — the
// per-call host argument is the contract these methods have always had. Once
// P3 Step 3 replaces the bridge with a queue there is only ever one consumer,
// and the parameter can go away entirely.
void GameSession::tick(world::World& world, SimulationHost& host) {
    hostBridge_.setHost(&host);
    // The server tick is unconditional: no gamerule, command or pause reaches
    // it, so everything timed against it (mining, cooldowns, scheduled work)
    // keeps running even when the sun is frozen.
    ++serverTick_;
    // The action timeline (swing arc, ongoing use) advances once per tick, so
    // an action consumes the same ticks at any frame rate.
    primaryPlayer().actions.tick();
    // advance_time is the *global* gate on the whole clock manager, exactly the
    // shape ServerClockManager#tick has (`if (advanceTime) clocks.forEach(tick)`).
    // It used to be written into the overworld clock's own `paused` flag
    // instead, which conflated the two: a rule the player never touched would
    // stomp whatever `/time pause` had set, every tick. They are separate
    // states in vanilla — the effective pause is `paused || !advanceTime` — and
    // now here too, which is what makes `/time pause` and `/time resume` mean
    // anything.
    if (gameRules_.get<bool>(GameRuleId::AdvanceTime)) {
        clocks_.tick();
    }
    // ServerWorld.tick runs its weather section first, before the world and
    // entities; the auto-cycle is gated on the doWeatherCycle gamerule the same
    // way doDaylightCycle gates the day.
    primaryLevel().weather.tick(gameRules_.get<bool>(GameRuleId::AdvanceWeather));
    // Level#updateSkyBrightness, right after the clock and the weather that
    // feed it and before anything reads light. Resolved once here and handed
    // down as a POD: growth, spreading and spawning all read the same fields
    // for the same tick instead of each deriving its own idea of how dark it
    // is. 26.1 does the same thing through EnvironmentAttributes, with
    // invalidateTickCache standing where this single call does.
    environment_ = EnvironmentSnapshot::resolve(static_cast<double>(dayTimeTicks()),
                                                primaryLevel().weather.rainGradient(),
                                                primaryLevel().weather.thunderGradient());
    worldSimulation_.setEnvironment(environment_);
    // Take the published input once, at the top of the tick, so the whole tick
    // sees one consistent keyboard state rather than whatever the main thread
    // happened to be writing partway through.
    {
        const std::lock_guard<std::mutex> guard{inputMutex_};
        primaryPlayer().playerInput = primaryPlayer().sharedInput;
        // The jump and sprint-double-tap edges come from their accumulators, not
        // sharedInput's level copy — a press ORed in by any input between two ticks
        // reaches this tick exactly once, then is cleared.
        primaryPlayer().playerInput.jumpPressed = jumpPressed_;
        primaryPlayer().playerInput.forwardPressed = forwardPressed_;
        jumpPressed_ = false;
        forwardPressed_ = false;
    }
    primaryPlayer().physicsPrevious = primaryPlayer().physicsCurrent;
    primaryPlayer().controller.tick(world, primaryPlayer().playerInput);
    // FarmlandBlock#onLandedUpon: on a landing, the player's fall distance
    // (Entity.fallDistance, tracked across frames in PlayerController) decides
    // whether the tilled soil under the feet tramples back to dirt. Vanilla
    // vanilla rolls nextFloat() < fallDistance - 0.5f, so a one-block fall breaks
    // farmland half the time and a taller one almost always; walking never
    // tramples.
    if (primaryPlayer().controller.onGround() && primaryPlayer().controller.fallDistance() > 0.5F) {
        const auto feet = primaryPlayer().controller.position();
        const int trampleX = static_cast<int>(std::floor(feet.x));
        const int trampleY = static_cast<int>(std::floor(feet.y - 0.001F));
        const int trampleZ = static_cast<int>(std::floor(feet.z));
        const auto soil = world.block(trampleX, trampleY, trampleZ);
        const float roll = mc::rng::nextFloat(lootRandomState_);
        if (world::isFarmland(soil) && roll < primaryPlayer().controller.fallDistance() - 0.5F) {
            // Trampling farmland is an ordinary world edit, so it goes through
            // the service: the section is dirtied and the neighbours (a crop
            // standing on the soil) are told, which the hand-written version
            // never did.
            GameplayMutationSink sink{world, *this};
            static_cast<void>(worldMutations_.setBlock(
                world, {trampleX, trampleY, trampleZ}, world::BlockState{world::Block::Dirt},
                world::MutationFlags::All, world::MutationCause::Gravity, sink));
            events_.publish(SoundEvent{SoundEventKind::BlockBreak,
                                      {static_cast<float>(trampleX) + 0.5F,
                                       static_cast<float>(trampleY) + 0.5F,
                                       static_cast<float>(trampleZ) + 0.5F},
                                      soil});
            events_.publish(
                ParticleEvent{ParticleEventKind::BlockBreak, {trampleX, trampleY, trampleZ}, soil});
            // The crop above loses its farmland and pops.
            worldSimulation_.notifyNeighborChanged(world, {trampleX, trampleY, trampleZ});
        }
    }
    updateMovementAudio(world, primaryPlayer().physicsPrevious, primaryPlayer().controller.position());
    tickPlayerVitals(host, world, primaryPlayer().physicsPrevious, primaryPlayer().controller.jumpedThisTick());
    tickEating(host);
    // The fluid phase runs once per accumulator drain; the renderer never lets
    // more than one batch of overdue water updates stack up across frames.
    bool fluidUpdatePhaseConsumed = false;
    // Confine random ticks to the simulation distance around the player, the way
    // 26.1's ServerWorld does — the same window entity ticking and spawning above
    // already use (simulationRadiusBlocks_), converted to a chunk radius with a
    // one-chunk margin so blocks at the very edge still tick. Without this the
    // random-tick pass walked every loaded chunk, so a high randomTickSpeed cost
    // scaled with render distance and stalled the frame.
    const auto simFeet = primaryPlayer().controller.position();
    const int radiusChunks = static_cast<int>(simulationRadiusBlocks_) / 16 + 1;
    worldSimulation_.setSimulationBounds(
        floorDiv(static_cast<int>(std::floor(simFeet.x)), 16),
        floorDiv(static_cast<int>(std::floor(simFeet.z)), 16), radiusChunks);
    // The exact centre too: fire_spread_radius_around_player measures in blocks,
    // which the chunk-granular bounds above cannot answer.
    worldSimulation_.setSimulationCenterBlock(static_cast<int>(std::floor(simFeet.x)),
                                              static_cast<int>(std::floor(simFeet.y)),
                                              static_cast<int>(std::floor(simFeet.z)));
    for (const auto& change : worldSimulation_.tick(world, !fluidUpdatePhaseConsumed)) {
        // A simulated break previews too (it used to do so further down, just
        // before its sound), so the edit's immediacy is decided once, here.
        const bool simulatedBreak =
            change.dropped.block() != world::Block::Air && change.worldChanged;
        if (change.worldChanged) {
            events_.publish(WorldEditEvent{
                change.position.x, change.position.y, change.position.z, change.state,
                change.immediateRenderUpdate || simulatedBreak});
        }
        // A simulated break — an attached block that lost its support, a
        // decoration a fluid washed away, or a leaf that decayed — is a real
        // block break: vanilla plays the break sound, throws the break particles
        // and rolls the loot table through World#breakBlock(pos, true), which is
        // game-mode independent (a wall torch drops in creative too). A falling
        // block that could not be placed also rolls its item here, but carries
        // worldChanged == false so it produces no fake break effects or edit.
        // Fluid spread changes carry dropped == Air, so the thousand-cell flows
        // never pay for this pass.
        if (change.dropped.block() != world::Block::Air) {
            if (change.worldChanged) {
                events_.publish(SoundEvent{SoundEventKind::BlockBreak,
                                          {static_cast<float>(change.position.x) + 0.5F,
                                           static_cast<float>(change.position.y) + 0.5F,
                                           static_cast<float>(change.position.z) + 0.5F},
                                          change.dropped.block()});
                events_.publish(ParticleEvent{
                    ParticleEventKind::BlockBreak,
                    {change.position.x, change.position.y, change.position.z},
                    change.dropped.block()});
            }
            // Nobody swung a tool at these, so they roll the same loot table an
            // empty hand would. The dropped *state* travels, so a popped crop
            // rolls its loot from the age it had reached.
            spawnBlockDrops({change.position.x, change.position.y, change.position.z},
                            change.dropped, ItemStack{});
        }
    }
    fluidUpdatePhaseConsumed = true;
    // Block entities advance through the ticker behaviour table, not a hand-list
    // of system tick() calls: each type with a ticker steps its container once,
    // in ascending BlockEntityTypeId order, and a tickless type is skipped by the
    // pre-filter. Chest lids and furnace burns are both driven here.
    tickBlockEntities(BlockEntityTickContext{chestSystem_, trappedChestSystem_, furnaceSystem_});
    // Every placed furnace smelts on its own now, screen open or not. Mirror its
    // authoritative LIT state — after the furnace ticker ran this tick — while
    // this tick owns the server-world write section; the mutation event carries
    // the client mesh/light update later.
    syncFurnaceLitStates(world);
    // ENCH-2: EnchantmentMenu#slotsChanged, driven from the tick rather than
    // from the click — vanilla recomputes through ContainerLevelAccess whenever
    // the container changes, and a rescan is also the only way a bookshelf
    // placed while the screen is open can raise the offers. Cheap and
    // self-guarding: it returns immediately unless the enchanting screen is
    // open, and re-derives only when (seed, shelf count, item) actually moved.
    refreshEnchantingOffers(world);
    if (primaryLevel().items.tick(world, primaryPlayer().controller.position(), primaryPlayer().inventory) > 0U) {
        events_.publish(SoundEvent{SoundEventKind::ItemPickup, primaryPlayer().controller.position()});
    }
    // XP-1: the experience orb pool — physics/magnet/merge/despawn, then
    // contact pickup credits primaryPlayer().experience directly (no loot/
    // inventory indirection, unlike item drops). Reuses ItemPickup for the
    // collect sound; XP has no dedicated orb sound event yet (AU scope).
    // ENCH-3: what Mending can repair — the four armor slots, the offhand and
    // the held item, which is vanilla's own candidate set (getRandomItemWith
    // walks the equipment slots, and the main hand is one of them). Rebuilt per
    // tick because a slot's stack can move between ticks; six pointers on the
    // stack, no allocation.
    std::array<ItemStack*, kEquipmentSlotCount + 1U> mendingCandidates{};
    for (std::size_t slot = 0; slot < kEquipmentSlotCount; ++slot) {
        mendingCandidates[slot] =
            &primaryPlayer().equipment.mutableSlot(static_cast<EquipmentSlot>(slot));
    }
    mendingCandidates[kEquipmentSlotCount] =
        &primaryPlayer().inventory.mutableSlot(primaryPlayer().inventory.selectedHotbarSlot());
    const MendingTargets mending{mendingCandidates, &mendingRandom_};
    if (primaryLevel().experienceOrbs.tick(world, primaryPlayer().controller.position(),
                                           !primaryPlayer().vitals.dead(),
                                           primaryPlayer().experience, mending) > 0) {
        events_.publish(SoundEvent{SoundEventKind::ItemPickup, primaryPlayer().controller.position()});
    }
    // The herd pushes back: Entity#pushAwayFrom moves both parties, so a pig
    // walking into the player nudges them. Difficulty is per-save (level.dat).
    // AR-M2: environment_ carries the tick's ambientDarkness, so the daylight-
    // ignition rule's "is it day" reads the exact same source NaturalSpawner's
    // darkness check already does (see EntitySystem::tick's doc comment) —
    // neither can disagree about the time of day.
    const auto entityTick = primaryLevel().entities.tick(
        world, primaryPlayer().controller.position(), PlayerController::kWidth, PlayerController::kHeight,
        difficulty_, !primaryPlayer().vitals.dead(), primaryPlayer().gameMode == GameMode::Creative,
        simulationRadiusBlocks_, primaryLevel().weather.isRaining(),
        primaryPlayer().inventory.selectedStack(), environment_);
    primaryPlayer().controller.applyExternalPush(entityTick.playerPush);
    for (const auto& attack : entityTick.mobAttacks) {
        if (attack.target == ActorReference::player()) {
            // The raw swing. The difficulty scaling is the damage type's own
            // rule now (DamageScaling::WhenCausedByLivingNonPlayer), applied
            // inside the pipeline against the unscaled amount the way
            // LivingEntity#hurt does — the call site used to apply it here,
            // which put it on the wrong side of the invulnerability window.
            const bool landed =
                hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, attack.amount, host, true);
            // AR-M2: HuskEntity#tryAttack — a landed hit from an attacker whose
            // type carries EntityBehavior::HungerOnHit (husk; the zombie beside
            // it in BuiltinSpecies.cpp carries no such bit) also applies EM2's
            // hunger effect. Read off the type, not a species id compare, so
            // this never grows an `if (species == husk)` branch here.
            if (landed) {
                if (const auto* attacker = primaryLevel().entities.byIdConst(attack.attackerId);
                    attacker != nullptr && attacker->type != nullptr &&
                    attacker->type->hungerOnHit()) {
                    static_cast<void>(primaryPlayer().vitals.applyEffect(
                        hungerEffect(), huskHungerDurationTicks(difficulty_), 0U));
                }
                // EQ-4: ThornsEnchantment#onUserDamaged — a landed hit gives the
                // victim's worn Thorns armor a chance to reflect damage onto the
                // attacker. The trigger chance (0.15*level) and the reflected
                // damage roll (1..5) are both taken through the DDC-2 effect
                // engine off this session's deterministic thornsRandom_ stream
                // (never a wall clock), so the sequence replays identically for a
                // given seed. When it fires, the reflected damage routes through
                // the same EntitySystem::hurt pipeline any hit uses (so the
                // attacker's own future armor/effects apply), and the enchanted
                // piece spends its durability.
                const ThornsReflection thorns =
                    resolveThorns(primaryPlayer().equipment, thornsRandom_);
                if (thorns.fired && thorns.attackerDamage > 0.0F) {
                    static_cast<void>(primaryLevel().entities.hurt(
                        attack.attackerId, thorns.attackerDamage,
                        primaryPlayer().controller.position(), ActorReference::player(),
                        DamageType::Generic));
                    if (thorns.itemDamage > 0.0F) {
                        damageThornsArmorPiece(primaryPlayer().equipment, thorns.slot,
                                               thorns.itemDamage);
                    }
                }
            }
        }
    }
    // RW-0: the projectile pool — physics/raycast hit (entity through
    // Damage.hpp above, or block -> stick), landed-arrow pickup, lifetime
    // despawn. Runs after the creature tick above so a hit this same tick
    // sees the herd's post-move positions, matching the ordering entityTick's
    // own mobAttacks already established.
    // RW-1a #7 — hand the projectile tick the player's inventory so a pickup is
    // consumed only when the arrow actually fits; the returned stacks are just
    // the ones that WERE stowed (already added inside tick), so here we only
    // play the pickup sound rather than re-adding (a double-add would dupe).
    for (const auto& pickup : primaryLevel().projectiles.tick(
             world, primaryLevel().entities, primaryPlayer().controller.position(),
             !primaryPlayer().vitals.dead(), projectileRandom_, &primaryPlayer().inventory)) {
        static_cast<void>(pickup);
        events_.publish(SoundEvent{SoundEventKind::ItemPickup, primaryPlayer().controller.position()});
    }
    // AR-A2: EatGrassGoal filed these mid-tick, when it only held a
    // `const World&` and could not write the cell itself (see MobBrain's
    // requestEatGrass comment). This is where the write actually happens —
    // through WorldMutationService, exactly like the farmland-trample edit
    // just above, so the eaten cell's neighbours and light get the same
    // treatment a player's own break would give it. The block is re-checked
    // here (not trusted from the request) because a tick can pass between the
    // goal filing the request and this drain running.
    for (const auto& request : entityTick.grassEats) {
        const auto& cell = request.cell;
        const auto current = world.block(cell.x, cell.y, cell.z);
        world::BlockState next;
        if (current == world::Block::GrassPlant) {
            next = world::BlockState{};  // short_grass -> air
        } else if (current == world::Block::Grass) {
            next = world::BlockState{world::Block::Dirt};  // grass_block -> dirt
        } else {
            continue;  // already gone (another sheep, a break) — nothing to eat
        }
        GameplayMutationSink sink{world, *this};
        if (worldMutations_
                .setBlock(world, {cell.x, cell.y, cell.z}, next, world::MutationFlags::All,
                          world::MutationCause::Gravity, sink)
                .changed) {
            // Sheep#ate: regrow wool and age a lamb up 60s. Replaces the older
            // clearSheared-only relay so eating grass also speeds a lamb's
            // growth, matching vanilla ate().
            static_cast<void>(worldEntities().ate(request.entityId));
        }
    }
    // NaturalSpawner: creatures and monsters settle inside the simulation
    // radius, respecting each category's spawnCap and the biome's spawn table.
    // It reads the tick's ambient darkness off the same snapshot the growth
    // checks use, so "dark enough for a monster" and "too dark for grass to
    // spread" can no longer disagree about the time of day.
    // NaturalSpawner#spawnCategoryForPosition reads spawn_mobs before it places
    // anything; skipping the whole pass is the same thing one level up, and
    // costs nothing when the rule is off.
    if (gameRules_.get<bool>(GameRuleId::SpawnMobs)) {
        primaryLevel().spawner.tick(world, primaryLevel().entities, primaryPlayer().controller.position(), simulationRadiusBlocks_,
                             difficulty_, environment_);
    }
    consumeEntityEvents();
    // The authoritative interaction: consume the render thread's queued commands
    // and apply the dig/use decisions once per tick, after every other system
    // has settled (the old renderer applied them per frame between ticks, which
    // is the same ordering — the edits land on the next tick's processing).
    playerInteraction_.tick(*this, world, host, commandQueue_.drain());
    // AR-B3: pressure plates check the player's feet and every live creature's
    // — a bounded set rather than a full loaded-chunk scan (see
    // PlayerInteraction.hpp's tickPressurePlates comment).
    {
        std::vector<glm::vec3> creatureFeet;
        creatureFeet.reserve(primaryLevel().entities.entities().size());
        for (const auto& creature : primaryLevel().entities.entities()) {
            creatureFeet.push_back(creature.position);
        }
        tickPressurePlates(*this, world, primaryPlayer().controller.position(), creatureFeet,
                           pressedPlates_);
    }
    // DIM-2: after the player's dimension has ticked in full, advance every other
    // active dimension's passive simulation (MinecraftServer.tickChildren walks
    // every ServerLevel, not only the player's). Dormant dimensions cost a
    // branch each; the loop is fixed DimensionId order for determinism.
    tickSecondaryLevels();
    // Publish the per-tick snapshots under the caller's world write lock, so
    // the render thread interpolates a coherent frame from them instead of
    // reading live gameplay objects the tick may be mid-mutation on. Only the
    // player's dimension is mirrored to the client — secondary dimensions tick
    // server-side with no render coupling.
    publishSnapshots();
}

void GameSession::tickSecondaryLevels() {
    const bool doWeatherCycle = gameRules_.get<bool>(GameRuleId::AdvanceWeather);
    // Fixed ascending DimensionId order: the loop must never depend on hash-map
    // iteration order, so the same seed produces the same per-tick sequence
    // across every dimension (determinism iron rule).
    for (std::size_t index = 0; index < world::kDimensionCount; ++index) {
        const auto dim = static_cast<world::DimensionId>(index);
        if (dim == primaryDimension_) {
            // The player's dimension already ticked in full above; record its
            // residency for symmetry but do not tick it twice.
            secondaryLevelReports_[index] = LevelTickReport{
                /*skippedEmpty=*/level(dim).isDormant(),
                /*chunksResident=*/level(dim).isDormant() ? 0U : level(dim).world().chunkCount(),
                /*creaturesTicked=*/0U};
            continue;
        }
        secondaryLevelReports_[index] = level(dim).tickPassive(doWeatherCycle, difficulty_);
    }
}

void GameSession::recordPendingCrossDimLoad(PendingCrossDimLoad request) {
    if (std::find(pendingCrossDimLoads_.begin(), pendingCrossDimLoads_.end(), request) ==
        pendingCrossDimLoads_.end()) {
        pendingCrossDimLoads_.push_back(request);
    }
}

world::Block GameSession::blockAcrossDimensions(world::DimensionId id, int x, int y, int z) {
    Level& target = level(id);
    // No world bound at all: the dimension is not even set up. Nothing to load;
    // return Air (JE getChunk on an absent level yields nothing).
    if (!target.hasWorld()) {
        return world::Block::Air;
    }
    const world::ChunkPosition chunkPos{floorDiv(x, 16), floorDiv(z, 16)};
    if (target.world().hasChunk(chunkPos)) {
        // Loaded: a plain read, one hash lookup. Never a generate.
        return target.world().block(x, y, z);
    }
    // Unloaded: record an async request for the streamer and return the default.
    // Crucially this does NOT create/generate the chunk — synchronous generation
    // in a tick is the long-tail root cause DIM-2 must never reintroduce.
    //
    // WG-4 (DIM-3 leftover #1): dedup by (dimension, chunk). A cross-dimension read
    // of an unloaded chunk that a caller repeats every tick — a redstone comparator
    // reaching into the Nether, say — used to push a fresh request each time, so the
    // deferred list grew without bound until the chunk finally loaded. Recording
    // each (dim, chunk) once keeps the queue bounded by the number of *distinct*
    // unloaded chunks queried, not the number of queries.
    recordPendingCrossDimLoad({id, chunkPos});
    return world::Block::Air;
}

GameSession::CrossDimLoadRouting GameSession::resolvePendingCrossDimLoads() {
    CrossDimLoadRouting routing;
    std::vector<PendingCrossDimLoad> deferred;
    for (const auto& request : pendingCrossDimLoads_) {
        // The generator seam: a dimension with a real terrain generator (only the
        // Overworld today) can be handed to a streamer; the Nether/End requests
        // are held until the worldgen subtree delivers their generators. Either
        // way this only *routes* — it never generates a chunk in the tick.
        if (world::dimensionGeneratorConfig(request.dimension).hasTerrainGenerator) {
            ++routing.routableToStreamer;
            // A live per-dimension streamer would enqueue request.chunk here; the
            // Overworld already streams through GameRuntime's single streamer, so
            // there is no second streamer to feed yet (see DIM-3 blocked note).
        } else {
            ++routing.deferredNoGenerator;
            deferred.push_back(request);
        }
    }
    // Keep only the deferred (generator-less) requests; the routable ones are
    // considered handed off.
    pendingCrossDimLoads_ = std::move(deferred);
    return routing;
}

namespace {
// Re-creates a detached creature in a target Level's entity system at `position`,
// preserving the state and RNG stream a save round-trip preserves (velocity,
// health, anger, age, rng, fire, effects, love). Returns the new stable id.
std::uint64_t recreateInLevel(Level& target, const SimpleEntity& entity, glm::vec3 position) {
    return target.entities.restore(
        position, *entity.type, entity.yaw, entity.velocity, entity.damage.health,
        entity.angerTicks, entity.ageTicks, entity.rngState, entity.fireTicks,
        entity.effects, entity.age, entity.loveTicks);
}
}  // namespace

GameSession::TransferResult GameSession::transferEntity(std::uint64_t entityId,
                                                        world::DimensionId from,
                                                        world::DimensionId to) {
    // Detach from the source (no death/loot) — the extraction half of the move.
    auto detached = level(from).entities.detach(entityId);
    if (!detached.has_value()) {
        return TransferResult::SourceMissing;
    }
    // Scale the horizontal coordinate by the dimensions' scale ratio (DIM-0),
    // never a hardcoded 8.
    const glm::vec3 scaled =
        scaleCoordinatesBetweenDimensions(detached->position, from, to);
    Level& targetLevel = level(to);
    const PortalDestination destination =
        resolvePortalDestination(targetLevel.hasWorld() ? &targetLevel.world() : nullptr, scaled);
    switch (destination.status) {
    case PortalDestinationStatus::NoWorld:
        // Put the creature back where it was — a transfer to an unbound dimension
        // is a no-op, not a loss.
        static_cast<void>(recreateInLevel(level(from), *detached, detached->position));
        return TransferResult::NoTargetWorld;
    case PortalDestinationStatus::AwaitingChunk:
        // The destination chunk is not loaded. Queue the transfer with the
        // creature's state intact — its position already scaled to the target —
        // and record an async load request; never a synchronous generate in the
        // tick. drainQueuedTransfers lands it once the chunk is resident.
        detached->position = destination.position;  // already scaled
        queuedTransfers_.push_back({std::move(*detached), to, destination.chunk});
        recordPendingCrossDimLoad({to, destination.chunk});
        return TransferResult::QueuedAwaitingChunk;
    case PortalDestinationStatus::Ready:
        static_cast<void>(recreateInLevel(targetLevel, *detached, destination.position));
        return TransferResult::Moved;
    }
    return TransferResult::SourceMissing;
}

std::size_t GameSession::drainQueuedTransfers() {
    std::size_t landed = 0;
    std::vector<QueuedTransfer> stillWaiting;
    for (auto& queued : queuedTransfers_) {
        Level& targetLevel = level(queued.to);
        // Only land when the destination chunk is now resident; never load it here.
        if (targetLevel.hasWorld() && targetLevel.world().hasChunk(queued.destinationChunk)) {
            const glm::vec3 position = queued.entity.position;  // already scaled at queue time
            static_cast<void>(recreateInLevel(targetLevel, queued.entity, position));
            ++landed;
        } else {
            stillWaiting.push_back(std::move(queued));
        }
    }
    queuedTransfers_ = std::move(stillWaiting);
    return landed;
}

glm::vec3 GameSession::transferPlayer(world::DimensionId to) {
    const world::DimensionId from = primaryDimension_;
    const glm::vec3 scaled =
        scaleCoordinatesBetweenDimensions(primaryPlayer().controller.position(), from, to);
    // Hand the player flag from the old Level to the new one, and repoint the
    // authoritative primary dimension. The world binding + camera/stream re-anchor
    // is GameRuntime's to perform (mirroring respawn); this owns the dimension
    // identity and the scaled coordinate.
    level(from).hasPlayer = false;
    level(to).hasPlayer = true;
    level(to).id = to;
    primaryDimension_ = to;
    return scaled;
}

void GameSession::publishSnapshots() {
    // The authoritative current position, then everything the renderer reads
    // this frame. Called at the end of tick() and once right after a world
    // load; a cold start restores the player's saved coordinates into the live
    // controller and the physics endpoints before this runs, so the published
    // snapshot carries the real position — never the default (0,0,0).
    primaryPlayer().physicsCurrent = primaryPlayer().controller.position();
    // The per-tick player snapshot, published under the caller's world write
    // lock so the render thread interpolates a coherent frame from it instead
    // of reading live gameplay objects the tick may be mid-mutation on. It is
    // built into a pooled bundle that has no readers and atomically published
    // at the end, so the render thread pins a complete frame without a lock.
    auto snapshots = acquireSnapshotWriteBundle();
    auto& playerTickSnapshot_ = snapshots->player;
    auto& worldSnapshot_ = snapshots->world;
    auto& entitySnapshot_ = snapshots->entities;
    playerTickSnapshot_.serverTick = serverTick_;
    playerTickSnapshot_.swing = primaryPlayer().actions.swing;
    playerTickSnapshot_.use = primaryPlayer().actions.use;
    playerTickSnapshot_.physicsPrevious = primaryPlayer().physicsPrevious;
    playerTickSnapshot_.physicsCurrent = primaryPlayer().physicsCurrent;
    playerTickSnapshot_.previousStride = primaryPlayer().controller.previousStrideDistance();
    playerTickSnapshot_.stride = primaryPlayer().controller.strideDistance();
    playerTickSnapshot_.previousSpeed = primaryPlayer().controller.previousHorizontalSpeed();
    playerTickSnapshot_.speed = primaryPlayer().controller.horizontalSpeed();
    // ANIM A1/A2: the vanilla WalkAnimationState amplitude + phase for the
    // third-person gait clips (separate from the view-bob stride above).
    playerTickSnapshot_.previousWalkAmount =
        primaryPlayer().controller.previousWalkAnimationSpeed();
    playerTickSnapshot_.walkAmount = primaryPlayer().controller.walkAnimationSpeed();
    playerTickSnapshot_.previousWalkPosition =
        primaryPlayer().controller.previousWalkAnimationPosition();
    playerTickSnapshot_.walkPosition = primaryPlayer().controller.walkAnimationPosition();
    playerTickSnapshot_.sneaking = primaryPlayer().controller.sneaking();
    playerTickSnapshot_.flying = primaryPlayer().controller.flying();
    playerTickSnapshot_.sprinting = primaryPlayer().controller.sprinting();
    playerTickSnapshot_.inWater = primaryPlayer().controller.inWater();
    playerTickSnapshot_.onGround = primaryPlayer().controller.onGround();
    playerTickSnapshot_.previousFieldOfViewMultiplier =
        primaryPlayer().controller.previousFieldOfViewMultiplier();
    playerTickSnapshot_.fieldOfViewMultiplier =
        primaryPlayer().controller.fieldOfViewMultiplier();
    playerTickSnapshot_.heldStack = primaryPlayer().inventory.selectedStack();
    // The dig the interaction pass is mid-way through, so the crack overlay
    // reads the published snapshot instead of the live PlayerInteraction.
    const auto dig = playerInteraction_.digSnapshot();
    playerTickSnapshot_.digging = {dig.active, dig.target, dig.startedTick};
    playerTickSnapshot_.health = primaryPlayer().vitals.health();
    playerTickSnapshot_.foodLevel = primaryPlayer().vitals.foodLevel();
    playerTickSnapshot_.airTicks = primaryPlayer().vitals.airTicks();
    playerTickSnapshot_.ticksSinceDamage = primaryPlayer().vitals.ticksSinceDamage();
    playerTickSnapshot_.experienceLevel = primaryPlayer().experience.level();
    playerTickSnapshot_.experienceProgress = primaryPlayer().experience.progress();
    playerTickSnapshot_.gameMode = primaryPlayer().gameMode;
    playerTickSnapshot_.eating = primaryPlayer().eating;
    playerTickSnapshot_.selectedHotbarSlot = primaryPlayer().inventory.selectedHotbarSlot();
    // The render-visible world state, captured under the same lock.
    worldSnapshot_.serverTick = serverTick_;
    worldSnapshot_.previousRainGradient = primaryLevel().weather.previousRainGradient();
    worldSnapshot_.rainGradient = primaryLevel().weather.rainGradient();
    worldSnapshot_.previousThunderGradient = primaryLevel().weather.previousThunderGradient();
    worldSnapshot_.thunderGradient = primaryLevel().weather.thunderGradient();
    // The gradient-derived flags, matching what the renderer's rain/sky reads.
    worldSnapshot_.raining = primaryLevel().weather.isRaining();
    worldSnapshot_.thundering = primaryLevel().weather.isThundering();
    worldSnapshot_.dayTimeTicks = static_cast<double>(dayTimeTicks());
    for (std::size_t index = 0; index < world::kClockCount; ++index) {
        worldSnapshot_.clocks[index] = clocks_.state(static_cast<world::ClockId>(index));
    }
    worldSnapshot_.doDaylightCycle = gameRules_.get<bool>(GameRuleId::AdvanceTime);
    worldSnapshot_.doWeatherCycle = gameRules_.get<bool>(GameRuleId::AdvanceWeather);
    worldSnapshot_.worldSpawnPosition = worldSpawnPosition_;
    worldSnapshot_.playerSpawnPosition = primaryPlayer().spawnPosition;
    worldSnapshot_.playerSpawnYaw = primaryPlayer().spawnYaw;
    worldSnapshot_.hasPlayerSpawn = primaryPlayer().hasSpawn;
    worldSnapshot_.openContainerScreen = openContainerScreen_;
    worldSnapshot_.openChest = openChest_;
    worldSnapshot_.openFurnace = openFurnace_;
    // The chest lid render state, so the world renderer draws lids from the
    // snapshot instead of the live chest system.
    worldSnapshot_.chests.clear();
    worldSnapshot_.chests.reserve(chestSystem_.entities().size());
    for (const auto& chest : chestSystem_.entities()) {
        worldSnapshot_.chests.push_back(
            {chest.position, chest.previousLidAngle, chest.lidAngle});
    }
    // The container screen's display state: the player's inventory and cursor
    // plus the open container's contents, all values so no reference into a
    // gameplay vector survives the tick boundary.
    const auto& primary = primaryPlayer();
    for (std::size_t i = 0; i < Inventory::kSlotCount; ++i) {
        worldSnapshot_.inventorySlots[i] = primary.inventory.slot(i);
    }
    worldSnapshot_.cursorStack = primary.inventory.cursorStack();
    // EQ-0: the equipment slots ride the same per-tick snapshot, values not
    // references, the same "no gameplay reference survives the tick
    // boundary" contract every other snapshot field here follows.
    for (std::size_t i = 0; i < kEquipmentSlotCount; ++i) {
        worldSnapshot_.equipmentSlots[i] = primary.equipment.get(static_cast<EquipmentSlot>(i));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        worldSnapshot_.playerCraftingGrid[i] = primary.crafting.playerSlot(i);
    }
    worldSnapshot_.playerCraftingOutput = primary.crafting.playerOutput();
    for (std::size_t i = 0; i < 9; ++i) {
        worldSnapshot_.tableCraftingGrid[i] = primary.crafting.tableSlot(i);
    }
    worldSnapshot_.tableCraftingOutput = primary.crafting.tableOutput();
    if (openChest_.has_value()) {
        if (const auto* chest = chestSystem_.find(*openChest_); chest != nullptr) {
            for (std::size_t i = 0; i < ChestBlockEntity::kSlotCount; ++i) {
                worldSnapshot_.chestItems[i] = chest->items[i];
            }
        }
    }
    if (openFurnace_.has_value()) {
        const gameplay::FurnacePosition furnace{openFurnace_->x, openFurnace_->y,
                                                openFurnace_->z};
        if (const auto* entity = furnaceSystem_.find(furnace); entity != nullptr) {
            worldSnapshot_.furnaceInput = entity->input;
            worldSnapshot_.furnaceFuel = entity->fuel;
            worldSnapshot_.furnaceOutput = entity->output;
        }
        worldSnapshot_.furnaceFuelProgress = furnaceSystem_.fuelProgress(furnace);
        worldSnapshot_.furnaceCookProgress = furnaceSystem_.cookProgress(furnace);
    }
    if (openAnvil_.has_value()) {
        const AnvilMenu& menu = primaryPlayer().anvil;
        worldSnapshot_.anvilLeft = menu.left;
        worldSnapshot_.anvilRight = menu.right;
        worldSnapshot_.anvilResult = menu.result;
        worldSnapshot_.anvilCost = menu.cost;
    }
    if (openEnchantingTable_.has_value()) {
        const EnchantingMenu& menu = primaryPlayer().enchanting;
        worldSnapshot_.enchantingItem = menu.item;
        worldSnapshot_.enchantingLapis = menu.lapis;
        worldSnapshot_.enchantingBookshelfPower = static_cast<std::int32_t>(menu.bookshelfPower);
        worldSnapshot_.enchantingSeed = primaryPlayer().experience.enchantmentSeed();
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            const auto& offer = menu.offers.slots[slot];
            worldSnapshot_.enchantingRequiredLevels[slot] = offer.requiredLevel;
            // EnchantmentMenu's enchantClue/levelClue: only the FIRST rolled
            // enchantment is revealed, and only when the bar is live. The rest
            // of the offer stays hidden until the purchase lands — that hidden
            // remainder is the whole point of the preview.
            if (offer.requiredLevel > 0 && !offer.enchantments.empty()) {
                worldSnapshot_.enchantingClueIds[slot] =
                    static_cast<std::uint8_t>(offer.enchantments.front().id);
                worldSnapshot_.enchantingClueLevels[slot] =
                    static_cast<std::uint8_t>(offer.enchantments.front().level);
            } else {
                worldSnapshot_.enchantingClueIds[slot] = 0U;
                worldSnapshot_.enchantingClueLevels[slot] = 0U;
            }
        }
    }
    // Last, once every system has settled: what the renderer will draw from
    // until the next tick replaces it.
    entitySnapshot_.capture(primaryLevel().entities.entities(), primaryLevel().items.entities(),
                            primaryLevel().experienceOrbs.entities(),
                            primaryLevel().projectiles.entities(),
                            worldSimulation_.fallingBlocks());
    // Stamp the publish time so the render thread derives the interpolation alpha
    // from this very bundle. Written just before the atomic publish, so a reader
    // that pins this bundle sees a timestamp that belongs to the endpoints it
    // carries — the alpha and the endpoints can never be a tick out of step.
    snapshots->tickPublishRep =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // Publish all three views together. Readers that already loaded the previous
    // shared bundle keep it alive until their copies finish.
    publishSnapshotBundle(snapshots);
}

std::shared_ptr<GameSession::RenderSnapshots> GameSession::acquireSnapshotWriteBundle() {
    const auto current = loadSnapshotBundle();
    for (const auto& candidate : snapshotPool_) {
        if (candidate.get() != current.get() && candidate.use_count() == 1) {
            return candidate;
        }
    }
    auto candidate = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(candidate);
    return candidate;
}

void GameSession::publishSnapshotBundle(const std::shared_ptr<RenderSnapshots>& snapshots) {
    storeSnapshotBundle(std::shared_ptr<const RenderSnapshots>{snapshots});
}

std::shared_ptr<const GameSession::RenderSnapshots> GameSession::loadSnapshotBundle() const {
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(__APPLE__)
    return publishedSnapshots_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&publishedSnapshots_, std::memory_order_acquire);
#endif
}

void GameSession::storeSnapshotBundle(
    std::shared_ptr<const RenderSnapshots> snapshots) {
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(__APPLE__)
    publishedSnapshots_.store(std::move(snapshots), std::memory_order_release);
#else
    std::atomic_store_explicit(
        &publishedSnapshots_, std::move(snapshots), std::memory_order_release);
#endif
}

namespace {
// The interpolation alpha for a bundle published at `tickPublishRep` (a
// steady_clock::duration rep): how far now sits past that publish, in [0,1]. A
// zero rep means nothing has been published yet, so the frame sits exactly on
// the (default) snapshot and the alpha is 0.
[[nodiscard]] float alphaFromPublishRep(std::int64_t tickPublishRep) {
    if (tickPublishRep == 0) {
        return 0.0F;
    }
    const std::chrono::steady_clock::time_point published{
        std::chrono::steady_clock::duration{tickPublishRep}};
    const float elapsed =
        std::chrono::duration<float>{std::chrono::steady_clock::now() - published}.count();
    return std::clamp(elapsed / PlayerController::kTickSeconds, 0.0F, 1.0F);
}
}  // namespace

float GameSession::interpolationAlpha() const {
    return alphaFromPublishRep(loadSnapshotBundle()->tickPublishRep);
}

GameSession::InterpolatedEntities GameSession::entityRenderFrame() const {
    const auto bundle = loadSnapshotBundle();
    return {bundle->entities, alphaFromPublishRep(bundle->tickPublishRep)};
}

void GameSession::commitInput() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    primaryPlayer().sharedInput = primaryPlayer().stagedInput;
}

void GameSession::applyMovementInput(const MovementInput& intent) {
    // The gated fields are the server's to decide, not the client's: creative
    // flight follows the authoritative game mode, and sprinting needs a food
    // level above six (unless flight is allowed). Deriving them here — instead of
    // trusting the client's copy of gameMode/foodLevel — is the authority the
    // pre-split renderer used to hold and a networked client must not.
    const bool flightAllowed = primaryPlayer().gameMode == GameMode::Creative;
    const bool sprintAllowed = flightAllowed || primaryPlayer().vitals.foodLevel() > 6;
    const std::lock_guard<std::mutex> guard{inputMutex_};
    // Stage the raw intent onto the published input the tick reads at its top.
    // Both stagedInput and sharedInput are set so any residual staged reader sees
    // the same state; the tick takes sharedInput.
    PlayerInput& staged = primaryPlayer().stagedInput;
    staged.forward = intent.forward;
    staged.strafe = intent.strafe;
    staged.lookDirection = intent.lookDirection;
    staged.jumpHeld = intent.jumpHeld;
    staged.descendHeld = intent.descendHeld;
    staged.sneakHeld = intent.sneakHeld;
    staged.sprintHeld = intent.sprintHeld;
    staged.autoJump = intent.autoJump;
    staged.flightAllowed = flightAllowed;
    staged.sprintAllowed = sprintAllowed;
    primaryPlayer().sharedInput = staged;
    // The two edges are consumed once by the next tick and cleared there, so they
    // must be OR-accumulated — not written level-triggered into sharedInput, which
    // a later frame's send in the same between-tick interval would overwrite back
    // to false (losing the press). The tick reads jumpPressed_/forwardPressed_,
    // not sharedInput's copies. This is what makes the sprint double-tap survive
    // when several frames land between two ticks.
    if (intent.jumpPressed) {
        jumpPressed_ = true;
    }
    if (intent.forwardPressed) {
        forwardPressed_ = true;
    }
}

void GameSession::setJumpPressed() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    jumpPressed_ = true;
}

void GameSession::setForwardPressed() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    forwardPressed_ = true;
}

bool GameSession::forwardPressed() const {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    return forwardPressed_;
}

void GameSession::clearInputEdges() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    jumpPressed_ = false;
    forwardPressed_ = false;
}

void GameSession::setGameMode(GameMode mode) {
    primaryPlayer().gameMode = mode;
    primaryPlayer().stagedInput.flightAllowed = mode == GameMode::Creative;
    const std::lock_guard<std::mutex> guard{inputMutex_};
    primaryPlayer().sharedInput.flightAllowed = primaryPlayer().stagedInput.flightAllowed;
}

bool GameSession::hurtPlayer(PlayerId playerId, DamageType source, float amount,
                             SimulationHost& host, bool causedByLivingNonPlayer) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    // EQ-2: the armor/toughness stage reads the player's currently worn
    // armor, summed fresh on every hit (armor can change between hits, so
    // this is not cached on the player).
    const ArmorTotals armor = sumEquippedArmor(player.equipment);
    // EQ-4: the enchantment protection factor (EPF) the worn armor contributes
    // against THIS damage type, summed through the DDC-2 effect engine (Fire
    // Protection only counts on an IsFire hit, Feather Falling only on a fall,
    // …). Zero for an unenchanted / unarmored player, a no-op fold downstream.
    const float epf = enchantmentProtectionFactor(player.equipment, source);
    bool armorApplied = false;
    float preArmorDamage = 0.0F;
    if (!player.vitals.hurt(amount, source, causedByLivingNonPlayer, armor.armor,
                            armor.toughness, &armorApplied, &preArmorDamage, epf)) {
        return false;
    }
    // LivingEntity#applyArmorToDamage calls damageArmor() unconditionally
    // whenever the hit was not bypassesArmor — even a hit the toughness/armor
    // math reduces to a sliver still wears the armor down. armorApplied is
    // exactly that "the stage ran" signal (false on a bypass or an unarmored
    // player, where armor.armor == 0 skips the reduction call entirely);
    // preArmorDamage is the post-difficulty, pre-reduction amount
    // PlayerInventory#damageArmor actually divides by four — not the raw
    // `amount` parameter (pre-difficulty) and not the already-reduced health
    // cost.
    if (armorApplied) {
        damageEquippedArmor(player.equipment, preArmorDamage);
    }
    events_.publish(SoundEvent{SoundEventKind::PlayerHurt, player.controller.position()});
    if (player.vitals.dead()) {
        die(playerId, source, host);
    }
    return true;
}

void GameSession::killPlayer(PlayerId playerId, SimulationHost& host) {
    hostBridge_.setHost(&host);
    (void)hurtPlayer(playerId, DamageType::OutOfWorld, kInfiniteDamage, host);
}

bool GameSession::die(PlayerId playerId, DamageType source, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    // PlayerEntity#onDeath: the shared beginDeath guard is the `dead` field
    // that keeps onDeath from firing twice, so a tick that both falls and
    // drowns raises the death screen exactly once.
    static_cast<void>(source);
    if (!beginDeath(player.vitals.damage())) {
        return false;
    }
    // Closing stows the cursor/crafting grid first, preserving the old death
    // ordering, then inventory scattering completes on the simulation thread
    // before the presentation-only death event is queued.
    closeContainerMenu();
    onPlayerDeath(playerId);
    events_.publish(PlayerDiedEvent{});
    return true;
}

void GameSession::respawn(PlayerId playerId) {
    // PlayerManager#respawnPlayer prefers the player's personal spawn point and
    // only falls back to the world spawn when none was set. vanilla also respawns
    // facing due north (yaw 0) regardless of the spawn point's stored angle.
    auto& player = players_.at(playerId);
    player.vitals.reset();
    const glm::vec3 spawn = player.hasSpawn ? player.spawnPosition : worldSpawnPosition_;
    player.controller.resetForRespawn(spawn);
    player.physicsPrevious = spawn;
    player.physicsCurrent = spawn;
    // Mirror into a fresh published snapshot. resetForRespawn clears sneaking,
    // and the camera derives its eye height from the snapshot's sneaking, so the
    // fresh standing body must publish a standing eye height too.
    const auto current = loadSnapshotBundle();
    auto updated = acquireSnapshotWriteBundle();
    *updated = *current;
    updated->player.physicsPrevious = spawn;
    updated->player.physicsCurrent = spawn;
    updated->player.sneaking = false;
    publishSnapshotBundle(updated);
}

void GameSession::beginEating(PlayerId playerId, const Item* kind, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    player.eating = true;
    player.eatingKind = kind;
    player.eatTicks = 0;
    // The meal (or, AR-A3, the milk drink) is just UseAnimation::Eat/Drink on
    // the shared item-use timeline; the renderer reads the countdown from
    // playerActions(), not a private eat state. ArmPose::deriveArmPose already
    // treats both animations identically, so nothing downstream needs to know
    // which one this is beyond the finish-of-use branch in tickEating.
    player.actions.startUsing(InteractionHand::Main,
                              isDrinkable(kind) ? UseAnimation::Drink : UseAnimation::Eat,
                              kEatTicks);
    events_.publish(ClientActionEvent{ClientActionEventKind::EatingStarted});
}

void GameSession::cancelEating(PlayerId playerId, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    if (!player.eating) {
        return;
    }
    player.eating = false;
    player.eatingKind = nullptr;
    player.eatTicks = 0;
    player.actions.stopUsing();
    events_.publish(ClientActionEvent{ClientActionEventKind::EatingCancelled});
}

void GameSession::beginDrawingBow(PlayerId playerId, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    // startUsing no-ops if a use is already active (a bow held through
    // another right-click edge), matching startUsing's own "no restart" rule
    // eating already relies on.
    player.actions.startUsing(InteractionHand::Main, UseAnimation::Bow, kBowMaxUseTicks);
}

void GameSession::releaseBow(PlayerId playerId, const glm::vec3& lookDirection,
                             SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    if (!player.actions.use.active || player.actions.use.animation != UseAnimation::Bow) {
        return;
    }
    // BowItem.onStoppedUsing: `this.getMaxUseTime(stack) - remainingUseTicks`
    // — elapsed ticks since the draw started, read off the still-live use
    // state before stopUsing() below clears it.
    const std::uint32_t elapsedTicks =
        player.actions.use.durationTicks - player.actions.use.remainingTicks;
    const float pullProgress = bowPullProgress(elapsedTicks);
    // stopUsing() always ends the draw on release, whether or not a shot
    // actually fires — vanilla's onStoppedUsing runs unconditionally once the
    // use ends, and its own `f < 0.1` guard below just skips the shot itself.
    player.actions.stopUsing();
    if (pullProgress < kBowMinimumPullProgress) {
        return;
    }
    const bool creative = player.gameMode == GameMode::Creative;
    // RW-4 — read the firing bow's ranged enchantments once, off the held stack,
    // through the DDC-2-driven RangedEnchantment helpers. Snapshot the stack first
    // because the durability spend below mutates the selected slot.
    const ItemStack bow = player.inventory.selectedStack();
    const bool infinity = infinityKeepsArrow(bow);
    // PlayerEntity#getArrowType: creative always finds a (virtual) arrow;
    // survival needs a real one somewhere in the inventory. RW-4 Infinity does
    // NOT remove that requirement (vanilla still needs one arrow in the quiver to
    // fire) — it only skips the CONSUME below once the shot lands.
    const auto arrowSlot = player.inventory.findFirstArrowSlot();
    if (!creative && !arrowSlot.has_value()) {
        return;
    }
    const float lengthSquared = glm::dot(lookDirection, lookDirection);
    const glm::vec3 direction =
        lengthSquared < 1e-9F ? glm::vec3{0.0F, 0.0F, -1.0F} : glm::normalize(lookDirection);
    const glm::vec3 eye = player.controller.eyePosition();
    const float velocityLength = pullProgress * kBowFullDrawVelocity;
    // RW-1a #8 — store the arrow's BASE damage, NOT the velocity-scaled value.
    // AbstractArrow#onHitEntity derives the applied damage at hit time as
    // `ceil(velocity.length() * baseDamage)`, so a shot that decays over a long
    // arc lands softer. RW-0 baked the launch velocity in here and applied it
    // flat, which meant a full-draw arrow hit for the same damage whether it
    // struck point-blank or after a long, slow fall. The projectile now carries
    // the true base and the tick reads the live speed.
    // RW-4 Power — the bow's Power enchant multiplies the arrow's BASE damage by
    // `1 + 0.25*(level+1)` (RangedEnchantment.hpp), baked in here; the tick still
    // scales it by the live impact speed at the hit (RW-1a #8). No Power = base.
    const float damage = powerArrowBaseDamage(kArrowBaseDamage, bow);
    // AbstractArrow: only a FULLY drawn shot (pullProgress == 1.0F) crits —
    // not merely "a strong pull" — matching `if (f == 1.0F)` exactly.
    const bool critical = pullProgress >= 1.0F;
    // RW-4 Punch/Flame — the extra knockback strength and ignite seconds the
    // struck target takes, resolved off the bow through the DDC-2 helpers. Zero
    // for an unenchanted bow, so the shot reduces to RW-1a's plain arrow.
    const float punch = punchKnockbackStrength(bow);
    const int flame = flameArrowIgniteSeconds(bow);
    const ItemStack arrowPickup{world::Block::Air, 1U, &items::Arrow};
    spawnProjectile(eye, direction * velocityLength, ActorReference::player(), damage, critical,
                    ProjectilePickupState::Pickupable, arrowPickup,
                    kProjectileDefaultInaccuracy, punch, flame);
    // RW-4 Infinity — a survival shot from an Infinity bow consumes no arrow
    // (creative never consumes anyway). Any other survival shot spends one.
    if (!creative && !infinity) {
        static_cast<void>(player.inventory.consumeSlot(*arrowSlot));
    }
    if (player.inventory.damageSelected(1U)) {
        events_.publish(SoundEvent{SoundEventKind::ItemBreak, eye});
    }
}

void GameSession::teleportPlayer(PlayerId playerId, const glm::vec3& feet) {
    auto& player = players_.at(playerId);
    player.controller.setPosition(feet);
    player.physicsPrevious = feet;
    player.physicsCurrent = feet;
    // The renderer's camera reads the published snapshot, not the live
    // controller. Mirror the snapped endpoints into a fresh publish so a
    // teleport that happens between ticks is visible the same frame instead of
    // one tick later.
    const auto current = loadSnapshotBundle();
    auto updated = acquireSnapshotWriteBundle();
    *updated = *current;
    updated->player.physicsPrevious = feet;
    updated->player.physicsCurrent = feet;
    publishSnapshotBundle(updated);
}

void GameSession::setWorldSpawn(const glm::vec3& feet) {
    worldSpawnPosition_ = feet;
}

void GameSession::openContainer(ContainerScreen screen, std::optional<ChestPosition> chest,
                                std::optional<glm::ivec3> furnace) {
    openContainerScreen_ = screen;
    openChest_ = chest;
    openFurnace_ = furnace;
    // Opening any other container ends the enchanting screen's binding, so the
    // tick stops rescanning a table nobody is looking at. The menu's two stacks
    // are NOT cleared here — closeContainerMenu is what hands them back, and it
    // runs on every real close.
    openEnchantingTable_.reset();
}

void GameSession::closeContainer() {
    openContainerScreen_ = ContainerScreen::PlayerInventory;
    openChest_.reset();
    openFurnace_.reset();
    openEnchantingTable_.reset();
    openAnvil_.reset();
}

EnchantingMenu& GameSession::enchantingMenu() { return primaryPlayer().enchanting; }

const EnchantingMenu& GameSession::enchantingMenu() const { return primaryPlayer().enchanting; }

void GameSession::openEnchantingContainer(const world::World& world, glm::ivec3 table) {
    EnchantingMenu& menu = enchantingMenu();
    menu.position = table;
    menu.bookshelfPower = bookshelfPower(world, table);
    // Force a derivation even if the (seed, power, item) triple happens to
    // match the one a previous session at another table left behind.
    menu.derived = false;
    refreshOffers(menu, primaryPlayer().experience.enchantmentSeed());
    openContainerScreen_ = ContainerScreen::EnchantingTable;
    openChest_.reset();
    openFurnace_.reset();
    openEnchantingTable_ = table;
}

void GameSession::refreshEnchantingOffers(const world::World& world) {
    if (!openEnchantingTable_.has_value()) {
        return;
    }
    EnchantingMenu& menu = enchantingMenu();
    menu.bookshelfPower = bookshelfPower(world, *openEnchantingTable_);
    refreshOffers(menu, primaryPlayer().experience.enchantmentSeed());
}

bool GameSession::purchaseEnchantment(int optionIndex) {
    if (openContainerScreen_ != ContainerScreen::EnchantingTable) {
        return false;
    }
    EnchantingMenu& menu = enchantingMenu();
    const bool infiniteMaterials = restoresHeldStack(gameMode());
    const auto outcome = purchase(menu, primaryPlayer().experience, optionIndex,
                                  infiniteMaterials, enchantmentSeedRandom_);
    if (!outcome.applied) {
        return false;
    }
    // The seed changed, so the preview must be re-derived from it before anyone
    // (the snapshot publish this tick included) reads the three offers again.
    refreshOffers(menu, primaryPlayer().experience.enchantmentSeed());
    return true;
}

AnvilMenu& GameSession::anvilMenu() { return primaryPlayer().anvil; }

const AnvilMenu& GameSession::anvilMenu() const { return primaryPlayer().anvil; }

void GameSession::openAnvilContainer(glm::ivec3 anvil) {
    anvilMenu().position = anvil;
    refreshAnvilResult();
    openContainerScreen_ = ContainerScreen::Anvil;
    openChest_.reset();
    openFurnace_.reset();
    openEnchantingTable_.reset();
    openAnvil_ = anvil;
}

void GameSession::setAnvilName(std::string name) {
    anvilMenu().name = std::move(name);
    refreshAnvilResult();
}

void GameSession::refreshAnvilResult() {
    // Qualified: the member and the free function share a name on purpose
    // (this is the session's wrapper around Anvil.hpp's pure one), and
    // unqualified lookup would find the member and recurse.
    ::mc::gameplay::refreshAnvilResult(anvilMenu(), restoresHeldStack(gameMode()));
}

bool GameSession::takeAnvilResult(bool shiftHeld) {
    if (openContainerScreen_ != ContainerScreen::Anvil) {
        return false;
    }
    const bool infiniteMaterials = restoresHeldStack(gameMode());
    // Vanilla's result slot refuses the click outright when the cursor is
    // holding something the result cannot join — checked BEFORE anything is
    // spent, so a refused take costs no levels and consumes no inputs.
    if (!shiftHeld && !inventory().cursorStack().empty()) {
        return false;
    }
    ItemStack taken;
    if (!::mc::gameplay::takeAnvilResult(anvilMenu(), primaryPlayer().experience,
                                        infiniteMaterials, taken)
             .applied) {
        return false;
    }
    // Shift-click sends the result to the inventory, a plain click to the
    // cursor — the same two destinations every other result slot uses. What the
    // inventory cannot take is dropped rather than deleted.
    if (!shiftHeld) {
        static_cast<void>(inventory().mergeIntoCursor(taken));
    } else if (!inventory().add(taken) && !taken.empty()) {
        spawnItemDrop(primaryPlayer().playerInput.lookDirection, taken);
    }
    return true;
}

bool GameSession::openChestContainer(ChestPosition position) {
    if (!chestSystem_.open(position)) {
        return false;
    }
    openContainer(ContainerScreen::Chest, position);
    return true;
}

void GameSession::closeContainerMenu() {
    auto& inventory = primaryPlayer().inventory;
    inventory.stowCursorStack();
    primaryPlayer().crafting.stowAll(inventory);
    // EnchantmentMenu#removed -> clearContainer: the two input slots go back to
    // the player. Unconditional, like the crafting grid above — a menu that was
    // never opened is empty, and a table that was mined while its screen was
    // open must still not eat the item.
    for (ItemStack* slot : {&primaryPlayer().enchanting.item, &primaryPlayer().enchanting.lapis,
                           &primaryPlayer().anvil.left, &primaryPlayer().anvil.right}) {
        if (!inventory.add(*slot) && !slot->empty()) {
            // clearContainer's fallback: what the inventory could not take is
            // dropped in front of the player rather than deleted.
            spawnItemDrop(primaryPlayer().playerInput.lookDirection, *slot);
        }
    }
    primaryPlayer().enchanting = {};
    primaryPlayer().anvil = {};
    if (openChest_.has_value()) {
        chestSystem_.close(*openChest_);
    }
    closeContainer();
}

void GameSession::resetWorldState() {
    // The crafting grid is per-player (each player carries one); a world switch
    // empties every player's grid, not just the primary's.
    for (auto& [playerId, player] : players_) {
        player.crafting = {};
        player.enchanting = {};
        player.anvil = {};
    }
    // I-3 的自定义名字表**不**在这里清。
    // 它是会话内的 intern 表，而一次存档解析（SaveRepository::load）会在
    // readStackRecord 里把名字 intern 进去、只在 ItemStack 上留下 id。
    // 换世界的调用顺序是「先解析存档、再 startWorld/resetWorldState」，
    // 在这里清等于把刚读出来的 id 全部清成悬空值，铁砧命名过的物品因此
    // 一进存档就变回原名。清空的职责归解析侧（load/createWorld 各清一次），
    // 也就是「谁 intern，谁负责重置」。
    worldSimulation_ = {};
    primaryLevel().items = {};
    primaryLevel().experienceOrbs = {};
    primaryLevel().entities.clear();
    closeContainer();
    chestSystem_ = {};
    trappedChestSystem_ = {};
    furnaceSystem_ = {};
    // Drop the previous world's published state. Readers that pinned it may
    // finish normally; the fresh pool starts with one empty immutable bundle.
    snapshotPool_.clear();
    auto initialSnapshots = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(initialSnapshots);
    storeSnapshotBundle(
        std::shared_ptr<const RenderSnapshots>{std::move(initialSnapshots)});
}

void GameSession::syncFurnaceLitStates(world::World& world) {
    // Every furnace block entity carries its own burn, and each one smelts
    // whether or not its screen is open, so the lit state is synced per furnace
    // rather than only for the one the player is looking at. The early-out on an
    // unchanged LIT keeps this to a world write only on the ticks a furnace
    // actually ignites or dies.
    for (const auto& furnace : furnaceSystem_.entities()) {
        const auto& position = furnace.position;
        const auto current = world.state(position.x, position.y, position.z);
        if (current.block() != world::Block::Furnace) {
            continue; // the furnace was mined or replaced out from under its entity
        }
        if (current.lit() == furnace.burning()) {
            continue;
        }
        // Lighting a furnace is a state change on the same block, so the cell
        // keeps its facing and its block entity (and thus its smelt). Through
        // the service like every other edit: because the block is unchanged,
        // onBlockEntityReplaced does not fire and the furnace keeps its entity.
        const auto desired = current.withLit(furnace.burning());
        GameplayMutationSink sink{world, *this};
        static_cast<void>(worldMutations_.setBlock(
            world, {position.x, position.y, position.z}, desired,
            world::MutationFlags::All, world::MutationCause::ScheduledTick, sink));
    }
}

void GameSession::createChestBlockEntity(ChestPosition position) {
    static_cast<void>(chestSystem_.place(position));
}

void GameSession::spawnItemEntity(const glm::vec3& position, ItemStack stack,
                                  const glm::vec3& velocity) {
    static_cast<void>(primaryLevel().items.spawn(position, stack, velocity));
}

void GameSession::dropCursorStack(const glm::vec3& lookDirection) {
    spawnItemDrop(lookDirection, primaryPlayer().inventory.takeCursorStack());
}

void GameSession::dropSelectedStack(bool wholeStack, const glm::vec3& lookDirection) {
    spawnItemDrop(lookDirection, primaryPlayer().inventory.takeSelected(wholeStack));
}

void GameSession::spawnItemDrop(const glm::vec3& lookDirection, ItemStack stack) {
    if (stack.empty()) {
        return;
    }
    const float lengthSquared = glm::dot(lookDirection, lookDirection);
    const glm::vec3 direction =
        lengthSquared < 1e-9F ? glm::vec3{0.0F, 0.0F, -1.0F} : glm::normalize(lookDirection);
    const glm::vec3 eye = primaryPlayer().controller.eyePosition();
    spawnItemEntity(eye + direction * 0.45F, stack,
                    direction * 0.28F + glm::vec3{0.0F, 0.12F, 0.0F});
}

void GameSession::attachGameRuleHandlers() {
    // A rule is mirrored into a system only when the system has no business
    // knowing about GameRules and reads the value deep inside a per-tick path:
    // the random-tick speed and the fire radius (WorldSimulation), the four
    // player damage/regeneration rules (PlayerVitals) and mob_drops
    // (EntitySystem). Everything else — advance_time, advance_weather,
    // keep_inventory, block_drops, spawn_mobs, send_command_feedback and the
    // three command limits — is read straight from gameRules_ at its use site,
    // which is one fewer copy to keep honest.
    gameRules_.setChangeHandler(
        [this](GameRuleId id, const GameRuleValueData&) { applyGameRuleMirrors(id); });
    // The mirrors also have to be pushed once up front: a world that loaded a
    // save applied its rules through applyDecoded, which deliberately fires no
    // handler, and a fresh world's systems must still start from the defaults
    // this table declares rather than the ones they each hardcode.
    applyGameRuleMirrors(std::nullopt);
}

void GameSession::applyGameRuleMirrors(std::optional<GameRuleId> changed) {
    const auto touched = [&](GameRuleId id) { return !changed.has_value() || *changed == id; };
    if (touched(GameRuleId::RandomTickSpeed)) {
        worldSimulation_.setRandomTickSpeed(
            gameRules_.get<std::int32_t>(GameRuleId::RandomTickSpeed));
    }
    if (touched(GameRuleId::FireSpreadRadiusAroundPlayer)) {
        worldSimulation_.setFireSpreadRadius(
            gameRules_.get<std::int32_t>(GameRuleId::FireSpreadRadiusAroundPlayer));
    }
    if (touched(GameRuleId::MobDrops)) {
        for (auto& level : levels_) {
            level.entities.setMobDropsEnabled(gameRules_.get<bool>(GameRuleId::MobDrops));
        }
    }
    if (touched(GameRuleId::FallDamage) || touched(GameRuleId::FireDamage) ||
        touched(GameRuleId::DrowningDamage) ||
        touched(GameRuleId::NaturalHealthRegeneration)) {
        const VitalsRules rules{
            gameRules_.get<bool>(GameRuleId::FallDamage),
            gameRules_.get<bool>(GameRuleId::FireDamage),
            gameRules_.get<bool>(GameRuleId::DrowningDamage),
            gameRules_.get<bool>(GameRuleId::NaturalHealthRegeneration)};
        for (auto& entry : players_) {
            entry.second.vitals.setRules(rules);
        }
    }
}


bool GameSession::damageHeldTool(PlayerId playerId, ToolUse use, float blockHardness) {
    auto& player = players_.at(playerId);
    const ItemStack& held = player.inventory.selectedStack();
    const auto baseCost = toolDurabilityCost(held, use, blockHardness);
    if (baseCost == 0U) {
        return false;
    }
    // ENCH-1b Unbreaking: each of the base cost's durability points is
    // probabilistically preserved (level/(level+1)) through the DDC-2 effect
    // engine's random_chance, drawn off this session's deterministic
    // toolDamageRandom_ stream. No Unbreaking ⇒ unbreakingDurabilityCost returns
    // baseCost unchanged (identity), so an unenchanted tool wears exactly as before.
    const auto cost = unbreakingDurabilityCost(
        baseCost, enchantmentLevel(held, EnchantmentId::Unbreaking), toolDamageRandom_);
    return cost > 0U && player.inventory.damageSelected(cost);
}

void GameSession::spawnBlockDrops(glm::ivec3 position, world::BlockState removed,
                                  const ItemStack& tool) {
    // Block#dropResources reads block_drops before it rolls anything, and this is
    // the one funnel every broken block reaches — the mined ones through
    // GameplayMutationSink, the simulated ones (an unsupported torch, a decayed
    // leaf, a washed-away decoration) through the world-simulation loop above.
    // Skipping the roll rather than discarding its result keeps the loot stream
    // from advancing on a break that produced nothing.
    if (!gameRules_.get<bool>(GameRuleId::BlockDrops)) {
        return;
    }
    // The whole state arrives, so the loot table can roll against the stage a
    // crop had grown to rather than against the bare block.
    const auto drops =
        minedDrops(removed.block(), tool, lootRandomState_, removed.age(),
                   world::isSlab(removed.block()) &&
                       removed.slabPortion() == world::SlabPortion::Double);
    std::size_t dropIndex = 0U;
    for (const auto& stack : drops.view()) {
        const float angle = static_cast<float>(dropIndex) * 2.39996323F;
        const glm::vec3 velocity = drops.count > 1U
            ? glm::vec3{std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F}
            : glm::vec3{0.0F, 0.12F, 0.0F};
        primaryLevel().items.spawn(glm::vec3{position} + glm::vec3{0.5F}, stack, velocity);
        ++dropIndex;
    }
}

void GameSession::onPlayerDeath(PlayerId playerId) {
    // Vanilla scatters the whole inventory AND drops experience at the death
    // position unless the keepInventory gamerule keeps both on the respawned
    // player (XP-4 / XP-experience/REGULAR.md #8: "保经验与保物品同一开关" —
    // one gamerule gates both, matching PlayerEntity#die's
    // `!level.getGameRules().getBoolean(GameRules.RULE_KEEPINVENTORY)` guard
    // around both the inventory drop and the `dropExperience()` call).
    if (gameRules_.get<bool>(GameRuleId::KeepInventory)) {
        return;
    }
    auto& player = players_.at(playerId);
    // PlayerEntity#dropExperience: min(7*level, 100), vanilla's cap so a
    // high-level survival death cannot dump an unbounded pile of orbs. XP-0's
    // `level()` is the integer truth this reads, and `giveExperienceLevels`
    // with a very negative amount is how vanilla's own zeroing (progress AND
    // total together) is expressed, rather than hand-rolling a separate
    // "clear" method that could drift out of step with XP-0's own floor logic.
    const std::int32_t droppedExperience =
        std::min(7 * player.experience.level(), 100);
    if (droppedExperience > 0) {
        spawnExperienceOrbs(player.controller.position(), droppedExperience);
    }
    player.experience.giveExperienceLevels(std::numeric_limits<std::int32_t>::min());
    const glm::vec3 dropOrigin = player.controller.position() + glm::vec3{0.0F, 0.9F, 0.0F};
    std::size_t dropIndex = 0U;
    const auto scatter = [&](const ItemStack& stack) {
        const float angle = static_cast<float>(dropIndex++) * 2.39996323F;
        primaryLevel().items.spawn(dropOrigin, stack,
                            {std::cos(angle) * 0.12F, 0.18F, std::sin(angle) * 0.12F});
    };
    // The cursor stack drops first; then the crafting grid stows into the
    // inventory and its contents scatter with everything else.
    if (!player.inventory.cursorStack().empty()) {
        scatter(player.inventory.takeCursorStack());
    }
    player.crafting.stowAll(player.inventory);
    for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
        const auto stack = player.inventory.slot(index);
        if (stack.empty()) {
            continue;
        }
        scatter(stack);
    }
    player.inventory.restore({}, player.inventory.selectedHotbarSlot());

    // EQ-1: LivingEntity#dropEquipment scatters the four armor slots + offhand
    // too, gated by the exact same keepInventory early-out above — this is
    // the "keepInventory keeps equipment too" acceptance case, reached simply
    // by never getting here when the rule is on. Cleared after scattering so
    // a dead (and about-to-respawn) player is not still "wearing" what just
    // hit the ground.
    for (std::size_t index = 0; index < kEquipmentSlotCount; ++index) {
        const auto slot = static_cast<EquipmentSlot>(index);
        const auto& worn = player.equipment.get(slot);
        if (!worn.empty()) {
            scatter(worn);
        }
    }
    player.equipment = EquipmentSlots{};
}

void GameSession::tickEating(SimulationHost& host) {
    if (!primaryPlayer().eating) {
        return;
    }
    ++primaryPlayer().eatTicks;
    // LivingEntity#shouldSpawnConsumptionEffects: once the eat is past its
    // first seven ticks, the chew sound (generic.eat) fires every fourth tick.
    // `remaining > 0` keeps the final tick's burst below from double-firing.
    const int remaining = kEatTicks - primaryPlayer().eatTicks;
    if (remaining > 0 && remaining % 4 == 0 && remaining <= kEatTicks - 7) {
        events_.publish(SoundEvent{SoundEventKind::Eat, primaryPlayer().controller.position()});
    }
    if (primaryPlayer().eatTicks < kEatTicks) {
        return;
    }
    // The meal lands only if the same food/drink is still in hand.
    if (primaryPlayer().inventory.selectedStack().item != primaryPlayer().eatingKind) {
        cancelEating(kPrimaryPlayerId, host);
        return;
    }
    // MilkBucketItem#finishUsing (26.1): drinking milk clears every active
    // status effect and reverts to an empty Bucket — it never touches hunger,
    // so this is a separate finish path from the ordinary food branch below,
    // not an extra case bolted onto it. Creative keeps its milk bucket
    // (restoresHeldStack), same as the bucket-fill branches in performUse.
    if (isDrinkable(primaryPlayer().eatingKind)) {
        static_cast<void>(primaryPlayer().vitals.clearEffects());
        if (!restoresHeldStack(primaryPlayer().gameMode)) {
            primaryPlayer().inventory.replaceSelected({world::Block::Air, 1U, &items::Bucket});
        }
        // MilkBucketItem has no consumeItem burst/burp — the drink itself is
        // silent past its own use-sound cadence above.
        cancelEating(kPrimaryPlayerId, host);
        return;
    }
    // Creative players run the full meal but neither gain hunger nor spend the
    // food, exactly like vanilla (creative never consumes food).
    if (primaryPlayer().gameMode != GameMode::Creative) {
        const auto food = foodValue(primaryPlayer().eatingKind);
        primaryPlayer().vitals.eat(food.foodLevel, food.saturationModifier);
        static_cast<void>(primaryPlayer().inventory.consumeSelected());
    }
    // consumeItem's burst eat sound, then PlayerEntity.eatFood's burp.
    events_.publish(SoundEvent{SoundEventKind::Eat, primaryPlayer().controller.position()});
    events_.publish(SoundEvent{SoundEventKind::Burp, primaryPlayer().controller.position()});
    cancelEating(kPrimaryPlayerId, host);
}

void GameSession::tickPlayerVitals(SimulationHost& host, const world::World& world,
                                   const glm::vec3& previousPosition, bool jumped) {
    if (primaryPlayer().gameMode != GameMode::Survival || primaryPlayer().vitals.dead()) {
        return;
    }
    const glm::vec3 delta = primaryPlayer().controller.position() - previousPosition;
    VitalsInput input;
    input.horizontalDistance = glm::length(glm::vec2{delta.x, delta.z});
    input.verticalDistance = delta.y;
    input.onGround = primaryPlayer().controller.onGround();
    input.sprinting = primaryPlayer().controller.sprinting();
    input.jumped = jumped;
    input.inWater = primaryPlayer().controller.inWater();
    // The camera only catches up after the physics loop, so sample the player's
    // own eye instead of the interpolated render position.
    input.headInWater = submergedInWater(world, primaryPlayer().controller.eyePosition());
    input.flying = primaryPlayer().controller.flying();
    input.feetY = primaryPlayer().controller.position().y;
    const auto result = primaryPlayer().vitals.tick(input);
    if (result.damageTaken > 0.0F) {
        if (result.cause == DamageType::Fall) {
            events_.publish(SoundEvent{SoundEventKind::PlayerFall, primaryPlayer().controller.position(),
                                      world::Block::Air, nullptr, 1.0F,
                                      result.damageTaken > 4.0F});
        } else {
            events_.publish(SoundEvent{SoundEventKind::PlayerHurt, primaryPlayer().controller.position()});
        }
    }
    if (result.died) {
        die(kPrimaryPlayerId, result.cause, host);
    }
}

void GameSession::updateMovementAudio(const world::World& world,
                                      const glm::vec3& previousPosition,
                                      const glm::vec3& currentPosition) {
    if (!primaryPlayer().previousInWater && primaryPlayer().controller.inWater()) {
        events_.publish(
            SoundEvent{SoundEventKind::Splash, currentPosition, world::Block::Air, nullptr, 0.65F});
    }
    primaryPlayer().previousInWater = primaryPlayer().controller.inWater();
    const glm::vec2 movement{currentPosition.x - previousPosition.x,
                             currentPosition.z - previousPosition.z};
    if (!primaryPlayer().controller.onGround() || glm::length(movement) < 0.0001F) {
        return;
    }
    // Entity#move accumulates 0.6 units of step distance per block travelled
    // and plays a sound whenever that accumulator crosses the next integer.
    // Keeping the multiplier here (rather than inventing separate walk/sprint
    // strides) makes sprint cadence rise naturally with its real movement
    // speed while a normal walk stays at the vanilla rhythm.
    primaryPlayer().footstepDistance += glm::length(movement) * 0.6F;
    constexpr float kStepSoundDistance = 1.0F;
    if (primaryPlayer().footstepDistance < kStepSoundDistance) {
        return;
    }
    primaryPlayer().footstepDistance = std::fmod(primaryPlayer().footstepDistance, kStepSoundDistance);
    const int blockX = static_cast<int>(std::floor(currentPosition.x));
    const int blockY = static_cast<int>(std::floor(currentPosition.y - 0.05F));
    const int blockZ = static_cast<int>(std::floor(currentPosition.z));
    const auto groundBlock = world.block(blockX, blockY, blockZ);
    if (world::isRenderable(groundBlock)) {
        events_.publish(SoundEvent{SoundEventKind::Footstep, currentPosition, groundBlock,
                                  nullptr, primaryPlayer().controller.sneaking() ? 0.18F : 0.5F});
    }
}

void GameSession::consumeEntityEvents() {
    for (const auto& sound : primaryLevel().entities.pendingSounds()) {
        // The species rides along so the host plays the right clip for the
        // right creature — a cow's hurt is not a zombie's hurt.
        const auto& type = *sound.type;
        switch (sound.event) {
        case MobSoundEvent::Hurt:
            events_.publish(SoundEvent{SoundEventKind::CreatureHurt, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Death:
            events_.publish(SoundEvent{SoundEventKind::CreatureDeath, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Ambient:
            events_.publish(SoundEvent{SoundEventKind::CreatureAmbient, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Step:
            events_.publish(SoundEvent{SoundEventKind::CreatureStep, sound.position,
                                      world::Block::Air, &type});
            break;
        }
    }
    primaryLevel().entities.clearPendingSounds();
    // A dead mob drops its loot whatever the player's game mode is — vanilla's
    // LivingEntity loot is rolled at death, never gated on the killer's mode.
    for (const auto& [position, drops] : primaryLevel().entities.pendingDrops()) {
        std::size_t dropIndex = 0U;
        for (const auto& stack : drops.view()) {
            const float angle = static_cast<float>(dropIndex) * 2.39996323F;
            primaryLevel().items.spawn(position, stack,
                                {std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F});
            ++dropIndex;
        }
    }
    primaryLevel().entities.clearPendingDrops();
    // XP-2: a gated kill (die()'s lastHurtByPlayer check) or a successful
    // breed (processBreeding) queued its points here; each becomes real orbs
    // through the one spawnExperienceOrbs entry point XP-1 built, so the
    // denomination split and the scatter velocity stay on the session's own
    // deterministic JavaRandom stream regardless of which source paid it.
    for (const auto& [position, amount] : primaryLevel().entities.pendingExperience()) {
        spawnExperienceOrbs(position, amount);
    }
    primaryLevel().entities.clearPendingExperience();
}

bool GameSession::submergedInWater(const world::World& world, glm::vec3 position) const {
    return world.block(static_cast<int>(std::floor(position.x)),
                       static_cast<int>(std::floor(position.y)),
                       static_cast<int>(std::floor(position.z))) == world::Block::Water;
}

} // namespace mc::gameplay
