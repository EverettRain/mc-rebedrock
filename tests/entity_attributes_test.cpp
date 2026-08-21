// E2 — entity attributes as data: the fixed attribute-id array, the codec, and
// the two-layer floor + datapack overlay + per-attribute fallback.
//
// The floor is each species' compiled-in Builder default; a datapack file at
// data/<space>/entity_attributes/<species>.json overrides the attributes it
// lists, the rest falling back to the floor. EntityType::attributes() resolves
// through the process-wide overlay, so the AI and the spawn code see the
// overridden numbers — proven here by shrinking a zombie's follow range and
// watching it stop acquiring a player it otherwise would.

#include "assets/ResourceProvider.hpp"
#include "data/Codec.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityAttributeOverlay.hpp"
#include "gameplay/entities/EntityAttributes.hpp"
#include "gameplay/entities/EntityAttributesCodec.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

using mc::gameplay::entities::Attribute;
using mc::gameplay::entities::EntityAttributes;
using mc::gameplay::entities::PigEntity;
using mc::gameplay::entities::ZombieEntity;
using mc::gameplay::entities::entityAttributeTable;

namespace fs = std::filesystem;

// Writes a species' override file into a standard pack's `data/` half.
void writeAttr(const fs::path& packRoot, std::string_view space, std::string_view species,
               std::string_view json) {
    const auto path =
        packRoot / "data" / space / "entity_attributes" / (std::string{species} + ".json");
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << json;
}

mc::world::World makeFlatWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// The codec round-trips every attribute through text (sabotage① for D-shaped
// codecs: a field written under one key and read under another breaks this).
void testCodecRoundTrip() {
    EntityAttributes attributes;
    attributes.set(Attribute::MaxHealth, 24.0F);
    attributes.set(Attribute::MovementSpeed, 0.31F);
    attributes.set(Attribute::AttackDamage, 7.0F);
    attributes.set(Attribute::FollowRange, 12.5F);
    attributes.set(Attribute::KnockbackResistance, 0.5F);
    assert(mc::data::roundTripsThroughText(attributes));

    // A file listing only one attribute reads onto a floor, leaving the rest
    // untouched — per-attribute fallback, the ObjectReader::optionalField shape.
    const auto json = mc::core::Json::parse(R"({"max_health": 99.0})");
    EntityAttributes merged = PigEntity::type().attributesFloor();
    assert(mc::data::Codec<EntityAttributes>::read(json, merged));
    assert(merged.maxHealth() == 99.0F);
    assert(merged.movementSpeed() == PigEntity::type().attributesFloor().movementSpeed());
    assert(merged.followRange() == PigEntity::type().attributesFloor().followRange());
}

// No `data/` at all: every species keeps its compiled-in floor and the overlay
// reports nothing overridden.
void testNoDataFallback(const fs::path& scratch) {
    const auto empty = scratch / "empty_pack";
    fs::create_directories(empty);
    mc::assets::StandardPackResourceProvider pack{empty};
    entityAttributeTable().load(pack);
    assert(entityAttributeTable().overrideCount() == 0U);
    // attributes() == floor for every species.
    assert(PigEntity::type().attributes() == PigEntity::type().attributesFloor());
    assert(ZombieEntity::type().attributes() == ZombieEntity::type().attributesFloor());
    // The pre-E2 pig numbers, proving the floor is byte-for-byte what it was.
    assert(PigEntity::type().attributes().maxHealth() == 10.0F);
    assert(PigEntity::type().attributes().movementSpeed() == 0.25F);
    assert(PigEntity::type().attributes().followRange() == 16.0F);
}

// A pack overriding one attribute of one species changes exactly that, and only
// that species; the other attributes fall back to the floor, and every other
// species is untouched.
void testOverlayIsScoped(const fs::path& scratch) {
    const auto pack = scratch / "one_attr_pack";
    writeAttr(pack, "minecraft", "pig", R"({"max_health": 42.0})");
    mc::assets::StandardPackResourceProvider provider{pack};
    entityAttributeTable().load(provider);

    assert(entityAttributeTable().overrideCount() == 1U);
    // Pig's max health changed; its other attributes kept their floor.
    assert(PigEntity::type().attributes().maxHealth() == 42.0F);
    assert(PigEntity::type().attributes().movementSpeed() ==
           PigEntity::type().attributesFloor().movementSpeed());
    assert(PigEntity::type().attributes().followRange() ==
           PigEntity::type().attributesFloor().followRange());
    // Its floor accessor is unchanged: the override lives in the overlay, not in
    // the type.
    assert(PigEntity::type().attributesFloor().maxHealth() == 10.0F);
    // Zombie was not mentioned, so it is entirely its floor.
    assert(ZombieEntity::type().attributes() == ZombieEntity::type().attributesFloor());
}

