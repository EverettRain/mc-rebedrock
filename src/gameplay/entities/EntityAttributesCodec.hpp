#pragma once

// The serde codec for EntityAttributes: the JSON shape a datapack writes an
// entity-attribute override in, and the reader the overlay loads it through.
// Kept out of EntityAttributes.hpp so the data-layer dependency (core::Json,
// data::Codec) does not leak into EntityType.hpp and every simulation TU that
// includes it — only the overlay loader and its test pull this in.
//
// Every field is optional (ObjectReader::optionalField): a file may list any
// subset of attributes, and each absent key leaves that attribute at whatever
// the target already held — which the overlay pre-fills with the species' floor,
// giving per-attribute fallback for free. A present-but-mistyped value still
// fails the read, so a malformed file is skipped rather than half-applied.

#include "data/Codec.hpp"
#include "gameplay/entities/EntityAttributes.hpp"

namespace mc::data {

template <>
struct Codec<mc::gameplay::entities::EntityAttributes> {
    using Attributes = mc::gameplay::entities::EntityAttributes;
    using Attribute = mc::gameplay::entities::Attribute;

    static core::Json write(const Attributes& value) {
        return ObjectWriter{}
            .field("max_health", value.maxHealth())
            .field("movement_speed", value.movementSpeed())
            .field("attack_damage", value.attackDamage())
            .field("follow_range", value.followRange())
            .field("knockback_resistance", value.knockbackResistance())
            .take();
    }

    static bool read(const core::Json& json, Attributes& out) {
        float maxHealth = out.maxHealth();
        float movementSpeed = out.movementSpeed();
        float attackDamage = out.attackDamage();
        float followRange = out.followRange();
        float knockbackResistance = out.knockbackResistance();
        ObjectReader reader{json};
        reader.optionalField("max_health", maxHealth)
            .optionalField("movement_speed", movementSpeed)
            .optionalField("attack_damage", attackDamage)
            .optionalField("follow_range", followRange)
            .optionalField("knockback_resistance", knockbackResistance);
        if (!reader.ok()) {
            return false;
        }
        out.set(Attribute::MaxHealth, maxHealth);
        out.set(Attribute::MovementSpeed, movementSpeed);
        out.set(Attribute::AttackDamage, attackDamage);
        out.set(Attribute::FollowRange, followRange);
        out.set(Attribute::KnockbackResistance, knockbackResistance);
        return true;
    }
};

} // namespace mc::data
