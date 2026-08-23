// EQ-1 interaction half: wear (right-click auto-equip), slot-click with the
// canEquip filter, shift-click quick-equip, and death-drop gated by
// keepInventory — the four behaviours built on top of EQ-0's storage-only
// EquipmentSlots (see equipment_slots_test.cpp for the storage layer's own
// coverage: get/set/save/snapshot). Headless, command-driven — no Vulkan, no
// window; every interaction goes through GameSession::enqueueCommand +
// GameSession::tick exactly like player_interaction_test.cpp.

#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"equipment_wear_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : SimulationHost {
    bool playerDied = false;
    int swings = 0;
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override {}
    void playItemBreak(glm::vec3) override {}
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
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override { playerDied = true; }
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
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

// --- Auto-equip: right-clicking (UseItem, in air) an armor item moves it
// into its matching equipment slot and empties the hand. Sabotage③ target:
// if the auto-equip routing ignores armorSlotOf (e.g. hardcodes Head), a
// chestplate would land in the wrong slot and this assertion aborts. ---
void testAutoEquipFillsMatchingSlot() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 5.0F, 5.5F});
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::IronHelmet};
    session.inventory().selectHotbar(0);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Head).item == &items::IronHelmet);
    REQUIRE(session.inventory().selectedStack().empty());
    std::cout << "testAutoEquipFillsMatchingSlot OK\n";
}

// A chestplate goes to the chest slot, not the head slot — proves the
// routing is armorSlotOf-driven, not a single hardcoded target.
void testAutoEquipRoutesEachSlotIndependently() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 5.0F, 5.5F});
    session.inventory().mutableSlot(0) =
        ItemStack{world::Block::Air, 1U, &items::IronChestplate};
    session.inventory().selectHotbar(0);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Chest).item == &items::IronChestplate);
    REQUIRE(session.equipment().get(EquipmentSlot::Head).empty());
    std::cout << "testAutoEquipRoutesEachSlotIndependently OK\n";
}

// Right-clicking with a full head slot swaps: the worn helmet comes back to
// the hand, the new one goes on — vanilla ArmorItem#use's behaviour whether
// or not the slot started empty.
void testAutoEquipSwapsOccupiedSlot() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 5.0F, 5.5F});
    session.equipment().set(EquipmentSlot::Head,
                            ItemStack{world::Block::Air, 1U, &items::IronHelmet});
    session.inventory().mutableSlot(0) =
        ItemStack{world::Block::Air, 1U, &items::DiamondHelmet};
    session.inventory().selectHotbar(0);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Head).item == &items::DiamondHelmet);
    REQUIRE(session.inventory().selectedStack().item == &items::IronHelmet);
    std::cout << "testAutoEquipSwapsOccupiedSlot OK\n";
}

// A non-armor item in hand does nothing on right-click-in-air (no crash, no
// spurious slot write) — auto-equip only ever fires for real armor.
void testAutoEquipIgnoresNonArmor() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 5.0F, 5.5F});
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::Apple};
    session.inventory().selectHotbar(0);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    for (std::size_t i = 0; i < kEquipmentSlotCount; ++i) {
        REQUIRE(session.equipment().get(static_cast<EquipmentSlot>(i)).empty());
    }
    std::cout << "testAutoEquipIgnoresNonArmor OK\n";
}

// --- Slot-click filter: a helmet placed via ClickSlot on the head slot (screen
// index 0) succeeds; the SAME helmet clicked onto the boots slot (screen index
// 3) is rejected outright — canEquip gates what goes IN. Sabotage① target: if
// the filter is dropped, the boots slot would accept the helmet and this
// assertion aborts. ---
void testSlotClickFilterAcceptsMatchingSlot() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::IronHelmet};

    // Pick the helmet up onto the cursor (left-click a player-inventory slot).
    session.enqueueCommand(
        ClickSlot{SlotKind::PlayerInventory, 0U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);
    REQUIRE(session.inventory().cursorStack().item == &items::IronHelmet);

    // Screen index 0 = Head (equipmentSlotAt's own draw order).
    session.enqueueCommand(
        ClickSlot{SlotKind::Equipment, 0U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Head).item == &items::IronHelmet);
    REQUIRE(session.inventory().cursorStack().empty());
    std::cout << "testSlotClickFilterAcceptsMatchingSlot OK\n";
}

