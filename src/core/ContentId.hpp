#pragma once

// Dense, strongly-typed content identifiers.
//
// Every kind of content (blocks, items, entity types, block-entity types) gets
// a distinct id type so the compiler rejects handing a BlockId where an ItemId
// belongs, while the storage stays a plain `uint16` — two bytes, trivially
// copyable, and `deref = one array index`. This is the DOD "holder = id" rule:
// a reference to content is the id value, never a heap pointer, and looking the
// content up is a single subscript into the registry's dense table.
//
// Width is `uint16` (65536 slots), not `uint8`: mods must have room to register
// past the 256 the old `enum class Block : uint8_t` topped out at, and block
// *state* ids are already `uint16`, so the identity space matches them.

#include <compare>
#include <cstddef>
#include <cstdint>

namespace mc::core {

// The `Tag` parameter is a phantom type: `ContentId<BlockTag>` and
// `ContentId<ItemTag>` are unrelated types even though both store a `uint16`.
template <typename Tag>
class ContentId final {
  public:
    using Value = std::uint16_t;
    // The sentinel every default-constructed id carries. All-ones so a zeroed
    // buffer never accidentally reads as the valid id 0 (which is a real slot).
    static constexpr Value kInvalidValue = 0xFFFFU;

    constexpr ContentId() = default;
    explicit constexpr ContentId(Value value) : value_(value) {}

    [[nodiscard]] static constexpr ContentId of(Value value) { return ContentId{value}; }
    [[nodiscard]] static constexpr ContentId invalid() { return ContentId{}; }

    [[nodiscard]] constexpr Value value() const { return value_; }
    // The array subscript this id derefs to. Only meaningful when valid().
    [[nodiscard]] constexpr std::size_t index() const { return static_cast<std::size_t>(value_); }
    [[nodiscard]] constexpr bool valid() const { return value_ != kInvalidValue; }

    [[nodiscard]] constexpr bool operator==(const ContentId&) const = default;
    [[nodiscard]] constexpr auto operator<=>(const ContentId&) const = default;

  private:
    Value value_ = kInvalidValue;
};

// Phantom tags. Empty structs whose only job is to make the id types distinct;
// they are never instantiated.
struct BlockIdTag final {};
struct ItemIdTag final {};
struct EntityTypeIdTag final {};
struct BlockEntityTypeIdTag final {};
// A registered status effect (poison, speed, …). EM-2's MobEffect registry
// hands these out; a per-entity EffectInstance holds one plus its duration and
// amplifier.
struct StatusEffectIdTag final {};
// A registered enchantment type (sharpness, protection, …). DDC-0's runtime
// EnchantmentRegistry hands these out; the dense id is the registry subscript,
// distinct from gameplay's EnchantmentId enum (which is the fixed vanilla
// ordinal an ItemStack stores). Both agree slot-for-slot for the built-ins so a
// stored enum ordinal indexes the runtime table directly.
struct EnchantmentTypeIdTag final {};
// The dense id an Identifier interns to, for hot paths that want to compare
// registry keys as integers instead of walking two string_views.
struct IdentifierIdTag final {};

using BlockId = ContentId<BlockIdTag>;
using ItemId = ContentId<ItemIdTag>;
using EntityTypeId = ContentId<EntityTypeIdTag>;
using BlockEntityTypeId = ContentId<BlockEntityTypeIdTag>;
using StatusEffectId = ContentId<StatusEffectIdTag>;
using EnchantmentTypeId = ContentId<EnchantmentTypeIdTag>;
using IdentifierId = ContentId<IdentifierIdTag>;

} // namespace mc::core
