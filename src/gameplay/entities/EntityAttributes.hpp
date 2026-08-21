#pragma once

// A creature's tunable numeric attributes, stored the way 26.1's
// DefaultAttributeContainer is *modelled* rather than *implemented*: JE keeps a
// `Map<Attribute, AttributeInstance>`, this keeps a fixed float array indexed by
// a dense attribute id. Deref is one subscript, zero map, zero allocation — the
// DOD form E-DESIGN §2 calls for.
//
// The values are data (E2): each built-in species bakes its floor through the
// Builder (the compiled-in default, the way BiomeSpawnTables compiles its spawn
// numbers), and a datapack may overlay any subset on top per attribute, missing
// ones falling back to that floor. See EntityAttributeOverlay.hpp.

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::gameplay::entities {

// The attributes this project models, in id order. The enum value is the array
// index, so a lookup is `values[static_cast<size_t>(attribute)]`. Mirrors the
// GENERIC_* attributes a MobEntity.createMobAttributes() chain configures.
enum class Attribute : std::uint8_t {
    MaxHealth,           // GENERIC_MAX_HEALTH
    MovementSpeed,       // MOVEMENT_SPEED; converted by locomotion
    AttackDamage,        // GENERIC_ATTACK_DAMAGE, zero for passive mobs
    FollowRange,         // GENERIC_FOLLOW_RANGE
    KnockbackResistance, // GENERIC_KNOCKBACK_RESISTANCE, in [0, 1]
};

inline constexpr std::size_t kAttributeCount = 5U;

// The dense float array plus named accessors so call sites read
// `attributes().maxHealth()` while storage stays a plain array a codec and the
// overlay can walk by attribute id. Defaults match the pre-E2 per-field defaults
// so a species that states none is unchanged.
struct EntityAttributes final {
    std::array<float, kAttributeCount> values{
        /*MaxHealth=*/10.0F, /*MovementSpeed=*/0.25F, /*AttackDamage=*/0.0F,
        /*FollowRange=*/16.0F, /*KnockbackResistance=*/0.0F};

    [[nodiscard]] float get(Attribute attribute) const {
        return values[static_cast<std::size_t>(attribute)];
    }
    void set(Attribute attribute, float value) {
        values[static_cast<std::size_t>(attribute)] = value;
    }

    [[nodiscard]] float maxHealth() const { return get(Attribute::MaxHealth); }
    [[nodiscard]] float movementSpeed() const { return get(Attribute::MovementSpeed); }
    [[nodiscard]] float attackDamage() const { return get(Attribute::AttackDamage); }
    [[nodiscard]] float followRange() const { return get(Attribute::FollowRange); }
    [[nodiscard]] float knockbackResistance() const { return get(Attribute::KnockbackResistance); }

    [[nodiscard]] bool operator==(const EntityAttributes&) const = default;
};

} // namespace mc::gameplay::entities
