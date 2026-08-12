#pragma once

#include "assets/ResourceProvider.hpp"

#include <filesystem>
#include <memory>

namespace mc::assets {

// Resolves resources from a standard resource pack that is a .zip archive rather
// than a directory — the form most packs are downloaded in. The ResourceProvider
// interface hands consumers a real file path (they stream the OGG, decode the
// PNG from disk), which a zip entry is not, so this extracts an entry to a cache
// directory on first access and returns the cached path. Extraction is lazy and
// per entry: opening a 144 MiB pack does not unpack it, only the files actually
// asked for. miniz lives entirely behind the pimpl so its header does not leak.
class ZipResourcePackProvider final : public ResourceProvider {
  public:
    // `cacheRoot` is where extracted entries are written (a per-pack subdirectory
    // of the game's pack cache). Construction opens the archive and indexes it;
    // valid() reports whether that succeeded.
    ZipResourcePackProvider(std::filesystem::path zipPath, std::filesystem::path cacheRoot);
    ~ZipResourcePackProvider() override;

    ZipResourcePackProvider(const ZipResourcePackProvider&) = delete;
    ZipResourcePackProvider& operator=(const ZipResourcePackProvider&) = delete;
    ZipResourcePackProvider(ZipResourcePackProvider&&) noexcept;
    ZipResourcePackProvider& operator=(ZipResourcePackProvider&&) noexcept;

    [[nodiscard]] bool valid() const;

    [[nodiscard]] std::filesystem::path locate(const ResourceLocation& location) const override;
    [[nodiscard]] bool exists(const ResourceLocation& location) const override;
    [[nodiscard]] std::vector<ResourceLocation> list(std::string_view space,
                                                     std::string_view pathPrefix) const override;
    [[nodiscard]] std::vector<PackLanguage> languages() const override;
    [[nodiscard]] std::filesystem::path resourceRoot() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mc::assets