void testSlotClickFilterRejectsMismatchedSlot() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::IronHelmet};

    session.enqueueCommand(
        ClickSlot{SlotKind::PlayerInventory, 0U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);
    REQUIRE(session.inventory().cursorStack().item == &items::IronHelmet);

    // Screen index 3 = Feet (the boots slot). A helmet must be rejected.
    session.enqueueCommand(
        ClickSlot{SlotKind::Equipment, 3U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Feet).empty());
    // The cursor still holds the helmet — the click was refused, not silently
    // dropped.
    REQUIRE(session.inventory().cursorStack().item == &items::IronHelmet);
    std::cout << "testSlotClickFilterRejectsMismatchedSlot OK\n";
}

// The offhand slot (screen index 4) takes anything — an ordinary block/tool
// stack included, not just armor.
void testOffhandAcceptsAnyItem() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Stone, 4U};

    session.enqueueCommand(
        ClickSlot{SlotKind::PlayerInventory, 0U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);
    REQUIRE(session.inventory().cursorStack().block == world::Block::Stone);

    session.enqueueCommand(
        ClickSlot{SlotKind::Equipment, 4U, static_cast<int>(InventoryMouseButton::Left), false});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Offhand).block == world::Block::Stone);
    REQUIRE(session.inventory().cursorStack().empty());
    std::cout << "testOffhandAcceptsAnyItem OK\n";
}

// --- Shift-click quick-equip: shift-clicking a chestplate sitting in the
// inventory moves it straight into the (empty) chest slot. ---
void testShiftClickQuickEquip() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.inventory().mutableSlot(9) =
        ItemStack{world::Block::Air, 1U, &items::DiamondChestplate};

    session.enqueueCommand(
        ClickSlot{SlotKind::PlayerInventory, 9U, static_cast<int>(InventoryMouseButton::Left), true});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Chest).item == &items::DiamondChestplate);
    REQUIRE(session.inventory().slot(9).empty());
    std::cout << "testShiftClickQuickEquip OK\n";
}

// Shift-click quick-equip does not clobber an already-worn piece — with the
// chest slot occupied, a shift-clicked chestplate falls through to the
// ordinary hotbar<->main quick-move instead (vanilla: quickMoveStack only
// tries the armor slot when it is a valid, EMPTY target).
void testShiftClickDoesNotOverwriteWornArmor() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.equipment().set(EquipmentSlot::Chest,
                            ItemStack{world::Block::Air, 1U, &items::IronChestplate});
    session.inventory().mutableSlot(9) =
        ItemStack{world::Block::Air, 1U, &items::DiamondChestplate};

    session.enqueueCommand(
        ClickSlot{SlotKind::PlayerInventory, 9U, static_cast<int>(InventoryMouseButton::Left), true});
    session.tick(world, host);

    // The worn iron chestplate is untouched...
    REQUIRE(session.equipment().get(EquipmentSlot::Chest).item == &items::IronChestplate);
    // ...and the diamond chestplate moved somewhere else in the player's own
    // inventory rather than vanishing.
    bool foundElsewhere = false;
    for (std::size_t i = 0; i < Inventory::kSlotCount; ++i) {
        if (session.inventory().slot(i).item == &items::DiamondChestplate) {
            foundElsewhere = true;
        }
    }
    REQUIRE(foundElsewhere);
    std::cout << "testShiftClickDoesNotOverwriteWornArmor OK\n";
}

// Shift-clicking OUT of an equipment slot sends the piece back to the
// player's ordinary inventory.
void testShiftClickOutOfEquipmentSlot() {
    TestHost host;
    GameSession session;
    world::World world;
    buildFloor(world);
    session.openContainer(ContainerScreen::PlayerInventory);
    session.equipment().set(EquipmentSlot::Feet,
                            ItemStack{world::Block::Air, 1U, &items::IronBoots});

    session.enqueueCommand(
        ClickSlot{SlotKind::Equipment, 3U, static_cast<int>(InventoryMouseButton::Left), true});
    session.tick(world, host);

    REQUIRE(session.equipment().get(EquipmentSlot::Feet).empty());
    bool foundInInventory = false;
    for (std::size_t i = 0; i < Inventory::kSlotCount; ++i) {
        if (session.inventory().slot(i).item == &items::IronBoots) {
            foundInInventory = true;
        }
    }
    REQUIRE(foundInInventory);
    std::cout << "testShiftClickOutOfEquipmentSlot OK\n";
}