// Every attribute maps to its own slot: a file that sets all five to distinct
// values must read back distinct through each accessor. This is sabotage③'s
// net — an accessor reading the wrong array index returns another attribute's
// value and this catches it.
void testAttributeSlotsAreDistinct(const fs::path& scratch) {
    const auto pack = scratch / "all_attrs_pack";
    writeAttr(pack, "minecraft", "pig", R"({
        "max_health": 1.0,
        "movement_speed": 2.0,
        "attack_damage": 3.0,
        "follow_range": 4.0,
        "knockback_resistance": 5.0
    })");
    mc::assets::StandardPackResourceProvider provider{pack};
    entityAttributeTable().load(provider);

    const auto& attributes = PigEntity::type().attributes();
    assert(attributes.maxHealth() == 1.0F);
    assert(attributes.movementSpeed() == 2.0F);
    assert(attributes.attackDamage() == 3.0F);
    assert(attributes.followRange() == 4.0F);
    assert(attributes.knockbackResistance() == 5.0F);
}

// The AI reads its numbers through attributes(), so an overlay changes behaviour
// without touching a line of AI logic. A zombie (follow range 35) acquires a
// player six blocks away; shrink its follow range to 2 through a datapack and
// the very same code no longer acquires that player — the radius moved, the
// logic did not.
void testFollowRangeOverlayMovesAcquisitionRadius(const fs::path& scratch) {
    using mc::gameplay::Difficulty;
    using mc::gameplay::EntitySystem;

    const glm::vec3 zombiePos{4.5F, 1.001F, 8.5F};
    const glm::vec3 player{10.5F, 1.001F, 8.5F}; // six blocks away

    const auto acquires = [&]() {
        auto world = makeFlatWorld();
        EntitySystem entities;
        entities.spawn(zombiePos, ZombieEntity::type(), 41U);
        const std::uint64_t zombieId = entities.entities()[0].id;
        for (int tick = 0; tick < 120; ++tick) {
            static_cast<void>(entities.tick(world, player, 0.6F, 1.8F, Difficulty::Normal, true,
                                            false));
            const auto* zombie = entities.byId(zombieId);
            if (zombie != nullptr &&
                zombie->brain.combatTarget() == mc::gameplay::ActorReference::player()) {
                return true;
            }
        }
        return false;
    };

    // Floor follow range (35): the player is well inside it, so it acquires.
    const auto emptyPack = scratch / "ai_empty_pack";
    fs::create_directories(emptyPack);
    entityAttributeTable().load(mc::assets::StandardPackResourceProvider{emptyPack});
    assert(acquires());

    // Shrink follow range under the six-block gap: the same run no longer
    // acquires. Only the datapack number changed.
    const auto shrinkPack = scratch / "ai_shrink_pack";
    writeAttr(shrinkPack, "minecraft", "zombie", R"({"follow_range": 2.0})");
    entityAttributeTable().load(mc::assets::StandardPackResourceProvider{shrinkPack});
    assert(ZombieEntity::type().attributes().followRange() == 2.0F);
    assert(!acquires());

    // Reset the process-wide table so nothing downstream inherits the shrink.
    entityAttributeTable().load(mc::assets::StandardPackResourceProvider{emptyPack});
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();

    const auto scratch = fs::temp_directory_path() / "mc_rebedrock_e2_entity_attributes_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    testCodecRoundTrip();
    testNoDataFallback(scratch);
    testOverlayIsScoped(scratch);
    testAttributeSlotsAreDistinct(scratch);
    testFollowRangeOverlayMovesAcquisitionRadius(scratch);

    fs::remove_all(scratch);
    return 0;
}
