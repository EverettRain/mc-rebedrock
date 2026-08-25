#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

void EntityDrops::add(const ItemStack& stack) {
    if (stack.empty() || count >= kMaximumEntries) {
        return;
    }
    entries[count++] = stack;
}

EntityType::Builder EntityType::Builder::create(MobCategory category, const EntityAi& ai) {
    Builder builder;
    builder.draft_.category_ = category;
    builder.draft_.ai_ = &ai;
    return builder;
}

EntityType::Builder& EntityType::Builder::spawnPlacement(SpawnPlacement placement) {
    draft_.spawnPlacement_ = placement;
    return *this;
}

EntityType::Builder& EntityType::Builder::sized(float width, float height) {
    draft_.dimensions_ = {width, height};
    return *this;
}

EntityType::Builder& EntityType::Builder::attributes(const EntityAttributes& attributes) {
    draft_.attributes_ = attributes;
    return *this;
}

EntityType::Builder& EntityType::Builder::health(float maxHealth) {
    draft_.attributes_.set(Attribute::MaxHealth, maxHealth);
    return *this;
}

EntityType::Builder& EntityType::Builder::movementSpeed(float speed) {
    draft_.attributes_.set(Attribute::MovementSpeed, speed);
    return *this;
}

EntityType::Builder& EntityType::Builder::attackDamage(float damage) {
    draft_.attributes_.set(Attribute::AttackDamage, damage);
    return *this;
}

EntityType::Builder& EntityType::Builder::followRange(float range) {
    draft_.attributes_.set(Attribute::FollowRange, range);
    return *this;
}

EntityType::Builder& EntityType::Builder::knockbackResistance(float resistance) {
    draft_.attributes_.set(Attribute::KnockbackResistance, resistance);
    return *this;
}

EntityType::Builder& EntityType::Builder::spawnEgg(std::uint32_t primary, std::uint32_t secondary) {
    draft_.spawnEgg_ = {primary, secondary};
    draft_.hasSpawnEgg_ = true;
    return *this;
}

EntityType::Builder& EntityType::Builder::behavior(EntityBehavior flag) {
    draft_.behaviorFlags_ = static_cast<std::uint16_t>(
        draft_.behaviorFlags_ | static_cast<std::uint16_t>(flag));
    return *this;
}

EntityType::Builder& EntityType::Builder::fireImmune() {
    return behavior(EntityBehavior::FireImmune);
}

EntityType::Builder& EntityType::Builder::sunImmune() {
    return behavior(EntityBehavior::SunImmune);
}

EntityType::Builder& EntityType::Builder::fallImmune() {
    return behavior(EntityBehavior::FallImmune);
}

EntityType::Builder& EntityType::Builder::undead() {
    return behavior(EntityBehavior::Undead);
}

EntityType::Builder& EntityType::Builder::hungerOnHit() {
    return behavior(EntityBehavior::HungerOnHit);
}

EntityType::Builder& EntityType::Builder::arthropod() {
    return behavior(EntityBehavior::Arthropod);
}

EntityType::Builder& EntityType::Builder::breeding(const BreedingProfile& profile) {
    draft_.breeding_ = profile;
    return *this;
}

EntityType::Builder& EntityType::Builder::breedableWith(const ItemStack& temptItem) {
    draft_.breeding_ = BreedingProfile{/*breedable=*/true, temptItem, /*babyScale=*/0.5F};
    return *this;
}

EntityType::Builder& EntityType::Builder::eggLay(const EggLayProfile& profile) {
    draft_.eggLay_ = profile;
    return *this;
}

EntityType::Builder& EntityType::Builder::laysEggs(const ItemStack& item) {
    draft_.eggLay_ = EggLayProfile{/*laysEggs=*/true, item};
    return *this;
}

EntityType::Builder& EntityType::Builder::loot(LootRoll roll) {
    draft_.loot_ = roll;
    return *this;
}

EntityType::Builder& EntityType::Builder::xpReward(std::int32_t amount) {
    draft_.xpReward_ = amount;
    draft_.xpRewardMax_ = amount;
    return *this;
}

EntityType::Builder& EntityType::Builder::xpReward(std::int32_t min, std::int32_t max) {
    draft_.xpReward_ = min;
    draft_.xpRewardMax_ = max < min ? min : max;
    return *this;
}

EntityType::Builder& EntityType::Builder::renderer(const EntityRenderDescriptor& descriptor) {
    draft_.render_ = descriptor;
    return *this;
}

EntityType::Builder& EntityType::Builder::sounds(const audio::MobSoundProfile& profile) {
    draft_.soundProfile_ = profile;
    return *this;
}

EntityType::Builder& EntityType::Builder::vanillaName(std::string_view path) {
    draft_.vanillaId_ = core::Identifier{core::kVanillaNamespace, path};
    return *this;
}

EntityType EntityType::Builder::build(std::string_view path) const {
    EntityType type = draft_;
    type.id_ = core::Identifier{core::kNamespace, path};
    if (type.vanillaId_.empty()) {
        type.vanillaId_ = core::Identifier{core::kVanillaNamespace, path};
    }
    return type;
}

} // namespace mc::gameplay::entities
