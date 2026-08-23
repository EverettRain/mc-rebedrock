#pragma once

// The game's single version identity — rebedrock's equivalent of Java 26.1's
// jar-internal `version.json` (what `SharedConstants` reads once at startup).
// Java parses that JSON at runtime because a launcher is a separate process that
// runs many version jars; rebedrock is one native build, so its identity is
// fixed at compile time. `kVersion` is therefore a `constexpr` singleton living
// in `.rodata`: zero parse, zero allocation, and every cross-field constraint is
// checked by `static_assert` here rather than at runtime.
//
// This is the ONE place any version number is defined. The wire protocol version
// and the save format version used to be independent literals buried in `net/`
// and `persistence/`, unaware of each other and of the game version that is their
// common source. They now retire to named fields of `kVersion`; their consumers
// reference `kVersion.protocolVersion` / `kVersion.worldVersion`, so bumping a
// version happens once and a forgotten sync is a compile error, not a shipped
// mismatch. See content-dev/META-metadata/META-DESIGN.md.

#include "core/BuildInfo.hpp"

#include <cstdint>
#include <string_view>

namespace mc::core {

// The resource/data pack format numbers, the pair Java's `version.json` carries
// as `pack_version{resource,data}`. They gate whether a resource or data pack is
// compatible with this build (resource-pack work consumes these); they move
// independently of the game version, hence their own struct.
struct PackVersion final {
    std::uint32_t resource = 0U;
    std::uint32_t data = 0U;

    friend constexpr bool operator==(const PackVersion&, const PackVersion&) = default;
};

// The whole version identity of this build, mirroring the field set of Java's
// `version.json`. Every number a consumer needs — protocol, save format, pack
// formats — is a named field here and nowhere else.
struct VersionManifest final {
    // The human game version, e.g. "26.1" (Java `id`/`name`). `id` is the stable
    // machine key, `name` the display form; they coincide for a release build.
    std::string_view id;
    std::string_view name;

    // The save/world format number (Java `world_version`, i.e. `level.dat`'s
    // top-level `DataVersion`). This IS the former `persistence` kFormatVersion;
    // it advances whenever the on-disk layout changes. Note this is rebedrock's
    // own monotonic format number, NOT Java's DataVersion value space — the two
    // are mapped by JC, not equal (registered as a JC deviation).
    std::uint32_t worldVersion = 0U;

    // The wire protocol version the handshake negotiates (Java `protocol_version`,
    // the former `net` kProtocolVersion). It advances when a message's byte layout
    // changes in a way an older peer cannot decode.
    std::uint32_t protocolVersion = 0U;

    // Resource/data pack format compatibility (Java `pack_version`).
    PackVersion packVersion;

    // The release series this build belongs to (Java `series_id`), and whether it
    // is a stable release versus a snapshot (Java `stable`).
    std::string_view seriesId;
    bool stable = false;

    // Build provenance, baked in by CMake from the git commit (never the wall
    // clock). `buildTime` is ISO 8601; `buildRef` is the short commit hash.
    std::string_view buildTime;
    std::string_view buildRef;

    friend constexpr bool operator==(const VersionManifest&, const VersionManifest&) = default;
};

// The one and only build identity. constexpr .rodata — compiled in, not parsed.
inline constexpr VersionManifest kVersion{
    .id = "26.1",
    .name = "26.1",
    .worldVersion = 19U,
    .protocolVersion = 4U,
    .packVersion = PackVersion{.resource = 1U, .data = 1U},
    .seriesId = "main",
    .stable = true,
    .buildTime = kBuildTime,
    .buildRef = kBuildRef,
};

// --- Compile-time consistency: a version that violates an invariant never ships.

// Every version number is meaningful; zero means "forgot to set it".
static_assert(kVersion.worldVersion != 0U, "worldVersion must be set");
static_assert(kVersion.protocolVersion != 0U, "protocolVersion must be set");
static_assert(kVersion.packVersion.resource != 0U, "resource pack version must be set");
static_assert(kVersion.packVersion.data != 0U, "data pack version must be set");

// Identity strings are non-empty — a nameless build cannot self-describe a save.
static_assert(!kVersion.id.empty(), "version id must be set");
static_assert(!kVersion.name.empty(), "version name must be set");
static_assert(!kVersion.seriesId.empty(), "series id must be set");

namespace detail {
// A cheap, parse-free ISO 8601 sanity shape: "YYYY-MM-DDT..." — enough to catch a
// buildTime that is empty or obviously not a timestamp, without a real parser at
// compile time. "unknown" (the no-git fallback) is accepted so an out-of-checkout
// build still compiles.
[[nodiscard]] constexpr bool looksLikeIso8601OrUnknown(std::string_view value) {
    if (value == "unknown") {
        return true;
    }
    if (value.size() < 10U) {
        return false;
    }
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    return isDigit(value[0]) && isDigit(value[1]) && isDigit(value[2]) && isDigit(value[3]) &&
           value[4] == '-' && isDigit(value[5]) && isDigit(value[6]) && value[7] == '-' &&
           isDigit(value[8]) && isDigit(value[9]);
}
}  // namespace detail

static_assert(detail::looksLikeIso8601OrUnknown(kVersion.buildTime),
              "buildTime must be ISO 8601 (YYYY-MM-DD...) or the 'unknown' fallback");

} // namespace mc::core
