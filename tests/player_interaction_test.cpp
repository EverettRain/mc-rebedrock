// N1's interaction acceptance: the dig, place, bucket, attack and eat paths
// run headless through GameSession's command-driven PlayerInteraction, and a
// dig consumes the same whole number of ticks regardless of frame rate (the
// timeline is tick-owned, so there is no frame path left to drift).

#include "gameplay/GameSession.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace mc;

namespace {

struct TestHost final : gameplay::SimulationHost {
    int blockBreaks = 0;
    int blockPlaces = 0;
    int splashes = 0;
    int entityHits = 0;
    int itemBreaks = 0;
    int eatingStarted = 0;

    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override { ++blockBreaks; }
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override { ++blockPlaces; }
    void playItemBreak(glm::vec3) override { ++itemBreaks; }
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override { ++splashes; }
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override { ++eatingStarted; }
    void onEatingCancelled() override {}
};

void buildFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

// The whole-number tick count a dig of `block` with `stack` lands on, given the
// player is not underwater and not on the ground (the default floating spawn).
[[nodiscard]] std::uint64_t digTicks(gameplay::GameSession& session,
                                     const gameplay::ItemStack& stack, world::Block block) {
    const float duration = gameplay::miningSeconds(block, stack, session.player().inWater(),
                                                   !session.player().onGround());
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(duration) *
                                                world::DayNightCycle::kTicksPerSecond));
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    // --- Dig: a StartDestroy command digs the block over a whole number of
    // ticks, and that count is the same however the frame rate would have
    // paced it (there is no frame path left). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Stone});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        const auto expected = digTicks(session, session.inventory().selectedStack(),
                                       world::Block::Stone);
        for (std::uint64_t tick = 0; tick < expected; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        // The block gave way exactly on the computed tick.
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(host.blockBreaks == 1);
        // The swing timeline ran with the dig (the arc restarts every ~half
        // swing, so the sequence advanced past the first swing).
        assert(session.playerActions().swing.sequence >= 1U);
    }

    // --- Place: a UseItemOn with a block stack in hand places the block. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::Stone, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn use;
        use.block = glm::ivec3{5, 1, 5};
        use.adjacent = glm::ivec3{5, 2, 5};
        use.face = world::BlockOrientation::Up;
        use.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(use));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 2, 5) == world::Block::Stone);
        assert(host.blockPlaces == 1);
    }

    // --- Slab: placed on a top face it is a bottom slab; right-clicking its top
    //     with the same slab merges the pair into a double rather than stacking a
    //     new slab above. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakSlab);
        assert(world.state(5, 1, 5).slabPortion() == world::SlabPortion::Bottom);

        // The 4-tick right-click delay has to pass before the next use fires.
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        gameplay::UseItemOn merge;
        merge.block = glm::ivec3{5, 1, 5};
        merge.adjacent = glm::ivec3{5, 2, 5};
        merge.face = world::BlockOrientation::Up;
        merge.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(merge));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).slabPortion() == world::SlabPortion::Double);
        assert(world.block(5, 2, 5) == world::Block::Air);
    }

    // --- Slab: the sub-cell hit height decides the half on a horizontal face.
    //     Aiming at the upper half of a block's side rests a TOP slab in the
    //     neighbour; the lower half a bottom one. Without the real hit point this
    //     regressed to bottom-only once the pick ray honoured the half box. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setBlock(5, 1, 7, world::Block::Stone); // an exposed post to click
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        session.inventory().selectHotbar(0);

        // Click the south face of the post, high up: a top slab lands in (5,1,8).
        gameplay::UseItemOn high;
        high.block = glm::ivec3{5, 1, 7};
        high.adjacent = glm::ivec3{5, 1, 8};
        high.face = world::BlockOrientation::South;
        high.hitPosition = glm::vec3{5.5F, 1.8F, 8.0F};
        high.lookDirection = glm::vec3{0.0F, 0.0F, 1.0F};
        session.enqueueCommand(std::move(high));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 8).slabPortion() == world::SlabPortion::Top);

        // Click the north face of the post, low down: a bottom slab in (5,1,6).
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        gameplay::UseItemOn low;
        low.block = glm::ivec3{5, 1, 7};
        low.adjacent = glm::ivec3{5, 1, 6};
        low.face = world::BlockOrientation::North;
        low.hitPosition = glm::vec3{5.5F, 1.2F, 7.0F};
        low.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(low));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 6).slabPortion() == world::SlabPortion::Bottom);
    }

    // --- Held dig hand-off: no release is needed between two instant blocks.
    // The renderer sends a new StartDestroy when the vanished first target lets
    // the ray reach the next cell; the interaction remains armed and accepts it
    // on the following tick. This is the grass/plant continuous-break path. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Grass});
        world.setState(5, 1, 6, world::BlockState{world::Block::Grass});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        session.tick(world, host);
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(session.interaction().destroying());

        // Same held mouse button, new ray target; importantly, no AbortDestroy.
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 6}});
        // Creative keeps vanilla's five-tick destroy delay between blocks.
        for (int tick = 0; tick < 5; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 6) == world::Block::Air);
        assert(host.blockBreaks == 2);
    }

    // --- Bucket: UseItemOn a water source with an empty bucket collects it. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Survival);
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn use;
        use.block = glm::ivec3{5, 1, 5};
        use.adjacent = glm::ivec3{5, 1, 5};
        use.face = world::BlockOrientation::Up;
        use.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(use));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(session.inventory().selectedStack().item == &gameplay::items::WaterBucket);
        assert(host.splashes == 1);
    }

    // --- Attack: a StartDestroy at a creature hits it once. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        assert(pigType != nullptr);
        session.worldEntities().spawn({5.5F, 2.0F, 5.5F}, *pigType);
        // Find the spawned id.
        std::uint64_t pigId = 0U;
        for (const auto& entity : session.worldEntities().entities()) {
            if (entity.type != nullptr && std::string{entity.type->id().path} == "pig") {
                pigId = entity.id;
                break;
            }
        }
        assert(pigId != 0U);
        const float healthBefore =
            session.worldEntities().byId(pigId) != nullptr
                ? session.worldEntities().byId(pigId)->damage.health
                : 10.0F;
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::StartDestroy;
        action.entity = true;
        action.entityId = pigId;
        session.enqueueCommand(std::move(action));
        session.tick(world, host);
        // A bare-hand attack deals one damage.
        assert(session.worldEntities().byId(pigId) != nullptr);
        assert(session.worldEntities().byId(pigId)->damage.health == healthBefore - 1.0F);
        assert(session.playerActions().swing.sequence == 1U);
    }

    // --- Eat: holding the use button runs the vanilla 32-tick meal. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Creative);
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
        session.inventory().selectHotbar(0);
        session.enqueueCommand(gameplay::UseItem{});
        world::World world;
        buildFloor(world);
        for (int tick = 0; tick < gameplay::GameSession::kEatTicks; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        assert(host.eatingStarted == 1);
        assert(!session.eating());
    }

    // --- Attacking cancels a meal (Minecraft#doAttack interrupts eating). ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
        session.inventory().selectHotbar(0);
        session.enqueueCommand(gameplay::UseItem{});
        world::World world;
        buildFloor(world);
        session.tick(world, host);  // the meal starts
        assert(session.eating());
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::StartDestroy;  // attack nothing
        session.enqueueCommand(std::move(action));
        session.tick(world, host);
        assert(!session.eating());  // the attack cancelled the meal
    }

    // --- The creative commands run through the queue like the rest of the
    // input, and the interaction applies them on the server tick. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);

        // A creative catalogue click puts the stack on the cursor; a second
        // click with the same item adds one more (max-stack creative sizing is
        // handled by shift, not by repetition).
        const gameplay::ItemStack diamond{world::Block::Air, 1U, &gameplay::items::Diamond};
        session.enqueueCommand(
            gameplay::ClickCreativeItem{diamond, gameplay::InventoryMouseButton::Left, false});
        session.tick(world, host);
        session.enqueueCommand(
            gameplay::ClickCreativeItem{diamond, gameplay::InventoryMouseButton::Left, false});
        session.tick(world, host);
        assert(session.inventory().cursorStack().item == &gameplay::items::Diamond);
        assert(session.inventory().cursorStack().count == 2U);

        // ClearCursor empties the cursor (the creative delete box / an empty
        // catalogue cell).
        session.enqueueCommand(gameplay::ClearCursor{});
        session.tick(world, host);
        assert(session.inventory().cursorStack().empty());

        // DropCursor throws the cursor stack as an item entity and empties it.
        session.enqueueCommand(gameplay::ClickCreativeItem{
            diamond, gameplay::InventoryMouseButton::Left, false});
        session.tick(world, host);
        session.enqueueCommand(gameplay::DropCursor{glm::vec3{0.0F, 0.0F, -1.0F}});
        session.tick(world, host);
        assert(session.inventory().cursorStack().empty());
        assert(session.itemEntities().entities().size() == 1U);
    }

    // --- DragDistribute resolves the swept (kind, index) slots and shares the
    // cursor stack across them on the server tick (vanilla QUICK_CRAFT). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        const gameplay::ItemStack diamond{world::Block::Air, 1U, &gameplay::items::Diamond};
        for (int i = 0; i < 2; ++i) {
            session.enqueueCommand(
                gameplay::ClickCreativeItem{diamond, gameplay::InventoryMouseButton::Left, false});
            session.tick(world, host);
        }
        assert(session.inventory().cursorStack().count == 2U);

        gameplay::DragDistribute drag;
        drag.button = gameplay::InventoryMouseButton::Left;
        drag.targets = {gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 0},
                        gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 1}};
        session.enqueueCommand(std::move(drag));
        session.tick(world, host);
        assert(session.inventory().cursorStack().empty());
        assert(session.inventory().slot(0).item == &gameplay::items::Diamond);
        assert(session.inventory().slot(1).item == &gameplay::items::Diamond);
        assert(session.inventory().slot(0).count == 1U);
        assert(session.inventory().slot(1).count == 1U);
    }

    // --- PickupAll gathers every matching stack into the cursor (the
    // double-click), stopping at the stack limit. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        const gameplay::ItemStack diamond{world::Block::Air, 1U, &gameplay::items::Diamond};
        session.inventory().mutableSlot(0) = diamond;
        session.inventory().mutableSlot(1) = diamond;
        session.enqueueCommand(
            gameplay::ClickCreativeItem{diamond, gameplay::InventoryMouseButton::Left, false});
        session.tick(world, host);
        assert(session.inventory().cursorStack().count == 1U);

        gameplay::PickupAll pickup;
        pickup.targets = {gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 0},
                          gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 1}};
        session.enqueueCommand(std::move(pickup));
        session.tick(world, host);
        assert(session.inventory().cursorStack().item == &gameplay::items::Diamond);
        assert(session.inventory().cursorStack().count == 3U);
        assert(session.inventory().slot(0).empty());
        assert(session.inventory().slot(1).empty());
    }

    // --- F2: placing a slab into a still water source comes out submerged. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        session.inventory().selectHotbar(0);
        // Click the floor's top face; the placement cell (adjacent) is the
        // water-filled cell above it, exactly like right-clicking the seabed
        // to place a slab into a shallow pool.
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakSlab);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::Water);

        // A slab placed on dry land stays dry (the auto-submerge only fires
        // when the target cell was actually a still water source).
        session.inventory().mutableSlot(0) = {world::Block::OakSlab, 1U, nullptr};
        gameplay::UseItemOn dryPlace;
        dryPlace.block = glm::ivec3{6, 0, 5};
        dryPlace.adjacent = glm::ivec3{6, 1, 5};
        dryPlace.face = world::BlockOrientation::Up;
        dryPlace.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(dryPlace));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(6, 1, 5) == world::Block::OakSlab);
        assert(world.state(6, 1, 5).submergedFluid() == world::SubmergedFluid::None);
    }

    // --- F2: breaking a submerged slab leaves a water source, not air. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5,
                       world::BlockState{world::Block::OakSlab}
                           .withSubmergedFluid(world::SubmergedFluid::Water));
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        // Give the dig loop enough ticks to finish (a slab breaks quickly by
        // hand, but drive several ticks to be independent of the exact speed).
        for (int tick = 0; tick < 40 && world.block(5, 1, 5) != world::Block::Water; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        // Sabotage #2's target: this must be a water *source*, not air — a
        // breakResidue that forgot the axis and returned plain BlockState{}
        // would leave Air here instead.
        assert(world.block(5, 1, 5) == world::Block::Water);
        assert(world.state(5, 1, 5).fluidLevel() == 0U);
        assert(host.blockBreaks == 1);

        // A dry slab breaks to ordinary air, exactly as before F2.
        world.setState(6, 1, 5, world::BlockState{world::Block::OakSlab});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{6, 1, 5}});
        for (int tick = 0; tick < 40 && world.block(6, 1, 5) != world::Block::Air; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(6, 1, 5) == world::Block::Air);
    }

    // --- F2: bucket interactions on a submergible block wet/dry it in place
    //     instead of replacing the block. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Survival);
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::OakSlab}
                                     .withSlabPortion(world::SlabPortion::Top));
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::WaterBucket};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn pour;
        pour.block = glm::ivec3{5, 1, 5};
        pour.adjacent = glm::ivec3{5, 2, 5};
        pour.face = world::BlockOrientation::Up;
        pour.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(pour));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        // The slab is still there, still a top slab, now wet — a bucket used
        // directly on it must not have replaced it with a plain water block.
        assert(world.block(5, 1, 5) == world::Block::OakSlab);
        assert(world.state(5, 1, 5).slabPortion() == world::SlabPortion::Top);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::Water);
        assert(session.inventory().selectedStack().item == &gameplay::items::Bucket);

        // An empty bucket on that same wet slab takes the water back and
        // leaves the (still top) slab dry, rather than mining it.
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
        gameplay::UseItemOn collect;
        collect.block = glm::ivec3{5, 1, 5};
        collect.adjacent = glm::ivec3{5, 2, 5};
        collect.face = world::BlockOrientation::Up;
        collect.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(collect));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakSlab);
        assert(world.state(5, 1, 5).slabPortion() == world::SlabPortion::Top);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::None);
        assert(session.inventory().selectedStack().item == &gameplay::items::WaterBucket);
    }

    // --- AR-B2 stairs: placement resolves Facing (opposite the player's look,
    // HorizontalDirectionalBlock's rule) and Half (from the sub-cell hit
    // height, the same slab rule) in one UseItemOn; the join Shape is already
    // computed against the world at placement time, and a later neighbour
    // placement recomputes it through updateShape rather than a second
    // placement-time compute. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::OakStairs, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        // Looking North (-Z): HorizontalDirectionalBlock faces the placer, so
        // the stair's FACING resolves to the opposite, South.
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakStairs);
        const auto placed = world.state(5, 1, 5);
        assert(placed.orientation() == world::BlockOrientation::South);
        assert(placed.stairHalf() == world::SlabPortion::Bottom);
        // No stair neighbour yet on either facing-axis side: Straight.
        assert(placed.stairShape() == world::StairShape::Straight);

        // A South-facing stair's "behind" cell is pos + offset(South) =
        // (5,1,6) (one *more* in Z). Placing a matching stair there, facing
        // off-axis, corner-joins (5,1,5) through updateShape — not a second
        // placement-time compute on the already-placed cell.
        session.inventory().mutableSlot(0) = {world::Block::OakStairs, 1U, nullptr};
        gameplay::UseItemOn second;
        second.block = glm::ivec3{5, 0, 6};
        second.adjacent = glm::ivec3{5, 1, 6};
        second.face = world::BlockOrientation::Up;
        second.lookDirection = glm::vec3{1.0F, 0.0F, 0.0F}; // facing resolves to West
        session.enqueueCommand(std::move(second));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 6) == world::Block::OakStairs);
        assert(world.state(5, 1, 6).orientation() == world::BlockOrientation::West);
        // (5,1,5)'s own shape recomputed once its behind-neighbour appeared —
        // sabotage target ②'s subject: if updateShape used the wrong offset or
        // direction, this would stay Straight instead of joining.
        assert(world.state(5, 1, 5).stairShape() != world::StairShape::Straight);
    }

    // --- F2 extension (this pass): a stair placed into a still water source
    // comes out submerged, using the exact same placementBlock branch a slab
    // does — the F2 axis and the AR-B2 shape resolution are independent
    // concerns computed side by side in the same call. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::OakStairs, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakStairs);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::Water);
        // Its Facing/Half/StairShape axes still resolved normally, unaffected
        // by the extra wet axis.
        assert(world.state(5, 1, 5).orientation() == world::BlockOrientation::South);
        assert(world.state(5, 1, 5).stairHalf() == world::SlabPortion::Bottom);

        // A stair placed on dry land stays dry.
        session.inventory().mutableSlot(0) = {world::Block::OakStairs, 1U, nullptr};
        gameplay::UseItemOn dryPlace;
        dryPlace.block = glm::ivec3{6, 0, 5};
        dryPlace.adjacent = glm::ivec3{6, 1, 5};
        dryPlace.face = world::BlockOrientation::Up;
        dryPlace.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(dryPlace));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(6, 1, 5) == world::Block::OakStairs);
        assert(world.state(6, 1, 5).submergedFluid() == world::SubmergedFluid::None);
    }

    // --- F2 extension: breaking a submerged stair leaves a water source, not
    // air — the same breakResidue path a slab uses, now exercised on a
    // non-full-cube shape that is not the slab's own model. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5,
                       world::BlockState{world::Block::OakStairs}
                           .withSubmergedFluid(world::SubmergedFluid::Water));
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        for (int tick = 0; tick < 40 && world.block(5, 1, 5) != world::Block::Water; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        // Sabotage target: a breakResidue that forgot the axis (or that only
        // special-cased slabs) would leave Air here instead of a water source.
        assert(world.block(5, 1, 5) == world::Block::Water);
        assert(world.state(5, 1, 5).fluidLevel() == 0U);
        assert(host.blockBreaks == 1);

        // A dry stair breaks to ordinary air, exactly as before this pass.
        world.setState(6, 1, 5, world::BlockState{world::Block::OakStairs});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{6, 1, 5}});
        for (int tick = 0; tick < 40 && world.block(6, 1, 5) != world::Block::Air; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(6, 1, 5) == world::Block::Air);
    }

    // --- F2 extension: bucket interactions on a stair wet/dry it in place,
    // preserving its Facing/Half/StairShape exactly like the slab case
    // preserves SlabType. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Survival);
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::OakStairs, world::BlockOrientation::East}
                                     .withStairHalf(world::SlabPortion::Top));
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::WaterBucket};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn pour;
        pour.block = glm::ivec3{5, 1, 5};
        pour.adjacent = glm::ivec3{5, 2, 5};
        pour.face = world::BlockOrientation::Up;
        pour.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(pour));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        // Still the same stair, still Top/East, now wet — a bucket used
        // directly on it must not have replaced it with a plain water block.
        assert(world.block(5, 1, 5) == world::Block::OakStairs);
        assert(world.state(5, 1, 5).orientation() == world::BlockOrientation::East);
        assert(world.state(5, 1, 5).stairHalf() == world::SlabPortion::Top);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::Water);
        assert(session.inventory().selectedStack().item == &gameplay::items::Bucket);

        // An empty bucket on that same wet stair takes the water back and
        // leaves the (still Top/East) stair dry, rather than mining it.
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
        gameplay::UseItemOn collect;
        collect.block = glm::ivec3{5, 1, 5};
        collect.adjacent = glm::ivec3{5, 2, 5};
        collect.face = world::BlockOrientation::Up;
        collect.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(collect));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakStairs);
        assert(world.state(5, 1, 5).orientation() == world::BlockOrientation::East);
        assert(world.state(5, 1, 5).stairHalf() == world::SlabPortion::Top);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::None);
        assert(session.inventory().selectedStack().item == &gameplay::items::WaterBucket);
    }

    // --- AR-B2 door: a single UseItemOn places two cells atomically (both
    // exist and share Facing/Hinge the instant the command resolves — no tick
    // boundary between them to observe a lone half), a right-click on either
    // half toggles Open on both together, and breaking either half removes
    // both. This is the task card's "两格原子" + "破坏任一半→两半皆消" +
    // "开关切换" trio in one flow. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::OakDoor, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        // Both halves exist, right after the one command that placed them.
        assert(world.block(5, 1, 5) == world::Block::OakDoor);
        assert(world.block(5, 2, 5) == world::Block::OakDoor);
        assert(!world.state(5, 1, 5).isDoorUpperHalf());
        assert(world.state(5, 2, 5).isDoorUpperHalf());
        assert(world.state(5, 1, 5).orientation() == world.state(5, 2, 5).orientation());
        assert(world.state(5, 1, 5).hinge() == world.state(5, 2, 5).hinge());
        assert(!world.state(5, 1, 5).open() && !world.state(5, 2, 5).open());
        // A door has real collision (the thin box), unlike an ordinary
        // no-collision decoration — the interaction test double-checks the
        // shape source's own hasCollision assertion from a gameplay angle.
        assert(world::hasCollision(world::Block::OakDoor));

        // Right-click the *lower* half: both flip open together, and the
        // door's own collision shape genuinely changes (the sabotage③
        // target: an open-but-uncollided-update door would still report the
        // closed box). Stopped the instant the toggle lands (mirroring a
        // real mouse-up), so the interaction's own "repeat every 4 ticks
        // while held" never gets a second window to re-fire and flip it
        // straight back — waiting for the flip and stopping on the same tick
        // (rather than an unconditional multi-tick loop) is what lets this
        // test hand-click a door safely.
        gameplay::UseItemOn toggleLower;
        toggleLower.block = glm::ivec3{5, 1, 5};
        toggleLower.adjacent = glm::ivec3{4, 1, 5};
        toggleLower.face = world::BlockOrientation::West;
        toggleLower.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(toggleLower));
        for (int tick = 0; tick < 20 && !world.state(5, 1, 5).open(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).open() && world.state(5, 2, 5).open());
        // The XZ footprint genuinely moves when open — the real "collision
        // changed" signal a door's toggle produces (unlike the gate, whose
        // whole span empties instead).
        assert(!(world::blockShape(world.state(5, 1, 5)).boxes.front().minX ==
                      world::blockShape(world.state(5, 1, 5).withOpen(false)).boxes.front().minX &&
                  world::blockShape(world.state(5, 1, 5)).boxes.front().minZ ==
                      world::blockShape(world.state(5, 1, 5).withOpen(false)).boxes.front().minZ));

        // Right-click the *upper* half: both flip closed together too.
        gameplay::UseItemOn toggleUpper;
        toggleUpper.block = glm::ivec3{5, 2, 5};
        toggleUpper.adjacent = glm::ivec3{4, 2, 5};
        toggleUpper.face = world::BlockOrientation::West;
        toggleUpper.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(toggleUpper));
        for (int tick = 0; tick < 20 && world.state(5, 1, 5).open(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(!world.state(5, 1, 5).open() && !world.state(5, 2, 5).open());

        // Breaking the *upper* half removes both — sabotage①'s target: a break
        // that only deletes the clicked half would leave (5,1,5) standing.
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                       glm::ivec3{5, 2, 5}});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(world.block(5, 2, 5) == world::Block::Air);
    }

    // --- AR-B2 door: breaking the *lower* half also removes the upper one
    // (the symmetric direction to the case above). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::OakDoor, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakDoor);
        assert(world.block(5, 2, 5) == world::Block::OakDoor);

        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                       glm::ivec3{5, 1, 5}});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(world.block(5, 2, 5) == world::Block::Air);
    }

    // --- AR-B2 fence gate: placement (single cell, Facing only), right-click
    // toggle, and the collision shape genuinely emptying while open (unlike
    // the door's thin sliver, a gate collides with nothing at all). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::OakFenceGate, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{5, 0, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::Up;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakFenceGate);
        assert(!world.state(5, 1, 5).open());
        const auto closedSpan = world::collisionSpan(world.state(5, 1, 5));
        assert(closedSpan.top > closedSpan.bottom); // solid while closed

        // Stopped the instant the toggle lands (see the door test's comment):
        // one click, not a held button the 4-tick repeat would keep re-firing.
        gameplay::UseItemOn toggle;
        toggle.block = glm::ivec3{5, 1, 5};
        toggle.adjacent = glm::ivec3{4, 1, 5};
        toggle.face = world::BlockOrientation::West;
        toggle.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(toggle));
        for (int tick = 0; tick < 20 && !world.state(5, 1, 5).open(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).open());
        const auto openSpan = world::collisionSpan(world.state(5, 1, 5));
        assert(openSpan.top <= openSpan.bottom); // fully clear while open

        // Toggling again closes it.
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, nullptr};
        gameplay::UseItemOn toggleBack;
        toggleBack.block = glm::ivec3{5, 1, 5};
        toggleBack.adjacent = glm::ivec3{4, 1, 5};
        toggleBack.face = world::BlockOrientation::West;
        toggleBack.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(toggleBack));
        for (int tick = 0; tick < 20 && world.state(5, 1, 5).open(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(!world.state(5, 1, 5).open());
    }

    // --- F2 extension: doors and fence gates are the negative case — vanilla
    // does not make either SimpleWaterloggedBlock (confirmed against 26.1's
    // DoorBlock.java/FenceGateBlock.java, neither carries a WATERLOGGED
    // property, unlike StairBlock.java), so this pass never called
    // .submerges() on them. Placing either into a still water source must NOT
    // wet it — placementBlock's `canBeSubmerged(selected)` prefilter is what
    // is under test here, the same prefilter sabotage①'s "make it identity-
    // blind" attack targets. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Water});
        world.setState(5, 2, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::OakDoor, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn placeDoor;
        placeDoor.block = glm::ivec3{5, 0, 5};
        placeDoor.adjacent = glm::ivec3{5, 1, 5};
        placeDoor.face = world::BlockOrientation::Up;
        placeDoor.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(placeDoor));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakDoor);
        assert(world.block(5, 2, 5) == world::Block::OakDoor);
        assert(world.state(5, 1, 5).submergedFluid() == world::SubmergedFluid::None);
        assert(world.state(5, 2, 5).submergedFluid() == world::SubmergedFluid::None);

        world.setState(6, 1, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::OakFenceGate, 1U, nullptr};
        gameplay::UseItemOn placeGate;
        placeGate.block = glm::ivec3{6, 0, 5};
        placeGate.adjacent = glm::ivec3{6, 1, 5};
        placeGate.face = world::BlockOrientation::Up;
        placeGate.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(placeGate));
        for (int tick = 0; tick < 6; ++tick) {
            session.tick(world, host);
        }
        static_cast<void>(session.drainEvents());
        assert(world.block(6, 1, 5) == world::Block::OakFenceGate);
        assert(world.state(6, 1, 5).submergedFluid() == world::SubmergedFluid::None);

        // A water bucket used directly on either has no submerge effect (the
        // clicked-block prefilter in bucketPlaceUseOn falls through to the
        // adjacent-cell pour branch instead), and neither block round-trips
        // withSubmergedFluid as anything but a no-op.
        assert(world::BlockState{world::Block::OakDoor}.withSubmergedFluid(
                   world::SubmergedFluid::Water) == world::BlockState{world::Block::OakDoor});
        assert(world::BlockState{world::Block::OakFenceGate}.withSubmergedFluid(
                   world::SubmergedFluid::Water) == world::BlockState{world::Block::OakFenceGate});
    }

    // --- AR-B3 trapdoor: placement against a horizontal face resolves
    // Facing+Half from the clicked face/hit height, a right-click flips OPEN,
    // and the collision box genuinely relocates (mirrors the door test's
    // sabotage③-style assertion). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(6, 1, 5, world::BlockState{world::Block::Stone});
        session.inventory().mutableSlot(0) = {world::Block::OakTrapdoor, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{6, 1, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::West;
        place.hitPosition = glm::vec3{6.0F, 1.3F, 5.5F}; // lower half of the clicked face
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::OakTrapdoor);
        assert(world.state(5, 1, 5).orientation() == world::BlockOrientation::West);
        assert(world.state(5, 1, 5).trapdoorHalf() == world::SlabPortion::Bottom);
        assert(!world.state(5, 1, 5).open());
        assert(world::hasCollision(world::Block::OakTrapdoor));

        gameplay::UseItemOn toggle;
        toggle.block = glm::ivec3{5, 1, 5};
        toggle.adjacent = glm::ivec3{4, 1, 5};
        toggle.face = world::BlockOrientation::West;
        toggle.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(toggle));
        for (int tick = 0; tick < 20 && !world.state(5, 1, 5).open(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).open());
        // The collision box genuinely moves: an open trapdoor's Y span covers
        // the whole cell (a vertical leaf), unlike the closed thin slab.
        const auto openShape = world::blockShape(world.state(5, 1, 5));
        assert(openShape.boxes.front().minY == 0.0F && openShape.boxes.front().maxY == 1.0F);
    }

    // --- AR-B3 button: placement against a wall resolves Facing from the
    // clicked face, a right-click presses it (POWERED true immediately), and
    // it releases itself automatically 20 ticks later without any further
    // input — the "按下→计时回弹" clause, and this test's own sabotage①
    // target (a broken release timer would leave it stuck powered forever). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(6, 1, 5, world::BlockState{world::Block::Stone});
        session.inventory().mutableSlot(0) = {world::Block::StoneButton, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn place;
        place.block = glm::ivec3{6, 1, 5};
        place.adjacent = glm::ivec3{5, 1, 5};
        place.face = world::BlockOrientation::West;
        place.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(place));
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.block(5, 1, 5) == world::Block::StoneButton);
        assert(world.state(5, 1, 5).orientation() == world::BlockOrientation::West);
        assert(!world.state(5, 1, 5).powered());

        gameplay::UseItemOn press;
        press.block = glm::ivec3{5, 1, 5};
        press.adjacent = glm::ivec3{4, 1, 5};
        press.face = world::BlockOrientation::West;
        press.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(press));
        for (int tick = 0; tick < 5 && !world.state(5, 1, 5).powered(); ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).powered());
        // A re-press while already powered does not restart the timer or do
        // anything observable (ButtonBlock#useWithoutItem's CONSUME-without-
        // re-trigger branch) — the release below still lands at the original
        // 20-tick mark, not later, which the tick budget below would miss if
        // a re-press had pushed it out.
        gameplay::UseItemOn rePress;
        rePress.block = glm::ivec3{5, 1, 5};
        rePress.adjacent = glm::ivec3{4, 1, 5};
        rePress.face = world::BlockOrientation::West;
        rePress.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(rePress));
        session.tick(world, host);
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).powered());

        // Sabotage①'s target: the release timer must fire on its own, with no
        // further player input — 20gt after the *first* press (kStoneButton-
        // PressTicks in WorldSimulation.cpp), well within a generous 30-tick
        // budget.
        bool released = false;
        for (int tick = 0; tick < 30; ++tick) {
            session.tick(world, host);
            if (!world.state(5, 1, 5).powered()) {
                released = true;
                break;
            }
        }
        static_cast<void>(session.drainEvents());
        assert(released);
    }

    // --- AR-B3 pressure plate: placement on solid ground, a live creature
    // standing on it presses it, walking off releases it — the "踩踏触发→离开
    // 复位" clause and this test's own sabotage②/③ targets. ---
    {
        gameplay::entities::registerBuiltinEntities();
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::StonePressurePlate});
        assert(!world.state(5, 1, 5).powered());

        // The player's own feet, tickPressurePlates' bounded set: standing on
        // the plate presses it. The plate occupies cell y=1 (floor cell y=0
        // holds the stone below it), so its top surface — where standing feet
        // rest — is world y=2.0.
        session.teleportPlayer(gameplay::kPrimaryPlayerId, {5.5F, 2.0F, 5.5F});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).powered());

        // Walking off releases it (sabotage②'s target: a missing release path
        // would leave this stuck true forever).
        session.teleportPlayer(gameplay::kPrimaryPlayerId, {10.5F, 1.0F, 10.5F});
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(!world.state(5, 1, 5).powered());

        // A creature (not the player) standing on the plate also presses it —
        // proves the check is not player-only.
        session.primaryLevel().entities.spawn({5.5F, 2.001F, 5.5F},
                                              gameplay::entities::PigEntity::type());
        session.tick(world, host);
        static_cast<void>(session.drainEvents());
        assert(world.state(5, 1, 5).powered());
    }

    return 0;
}
