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
        // A server-data entry too: the data half lives under a different root,
        // and a reader that assumes `assets/` resolves nothing for it.
        const char* pickaxeTag = R"({"values":["minecraft:stone"]})";
        assert(mz_zip_writer_add_mem(&zip, "data/minecraft/tags/block/mineable/pickaxe.json",
                                     pickaxeTag, std::strlen(pickaxeTag),
                                     MZ_DEFAULT_COMPRESSION) != MZ_FALSE);
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

    // --- The regression that made the whole streaming change a no-op: consumers
    // hold a LayeredResourceProvider, not the zip provider directly. Until it
    // overrode readBytes/readAllBytes the base implementations routed through
    // locate(), so every read still extracted to `.packcache` and nothing was
    // actually served from memory. Asserting on the cache directory is the only
    // way to see that — the bytes come back correct either way. ---
    {
        const fs::path layeredCache = tmp / "layered-cache";
        fs::remove_all(layeredCache);
        DirectoryResourceProvider bundled{tmp / "bundled"};
        ZipResourcePackProvider zipPack{zipPath, layeredCache};
        assert(zipPack.valid());
        const LayeredResourceProvider layered{bundled, {&zipPack}};

        // A plain resource...
        const auto stoneBytes = layered.readBytes(textures("block/stone.png"));
        assert(!stoneBytes.empty());
        // ...and a merged one, which goes through readAllBytes.
        const auto merged = layered.readAllBytes(textures("block/stone.png"));
        assert(merged.size() == 1U);
        assert(merged[0].size() == stoneBytes.size());
        // A server-data resource through the stack too.
        assert(!layered.readBytes(data("tags/block/mineable/pickaxe.json")).empty());

        // Nothing was written to disk. This is the assertion that fails if the
        // layered provider falls back to locate().
        assert(!fs::exists(layeredCache / "assets"));
        assert(!fs::exists(layeredCache / "data"));

        // locate() still extracts — it has no other way to answer — which is
        // why the remaining path-based consumer keeps the cache alive.
        static_cast<void>(layered.locate(textures("block/stone.png")));
        assert(fs::exists(layeredCache / "assets" / "minecraft" / "textures" / "block" /
                          "stone.png"));
    }
    fs::remove_all(tmp, cleanup);
    // --- readBytes serves an entry straight out of the archive. This is what
    // lets a zipped pack be consumed without `.packcache`: `locate` can only
    // answer with an OS path, so satisfying it means extracting to disk, and
    // every asset a session touched ended up copied there. ---
    {
        const auto bytes = provider.readBytes(textures("block/stone.png"));
        assert(!bytes.empty());
        // Same content the extracting path yields, so the two agree.
        const auto extracted = provider.locate(textures("block/stone.png"));
        std::ifstream input{extracted, std::ios::binary};
        assert(static_cast<bool>(input));
        const std::string onDisk{std::istreambuf_iterator<char>{input},
                                 std::istreambuf_iterator<char>{}};
        assert(onDisk.size() == bytes.size());
        assert(std::memcmp(onDisk.data(), bytes.data(), bytes.size()) == 0);

        // A missing entry reads as empty rather than throwing or handing back a
        // path that does not exist.
        assert(provider.readBytes(textures("block/absent.png")).empty());
    }

    // --- The data half of a zipped pack resolves under `data/`, not `assets/`.
    // entryName hard-coded the assets prefix when PackType was introduced, so a
    // zipped data pack silently resolved nothing. ---
    {
        const auto clientPath = provider.locate(textures("block/stone.png")).string();
        assert(clientPath.find("assets") != std::string::npos);
        // The archive carries a real data/ entry, so a reader that looks under
        // `assets/` finds nothing and this comes back empty.
        const auto tagBytes = provider.readBytes(data("tags/block/mineable/pickaxe.json"));
        assert(!tagBytes.empty());
        const std::string tagText{reinterpret_cast<const char*>(tagBytes.data()),
                                  tagBytes.size()};
        assert(tagText.find("minecraft:stone") != std::string::npos);
        assert(provider.exists(data("tags/block/mineable/pickaxe.json")));
    }


    return 0;
}