// --- Death drop: keepInventory OFF scatters all 5 equipment slots as item
// entities and clears them. Sabotage② target: if the drop ignores the
// keepInventory gate, equipment would drop even with the rule ON and the
// "keepInventory retains equipment" assertion below aborts. ---
void testDeathDropsEquipmentWhenKeepInventoryOff() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    REQUIRE(!session.gameRules().get<bool>(GameRuleId::KeepInventory));
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    session.equipment().set(EquipmentSlot::Head,
                            ItemStack{world::Block::Air, 1U, &items::IronHelmet});
    session.equipment().set(EquipmentSlot::Chest,
                            ItemStack{world::Block::Air, 1U, &items::IronChestplate});
    session.equipment().set(EquipmentSlot::Legs,
                            ItemStack{world::Block::Air, 1U, &items::IronLeggings});
    session.equipment().set(EquipmentSlot::Feet,
                            ItemStack{world::Block::Air, 1U, &items::IronBoots});
    session.equipment().set(EquipmentSlot::Offhand, ItemStack{world::Block::Stone, 1U});

    const auto entitiesBefore = session.itemEntities().entities().size();
    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();
    REQUIRE(host.playerDied);

    REQUIRE(session.itemEntities().entities().size() == entitiesBefore + 5U);
    for (std::size_t i = 0; i < kEquipmentSlotCount; ++i) {
        REQUIRE(session.equipment().get(static_cast<EquipmentSlot>(i)).empty());
    }
    std::cout << "testDeathDropsEquipmentWhenKeepInventoryOff OK\n";
}

void testKeepInventoryRetainsEquipment() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    REQUIRE(session.gameRules().set<bool>(GameRuleId::KeepInventory, true));
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});
    session.equipment().set(EquipmentSlot::Head,
                            ItemStack{world::Block::Air, 1U, &items::IronHelmet});
    session.equipment().set(EquipmentSlot::Offhand, ItemStack{world::Block::Stone, 1U});

    const auto entitiesBefore = session.itemEntities().entities().size();
    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();
    REQUIRE(host.playerDied);

    // Nothing dropped, nothing cleared — the whole equipment branch is
    // skipped exactly like the main inventory's own keepInventory guard.
    REQUIRE(session.itemEntities().entities().size() == entitiesBefore);
    REQUIRE(session.equipment().get(EquipmentSlot::Head).item == &items::IronHelmet);
    REQUIRE(session.equipment().get(EquipmentSlot::Offhand).block == world::Block::Stone);
    std::cout << "testKeepInventoryRetainsEquipment OK\n";
}

// An armor-less death (all five slots empty) does not crash and drops
// nothing extra — the common case for a player who never wore anything.
void testDeathWithNoEquipmentDropsNothingExtra() {
    TestHost host;
    GameSession session;
    session.setGameMode(GameMode::Survival);
    world::World world;
    buildFloor(world);
    session.player().setPosition({5.5F, 1.0F, 5.5F});

    const auto entitiesBefore = session.itemEntities().entities().size();
    REQUIRE(session.hurtPlayer(kPrimaryPlayerId, DamageType::Fall, 1000.0F, host));
    session.drainEvents();
    REQUIRE(host.playerDied);
    REQUIRE(session.itemEntities().entities().size() == entitiesBefore);
    std::cout << "testDeathWithNoEquipmentDropsNothingExtra OK\n";
}

} // namespace

int main() {
    entities::registerBuiltinEntities();

    testAutoEquipFillsMatchingSlot();
    testAutoEquipRoutesEachSlotIndependently();
    testAutoEquipSwapsOccupiedSlot();
    testAutoEquipIgnoresNonArmor();

    testSlotClickFilterAcceptsMatchingSlot();
    testSlotClickFilterRejectsMismatchedSlot();
    testOffhandAcceptsAnyItem();

    testShiftClickQuickEquip();
    testShiftClickDoesNotOverwriteWornArmor();
    testShiftClickOutOfEquipmentSlot();

    testDeathDropsEquipmentWhenKeepInventoryOff();
    testKeepInventoryRetainsEquipment();
    testDeathWithNoEquipmentDropsNothingExtra();

    std::cout << "equipment_wear_test: all tests passed\n";
    return 0;
}
