#pragma once

// ENCH-2: the enchanting screen's gibberish preview line — 26.1's
// EnchantmentNames, transcribed.
//
// This is deliberately presentation-only and lives in ui/, not gameplay/: the
// text is decorative, it never reaches the server, and it says nothing about
// what the offer actually is (that is the hover tooltip's clue). What it DOES
// have to be is stable — vanilla seeds one RandomSource from the player's
// enchantment seed and draws the three lines from it in order, so the same table
// shows the same three phrases every frame and changes them only when the seed
// does. A per-frame `rand()` here would make the screen shimmer.
//
// The glyphs come from the pack's Standard Galactic Alphabet page
// (`minecraft:alt` -> font/ascii_sga.png), reached through
// ui::galacticCodepoint; nothing here draws or invents a letterform.

#include "ui/TextFont.hpp"
#include "world/gen/JavaRandom.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mc::ui {

// EnchantmentNames#words, verbatim and in order — the order is load-bearing,
// since the pick is `words[random.nextInt(words.length)]`.
inline constexpr std::array<std::string_view, 63> kEnchantmentNameWords{
    "the",      "elder",   "scrolls",  "klaatu",    "berata",   "niktu",     "xyzzy",
    "bless",    "curse",   "light",    "darkness",  "fire",     "air",       "earth",
    "water",    "hot",     "dry",      "cold",      "wet",      "ignite",    "snuff",
    "embiggen", "twist",   "shorten",  "stretch",   "fiddle",   "destroy",   "imbue",
    "galvanize","enchant", "free",     "limited",   "range",    "of",        "towards",
    "inside",   "sphere",  "cube",     "self",      "other",    "ball",      "mental",
    "physical", "grow",    "shrink",   "demon",     "elemental","spirit",    "animal",
    "creature", "beast",   "humanoid", "undead",    "fresh",    "stale",     "phnglui",
    "mglwnafh", "cthulhu", "rlyeh",    "wgahnagl",  "fhtagn",   "baguette",
};

// EnchantmentNames#getRandomName's draw sequence: `nextInt(2) + 3` words, each
// `words[nextInt(words.length)]`, joined by spaces. Returned as plain Latin so
// a test can assert the exact phrase; the caller translates it to galactic when
// it draws.
//
// The RandomSource is the CALLER's, advanced across the three bars in order —
// vanilla seeds it once per render (initSeed) and only calls this for the bars
// that are live, so a dead bar must NOT consume draws. Passing the stream in
// keeps that sequencing where it belongs, at the call site.
[[nodiscard]] inline std::string randomEnchantmentName(world::gen::JavaRandom& random) {
    const std::int32_t wordCount = random.nextInt(2) + 3;
    std::string result;
    for (std::int32_t index = 0; index < wordCount; ++index) {
        if (index != 0) {
            result.push_back(' ');
        }
        const auto pick = static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(kEnchantmentNameWords.size())));
        result.append(kEnchantmentNameWords[pick]);
    }
    return result;
}

// The same text with every letter moved to its galactic form, ready to draw.
// Spaces (and anything else without a galactic form) pass through unchanged.
[[nodiscard]] inline std::string toGalactic(std::string_view text) {
    std::string result;
    result.reserve(text.size() * 3U);
    for (const char32_t codepoint : decodeUtf8(text)) {
        appendUtf8(result, galacticCodepoint(codepoint));
    }
    return result;
}

} // namespace mc::ui
