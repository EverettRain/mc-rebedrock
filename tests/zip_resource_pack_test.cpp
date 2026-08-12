#include "assets/ResourceLocation.hpp"
#include "assets/ZipResourcePack.hpp"

#include <miniz.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// A zip resource pack resolves the same ResourceLocations a directory pack does,
// extracting an entry to a cache path on first access. This builds a tiny pack
// zip with miniz's writer, then reads it back through the provider to pin
// presence checks and lazy extraction.
int main() {
    namespace fs = std::filesystem;
    using namespace mc::assets;

    const fs::path tmp = fs::temp_directory_path() / "rebedrock_zip_pack_test";
    std::error_code cleanup;
    fs::remove_all(tmp, cleanup);
    fs::create_directories(tmp);
    const fs::path zipPath = tmp / "pack.zip";
    const fs::path cache = tmp / "cache";

    // --- Build a standard-layout pack archive. ---
    {
        mz_zip_archive zip{};
        assert(mz_zip_writer_init_file(&zip, zipPath.string().c_str(), 0) != MZ_FALSE);
        const char* mcmeta =
            R"({"pack":{"pack_format":15},"language":{"en_us":{"name":"English","region":"United States","bidirectional":false},"ar_sa":{"name":"العربية","region":"العالم العربي","bidirectional":true}}})";
        const char* stone = "STONE-PIXELS";
        assert(mz_zip_writer_add_mem(&zip, "pack.mcmeta", mcmeta, std::strlen(mcmeta),
                                     MZ_DEFAULT_COMPRESSION) != MZ_FALSE);
        assert(mz_zip_writer_add_mem(&zip, "assets/minecraft/textures/block/stone.png", stone,
                                     std::strlen(stone), MZ_DEFAULT_COMPRESSION) != MZ_FALSE);
        assert(mz_zip_writer_finalize_archive(&zip) != MZ_FALSE);
        mz_zip_writer_end(&zip);
    }

    ZipResourcePackProvider provider{zipPath, cache};
    assert(provider.valid());
    const auto languages = provider.languages();
    assert(languages.size() == 2U);
    assert(languages[1].code == "ar_sa");
    assert(languages[1].bidirectional);
    // Reading pack.mcmeta for the catalog happens in memory and must not
    // defeat lazy extraction by unpacking metadata or unrelated resources.
    assert(!fs::exists(cache / "pack.mcmeta"));

    // --- Presence follows the archive's entries. ---
    assert(provider.exists(textures("block/stone.png")));
    assert(!provider.exists(textures("block/dirt.png")));

    // --- Locating an entry extracts it to the cache and returns that path. ---
    const auto located = provider.locate(textures("block/stone.png"));
    assert(fs::exists(located));
    // It lands under the cache root, mirroring the standard layout.
    assert(located == cache / "assets" / "minecraft" / "textures" / "block" / "stone.png");
    {
        std::ifstream in(located, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
        assert(content == "STONE-PIXELS");
    }

    // --- A second locate is served from the cache (still the same path/content). ---
    assert(provider.locate(textures("block/stone.png")) == located);

    // --- An absent entry yields a path but reports missing, so a layered stack
    //     falls through to the next provider. ---
    assert(!provider.exists(textures("block/dirt.png")));
    const auto absent = provider.locate(textures("block/dirt.png"));
    assert(!fs::exists(absent));

    fs::remove_all(tmp, cleanup);
    return 0;
}
