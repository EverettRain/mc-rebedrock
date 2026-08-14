#include "assets/ResourceProvider.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mc::assets {
namespace {

[[nodiscard]] std::string_view firstSegment(std::string_view path) {
    const auto slash = path.find('/');
    return slash == std::string_view::npos ? path : path.substr(0, slash);
}

[[nodiscard]] std::string_view afterFirstSegment(std::string_view path) {
    const auto slash = path.find('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1U);
}

} // namespace

std::vector<std::filesystem::path>
ResourceProvider::locateAll(const ResourceLocation& location) const {
    if (!exists(location)) {
        return {};
    }
    return {locate(location)};
}

std::vector<std::byte> ResourceProvider::readBytes(const ResourceLocation& location) const {
    // The default satisfies it through the filesystem, which is exactly right
    // for the directory-backed providers. Only the zip provider overrides it.
    const auto path = locate(location);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
}

std::vector<std::vector<std::byte>>
ResourceProvider::readAllBytes(const ResourceLocation& location) const {
    if (!exists(location)) {
        return {};
    }
    return {readBytes(location)};
}

std::vector<ResourceLocation> ResourceProvider::list(std::string_view, std::string_view) const {
    return {};
}

std::vector<PackLanguage> ResourceProvider::languages() const { return {}; }

DirectoryResourceProvider::DirectoryResourceProvider(std::filesystem::path resourceRoot,
                                                     std::string vanillaVersion)
    : resourceRoot_(std::move(resourceRoot)), vanillaVersion_(std::move(vanillaVersion)) {}

std::filesystem::path DirectoryResourceProvider::locate(const ResourceLocation& location) const {
    // The bundled tree carries no server data: tags, loot tables and recipes
    // come from the 26.1 data pack the player supplies, exactly like textures
    // and sounds do. An empty path reads as "this provider cannot place it",
    // and the layered stack falls through to a pack that can.
    if (location.type == PackType::ServerData) {
        return {};
    }
    const std::string_view category = firstSegment(location.path);
    const std::string rest{afterFirstSegment(location.path)};
    const auto vanilla = vanillaRoot();
    // The current bundled layout is `<category>/<namespace>/<rest>`, with three
    // historical renames of the category folder that a standard pack does not
    // have. Each is spelled out once, here.
    if (category == "textures") {
        // textures/minecraft/<rest> — also covers gui, colormap, misc,
        // environment and the bitmap font, which all live under textures/.
        return vanilla / "textures" / location.space / rest;
    }
    if (category == "sounds") {
        // The OGGs sit under `audio/minecraft/sounds/…`, not `sounds/…`.
        return vanilla / "audio" / location.space / "sounds" / rest;
    }
    if (category == "lang") {
        // ReBedrock-authored translations are project resources, not Mojang
        // assets. Keep them in their own namespace so a standard pack can
        // override/add `assets/rebedrock/lang/<code>.json` exactly like 26.1's
        // resource manager merges every namespace for the active language.
        if (location.space == "rebedrock") {
            return resourceRoot_ / "lang" / "rebedrock" / rest;
        }
        // Translation tables live under `localization/minecraft/…`.
        return vanilla / "localization" / location.space / rest;
    }
    if (category == "font") {
        // The glyph-width table is the lone tenant of the top-level `fonts/`
        // dir; the bitmap font pages are textures and resolve above.
        return vanilla / "fonts" / location.space / rest;
    }
    // Everything else is one of this project's own assets (animation clips,
    // entity models), which sit directly under the resources root.
    return resourceRoot_ / location.path;
}

bool DirectoryResourceProvider::exists(const ResourceLocation& location) const {
    std::error_code error;
    return std::filesystem::exists(locate(location), error);
}

StandardPackResourceProvider::StandardPackResourceProvider(std::filesystem::path packRoot)
    : packRoot_(std::move(packRoot)) {
    std::ifstream input{packRoot_ / "pack.mcmeta", std::ios::binary};
    if (!input) {
        return;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    try {
        languages_ = PackMetadata::parse(contents.str()).languages;
    } catch (const std::exception&) {
        // An invalid metadata file is diagnosed by pack discovery. Resource
        // resolution remains usable, but the pack contributes no languages.
    }
}

std::filesystem::path StandardPackResourceProvider::locate(const ResourceLocation& location) const {
    // The whole mapping: `<packRoot>/<assets|data>/<namespace>/<content path>`.
    const char* const root =
        location.type == PackType::ServerData ? "data" : "assets";
    return packRoot_ / root / location.space / location.path;
}

bool StandardPackResourceProvider::exists(const ResourceLocation& location) const {
    std::error_code error;
    return std::filesystem::exists(locate(location), error);
}

std::vector<ResourceLocation>
StandardPackResourceProvider::list(std::string_view space, std::string_view pathPrefix) const {
    std::vector<ResourceLocation> result;
    const auto namespaceRoot = packRoot_ / "assets" / space;
    const auto searchRoot = namespaceRoot / pathPrefix;
    std::error_code error;
    if (!std::filesystem::is_directory(searchRoot, error)) {
        return result;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(searchRoot, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        result.push_back(ResourceLocation{
            std::string{space}, entry.path().lexically_relative(namespaceRoot).generic_string()});
    }
    return result;
}

LayeredResourceProvider::LayeredResourceProvider(const ResourceProvider& base,
                                                 std::vector<const ResourceProvider*> overlays)
    : base_(&base), overlays_(std::move(overlays)) {}

std::filesystem::path LayeredResourceProvider::locate(const ResourceLocation& location) const {
    for (const auto* overlay : overlays_) {
        if (overlay != nullptr && overlay->exists(location)) {
            return overlay->locate(location);
        }
    }
    return base_->locate(location);
}

bool LayeredResourceProvider::exists(const ResourceLocation& location) const {
    for (const auto* overlay : overlays_) {
        if (overlay != nullptr && overlay->exists(location)) {
            return true;
        }
    }
    return base_->exists(location);
}

std::vector<std::filesystem::path>
LayeredResourceProvider::locateAll(const ResourceLocation& location) const {
    std::vector<std::filesystem::path> result = base_->locateAll(location);
    // overlays_ is stored highest-first for locate(); merged resources must be
    // read in the opposite direction so the highest pack is applied last.
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        if (*overlay == nullptr) {
            continue;
        }
        auto files = (*overlay)->locateAll(location);
        result.insert(result.end(), files.begin(), files.end());
    }
    return result;
}

std::vector<std::byte> LayeredResourceProvider::readBytes(const ResourceLocation& location) const {
    // Highest priority first, exactly like locate(): the first overlay that has
    // the resource wins. Without this override the base implementation would
    // route through locate(), and a zip in the stack would extract the file to
    // disk on every read — which is the whole thing readBytes removes.
    for (const auto* overlay : overlays_) {
        if (overlay != nullptr && overlay->exists(location)) {
            return overlay->readBytes(location);
        }
    }
    return base_->readBytes(location);
}

std::vector<std::vector<std::byte>>
LayeredResourceProvider::readAllBytes(const ResourceLocation& location) const {
    // Lowest to highest, mirroring locateAll: merged resources are applied in
    // pack order and the highest pack is applied last.
    std::vector<std::vector<std::byte>> result = base_->readAllBytes(location);
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        if (*overlay == nullptr) {
            continue;
        }
        auto bytes = (*overlay)->readAllBytes(location);
        result.insert(result.end(), std::make_move_iterator(bytes.begin()),
                      std::make_move_iterator(bytes.end()));
    }
    return result;
}

std::vector<ResourceLocation> LayeredResourceProvider::list(std::string_view space,
                                                            std::string_view pathPrefix) const {
    std::vector<ResourceLocation> result;
    std::unordered_set<std::string> seen;
    const auto append = [&](const ResourceProvider& provider) {
        for (auto location : provider.list(space, pathPrefix)) {
            if (seen.insert(location.toString()).second) {
                result.push_back(std::move(location));
            }
        }
    };
    append(*base_);
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        if (*overlay != nullptr) {
            append(**overlay);
        }
    }
    return result;
}

std::vector<PackLanguage> LayeredResourceProvider::languages() const {
    std::unordered_map<std::string, PackLanguage> merged;
    const auto append = [&](const ResourceProvider& provider) {
        for (auto entry : provider.languages()) {
            merged.insert_or_assign(entry.code, std::move(entry));
        }
    };
    append(*base_);
    // Apply metadata in the same low-to-high order as locateAll(), so the
    // highest-priority pack owns duplicate language codes.
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        if (*overlay != nullptr) {
            append(**overlay);
        }
    }
    std::vector<PackLanguage> result;
    result.reserve(merged.size());
    for (auto& [code, entry] : merged) {
        result.push_back(std::move(entry));
    }
    std::ranges::sort(result, {}, &PackLanguage::code);
    return result;
}

} // namespace mc::assets
