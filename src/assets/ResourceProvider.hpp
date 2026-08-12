#pragma once

#include "assets/PackMetadata.hpp"
#include "assets/ResourceLocation.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mc::assets {

// The one place that knows where a resource physically lives. Every consumer
// used to rebuild that knowledge by hand — TextureManager walked
// `blockTextureRoot.parent_path()` up to five times to recover the resource
// root, then re-joined `textures/minecraft/...` — so the layout was encoded in
// ~34 scattered path expressions. A provider centralises it: ask for a
// ResourceLocation, get a file. Swapping the layout (a standard resource pack,
// a zip) becomes a different provider rather than a hunt through call sites.
class ResourceProvider {
  public:
    virtual ~ResourceProvider() = default;

    // The on-disk path a resource resolves to. Directory-backed providers hand
    // back a real path (consumers stream the OGG or decode the PNG from it); a
    // future zip provider would instead expose bytes. Returns an empty path when
    // this provider cannot place the resource.
    [[nodiscard]] virtual std::filesystem::path locate(const ResourceLocation& location) const = 0;

    [[nodiscard]] virtual bool exists(const ResourceLocation& location) const = 0;

    // Every physical definition of a logical resource, ordered from lowest to
    // highest priority. Most resources use locate() because the top file wins;
    // stack-merged resources such as sounds.json and language tables need to
    // consume all definitions in pack order. A single provider contributes at
    // most one file, while LayeredResourceProvider exposes its complete stack.
    [[nodiscard]] virtual std::vector<std::filesystem::path>
    locateAll(const ResourceLocation& location) const;

    // Lists logical files below a standard content prefix. Language discovery
    // deliberately does not use this broad scan; its compact catalog comes
    // from languages()/pack.mcmeta.
    [[nodiscard]] virtual std::vector<ResourceLocation> list(std::string_view space,
                                                             std::string_view pathPrefix) const;

    // Language-menu metadata declared by this pack's top-level pack.mcmeta.
    // Vanilla builds the menu from this compact catalog; it never opens every
    // lang/*.json merely to discover a display name.
    [[nodiscard]] virtual std::vector<PackLanguage> languages() const;

    // The `resources/` root, for the two consumers (animation clips, entity
    // models) that still walk a whole subtree rather than name one file. A
    // directory source has a real root; a future zip source that cannot expose
    // one will need those consumers reworked to read named entries instead.
    [[nodiscard]] virtual std::filesystem::path resourceRoot() const = 0;
};

// Resolves resources against this project's current on-disk layout, which is not
// yet a standard pack: vanilla assets live under `<root>/vanilla/1.16.1/…` with
// the namespace folder *after* the category and a few historical quirks
// (`sounds/…` sits under `audio/`, translations under `localization/`, the
// bitmap font under `textures/font/` but its glyph widths under `fonts/`), while
// this project's own assets (animation clips, entity skins) sit directly under
// `<root>/`. All of that irregularity is captured here, in one function, so it
// is stated once instead of thirty-four times.
class DirectoryResourceProvider final : public ResourceProvider {
  public:
    // `resourceRoot` is the `resources/` directory; `vanillaVersion` is the
    // version segment under `vanilla/` (currently "1.16.1").
    explicit DirectoryResourceProvider(std::filesystem::path resourceRoot,
                                       std::string vanillaVersion = "1.16.1");

    [[nodiscard]] std::filesystem::path locate(const ResourceLocation& location) const override;
    [[nodiscard]] bool exists(const ResourceLocation& location) const override;
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return resourceRoot_; }

    // `resources/vanilla/<version>`, the root of the bundled vanilla assets.
    [[nodiscard]] std::filesystem::path vanillaRoot() const {
        return resourceRoot_ / "vanilla" / vanillaVersion_;
    }

  private:
    std::filesystem::path resourceRoot_;
    std::string vanillaVersion_;
};

// Resolves resources against a standard resource pack laid out the way Minecraft
// Java ships them: `<packRoot>/assets/<namespace>/<path>`. Because this project's
// ResourceLocation `path` already *is* the standard, pack-relative content path,
// the mapping is a single join with no category renames — the irregularity all
// lived in the legacy layout above. This is what lets rebedrock read a vanilla
// pack (the report's `vanilla-26.1`) or any third-party pack directly.
class StandardPackResourceProvider final : public ResourceProvider {
  public:
    explicit StandardPackResourceProvider(std::filesystem::path packRoot);

    [[nodiscard]] std::filesystem::path locate(const ResourceLocation& location) const override;
    [[nodiscard]] bool exists(const ResourceLocation& location) const override;
    [[nodiscard]] std::vector<ResourceLocation> list(std::string_view space,
                                                     std::string_view pathPrefix) const override;
    [[nodiscard]] std::vector<PackLanguage> languages() const override { return languages_; }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return packRoot_; }

    [[nodiscard]] const std::filesystem::path& packRoot() const { return packRoot_; }

  private:
    std::filesystem::path packRoot_;
    std::vector<PackLanguage> languages_;
};

// A priority stack of providers, the way a client layers enabled packs over the
// built-in resources. `locate` returns the first overlay that actually has the
// resource and falls back to the base, so an imported pack overrides only the
// files it ships and everything else still resolves from the bundled assets.
// Overlays are searched in the order given, highest priority first.
//
// resourceRoot() always comes from the base: this project's own assets
// (animation clips, entity models) are never part of a vanilla pack, so the
// subtree scanners that need a root must see the bundled `resources/`, not an
// overlay.
class LayeredResourceProvider final : public ResourceProvider {
  public:
    LayeredResourceProvider(const ResourceProvider& base,
                            std::vector<const ResourceProvider*> overlays);

    [[nodiscard]] std::filesystem::path locate(const ResourceLocation& location) const override;
    [[nodiscard]] bool exists(const ResourceLocation& location) const override;
    [[nodiscard]] std::vector<std::filesystem::path>
    locateAll(const ResourceLocation& location) const override;
    [[nodiscard]] std::vector<ResourceLocation> list(std::string_view space,
                                                     std::string_view pathPrefix) const override;
    [[nodiscard]] std::vector<PackLanguage> languages() const override;
    [[nodiscard]] std::filesystem::path resourceRoot() const override {
        return base_->resourceRoot();
    }

  private:
    const ResourceProvider* base_;
    std::vector<const ResourceProvider*> overlays_;
};

} // namespace mc::assets
