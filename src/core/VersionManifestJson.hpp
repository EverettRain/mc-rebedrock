#pragma once

// META-2a: export the version identity as a `version.json`-shaped document, the
// interop counterpart to VersionManifest. Java's jar carries this JSON so a
// launcher can read a build's identity without running it; rebedrock does not
// need it to start (its identity is baked in, META-0), so this is export-only —
// a product for external tools, launchers and the JC compat layer, never a
// runtime config rebedrock reads back to decide who it is.
//
// The keys mirror Java's `version.json` exactly (snake_case), so a tool or the JC
// layer needs zero translation to line the two up. See META-DESIGN.md §2.

#include "core/Json.hpp"
#include "core/VersionManifest.hpp"

#include <string>

namespace mc::core {

// The JE `version.json` key set, in Java's own order. Exposed so a test can
// assert the exported document carries exactly these keys and no rebedrock-
// private ones (a private key would defeat the whole point of interop).
inline constexpr std::string_view kVersionJsonKeys[] = {
    "id",           "name",       "world_version", "protocol_version",
    "pack_version", "build_time", "series_id",     "stable",
};

// Serialise a manifest into Java's version.json shape. Numbers that are version
// counts dump without a decimal point (Json prints exact integers plainly), so
// `world_version` reads back as an integer, matching Java's file.
[[nodiscard]] inline std::string exportVersionJson(const VersionManifest& version = kVersion) {
    Json::Object packVersion;
    packVersion.emplace_back("resource",
                             Json{static_cast<double>(version.packVersion.resource)});
    packVersion.emplace_back("data", Json{static_cast<double>(version.packVersion.data)});

    Json::Object root;
    root.emplace_back("id", Json{std::string{version.id}});
    root.emplace_back("name", Json{std::string{version.name}});
    root.emplace_back("world_version", Json{static_cast<double>(version.worldVersion)});
    root.emplace_back("protocol_version", Json{static_cast<double>(version.protocolVersion)});
    root.emplace_back("pack_version", Json{std::move(packVersion)});
    root.emplace_back("build_time", Json{std::string{version.buildTime}});
    root.emplace_back("series_id", Json{std::string{version.seriesId}});
    root.emplace_back("stable", Json{version.stable});
    return Json{std::move(root)}.dump();
}

} // namespace mc::core
