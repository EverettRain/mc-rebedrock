#include "assets/ZipResourcePack.hpp"

#include <miniz.h>

#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace mc::assets {

struct ZipResourcePackProvider::Impl {
    std::filesystem::path cacheRoot;
    mz_zip_archive zip{};
    bool opened = false;
    std::unordered_set<std::string> entries; // pack-relative names present in the archive
    std::vector<PackLanguage> languages;
    std::mutex archiveMutex;

    explicit Impl(std::filesystem::path zipPath, std::filesystem::path cache)
        : cacheRoot(std::move(cache)) {
        if (mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0) == MZ_FALSE) {
            return;
        }
        opened = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        entries.reserve(count);
        for (mz_uint index = 0; index < count; ++index) {
            mz_zip_archive_file_stat stat;
            if (mz_zip_reader_file_stat(&zip, index, &stat) == MZ_FALSE) {
                continue;
            }
            if (mz_zip_reader_is_file_a_directory(&zip, index) == MZ_FALSE) {
                entries.insert(stat.m_filename);
            }
        }
        size_t metadataSize = 0U;
        void* metadataBytes =
            mz_zip_reader_extract_file_to_heap(&zip, "pack.mcmeta", &metadataSize, 0);
        if (metadataBytes != nullptr) {
            try {
                const std::string_view metadata{static_cast<const char*>(metadataBytes),
                                                metadataSize};
                languages = PackMetadata::parse(metadata).languages;
            } catch (const std::exception&) {
                // Pack discovery owns malformed-metadata diagnostics. Keep the
                // archive usable for resource lookup, with no catalog entries.
            }
            mz_free(metadataBytes);
        }
    }

    ~Impl() {
        if (opened) {
            mz_zip_reader_end(&zip);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Extracts one archive entry to its mirror path under the cache root the
    // first time it is asked for, then returns that path. A failed extraction
    // returns an empty path so the caller treats the entry as absent.
    // Reads an entry straight into memory. This is what lets a zipped pack be
    // consumed without writing anything to disk — the whole reason `.packcache`
    // existed was that the only interface was a path.
    [[nodiscard]] std::vector<std::byte> read(const std::string& entry) {
        const std::scoped_lock lock{archiveMutex};
        std::size_t size = 0U;
        void* const raw = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
        if (raw == nullptr) {
            return {};
        }
        const auto* const first = static_cast<const std::byte*>(raw);
        std::vector<std::byte> bytes{first, first + size};
        mz_free(raw);
        return bytes;
    }

    [[nodiscard]] std::filesystem::path extract(const std::string& entry) {
        const std::scoped_lock lock{archiveMutex};
        const std::filesystem::path destination = cacheRoot / entry;
        std::error_code error;
        if (std::filesystem::exists(destination, error)) {
            return destination;
        }
        std::filesystem::create_directories(destination.parent_path(), error);
        if (mz_zip_reader_extract_file_to_file(&zip, entry.c_str(), destination.string().c_str(),
                                               0) == MZ_FALSE) {
            return {};
        }
        return destination;
    }
};

namespace {

[[nodiscard]] std::string entryName(const ResourceLocation& location) {
    // Standard pack layout, as the directory provider uses too:
    // <assets|data>/<ns>/<path>. The data half was added with PackType and has
    // to be honoured here as well, or a zipped data pack resolves nothing.
    const char* const root = location.type == PackType::ServerData ? "data/" : "assets/";
    return root + location.space + "/" + location.path;
}

} // namespace

ZipResourcePackProvider::ZipResourcePackProvider(std::filesystem::path zipPath,
                                                 std::filesystem::path cacheRoot)
    : impl_(std::make_unique<Impl>(std::move(zipPath), std::move(cacheRoot))) {}

ZipResourcePackProvider::~ZipResourcePackProvider() = default;
ZipResourcePackProvider::ZipResourcePackProvider(ZipResourcePackProvider&&) noexcept = default;
ZipResourcePackProvider&
ZipResourcePackProvider::operator=(ZipResourcePackProvider&&) noexcept = default;

bool ZipResourcePackProvider::valid() const { return impl_->opened; }

bool ZipResourcePackProvider::exists(const ResourceLocation& location) const {
    return impl_->opened && impl_->entries.contains(entryName(location));
}

std::filesystem::path ZipResourcePackProvider::locate(const ResourceLocation& location) const {
    if (!impl_->opened) {
        return {};
    }
    const std::string entry = entryName(location);
    if (!impl_->entries.contains(entry)) {
        // Absent from the archive: hand back the cache path it *would* occupy so
        // the shape matches the directory provider; exists() reports it missing.
        return impl_->cacheRoot / entry;
    }
    return impl_->extract(entry);
}

std::vector<std::byte> ZipResourcePackProvider::readBytes(const ResourceLocation& location) const {
    if (!impl_->opened) {
        return {};
    }
    const std::string entry = entryName(location);
    if (!impl_->entries.contains(entry)) {
        return {};
    }
    return impl_->read(entry);
}

std::vector<ResourceLocation> ZipResourcePackProvider::list(std::string_view space,
                                                            std::string_view pathPrefix) const {
    std::vector<ResourceLocation> result;
    if (!impl_->opened) {
        return result;
    }
    const std::string entryPrefix = "assets/" + std::string{space} + "/" + std::string{pathPrefix};
    for (const auto& entry : impl_->entries) {
        if (!entry.starts_with(entryPrefix)) {
            continue;
        }
        result.push_back(ResourceLocation{
            std::string{space}, entry.substr(std::string{"assets/"}.size() + space.size() + 1U)});
    }
    return result;
}

std::vector<PackLanguage> ZipResourcePackProvider::languages() const {
    return impl_->languages;
}

std::filesystem::path ZipResourcePackProvider::resourceRoot() const { return impl_->cacheRoot; }

} // namespace mc::assets
