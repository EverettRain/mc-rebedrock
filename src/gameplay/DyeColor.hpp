#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::gameplay {

// DYE-0: the 16 dye colours, as a dense id table mirroring Java 26.1's
// `net.minecraft.util.DyeColor`. The ids are the vanilla ordinal order
// (white=0 ... black=15), so a DyeColor is a DOD-friendly primary key: one
// `uint8` per coloured entity, the palette a `constexpr` table in `.rodata`.
// This node (DYE-0) only introduces the colour identity + the entity colour
// state; the dye item and dye/shear interactions are DYE-1/DYE-2.
//
// The stored representation on an entity is always this dense id. The stable
// vanilla name (used at the save/network boundary and by future JC mapping) is
// derived from the table below — a name never becomes a raw numeric id inside a
// save, and a numeric id never crosses the wire without a name to anchor it.
enum class DyeColor : std::uint8_t {
    White = 0,
    Orange = 1,
    Magenta = 2,
    LightBlue = 3,
    Yellow = 4,
    Lime = 5,
    Pink = 6,
    Gray = 7,
    LightGray = 8,
    Cyan = 9,
    Purple = 10,
    Blue = 11,
    Brown = 12,
    Green = 13,
    Red = 14,
    Black = 15,
};

// The number of dye colours (16). Every valid id is < this.
inline constexpr std::size_t kDyeColorCount = 16U;

// The default colour any entity spawns with and an old save (predating the
// colour field) restores to: white, mirroring a naturally-spawned sheep.
inline constexpr DyeColor kDefaultDyeColor = DyeColor::White;

// One dye colour's baked, parse-free data: the stable vanilla name (the JC /
// save-boundary anchor) and the packed 0xRRGGBB texture diffuse colour Java's
// `DyeColor.getEntityColor()` returns. `fireworkColor` etc. are deferred (this
// node only needs identity + name + the entity render tint).
struct DyeColorInfo final {
    std::string_view name;         // vanilla registry name, e.g. "light_blue"
    std::uint32_t textureColor;    // 0xRRGGBB, Java DyeColor#entityColor
};

// The palette, indexed by DyeColor id. constexpr .rodata — compiled in, never
// parsed. Colour values are the vanilla 26.1 entity/texture-diffuse colours.
inline constexpr std::array<DyeColorInfo, kDyeColorCount> kDyeColors{{
    {"white", 0xF9FFFEU},
    {"orange", 0xF9801DU},
    {"magenta", 0xC74EBDU},
    {"light_blue", 0x3AB3DAU},
    {"yellow", 0xFED83DU},
    {"lime", 0x80C71FU},
    {"pink", 0xF38BAAU},
    {"gray", 0x474F52U},
    {"light_gray", 0x9D9D97U},
    {"cyan", 0x169C9CU},
    {"purple", 0x8932B8U},
    {"blue", 0x3C44AAU},
    {"brown", 0x835432U},
    {"green", 0x5E7C16U},
    {"red", 0xB02E26U},
    {"black", 0x1D1D21U},
}};

// True for an on-disk / on-wire byte that names one of the 16 colours. A record
// carrying anything else is corrupt and the caller falls back to the default.
[[nodiscard]] constexpr bool isValidDyeColorId(std::uint8_t id) noexcept {
    return id < kDyeColorCount;
}

// The dense id of a colour, as the byte a save/network record stores.
[[nodiscard]] constexpr std::uint8_t dyeColorId(DyeColor color) noexcept {
    return static_cast<std::uint8_t>(color);
}

// A stored id back to a colour, clamped to the default on a corrupt/out-of-range
// byte so a bad record can never leave an entity in an invalid colour.
[[nodiscard]] constexpr DyeColor dyeColorFromId(std::uint8_t id) noexcept {
    return isValidDyeColorId(id) ? static_cast<DyeColor>(id) : kDefaultDyeColor;
}

// The stable vanilla name for a colour (the save/JC anchor).
[[nodiscard]] constexpr std::string_view dyeColorName(DyeColor color) noexcept {
    return kDyeColors[dyeColorId(color)].name;
}

// The vanilla entity/texture-diffuse colour (0xRRGGBB) the renderer tints with.
[[nodiscard]] constexpr std::uint32_t dyeColorTexture(DyeColor color) noexcept {
    return kDyeColors[dyeColorId(color)].textureColor;
}

// Resolve a vanilla name back to its colour id. Empty when the name is not one
// of the 16 — used only at the save/data boundary, never on a hot path.
[[nodiscard]] constexpr std::optional<DyeColor> dyeColorFromName(
    std::string_view name) noexcept {
    for (std::size_t index = 0; index < kDyeColorCount; ++index) {
        if (kDyeColors[index].name == name) {
            return static_cast<DyeColor>(index);
        }
    }
    return std::nullopt;
}

}  // namespace mc::gameplay
