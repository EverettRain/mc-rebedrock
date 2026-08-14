#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>

// The provider is a behaviour-preserving refactor: every consumer that used to
// build a path by walking `blockTextureRoot.parent_path()` now asks the provider
// instead, and must get the *same* file. So this pins each mapping to the exact
// physical path the old code produced against the current
// `resources/vanilla/1.16.1/…` layout. If a later change (or the P-B standard
// pack provider) moves a file, the mismatch shows up here rather than as a blank
// texture or a silent missing sound that only a GPU run would reveal.
int main() {
    using namespace mc::assets;
    namespace fs = std::filesystem;

    const fs::path root{"resources"};
    const DirectoryResourceProvider provider{root};
    const fs::path vanilla = root / "vanilla" / "1.16.1";

    assert(provider.resourceRoot() == root);
    assert(provider.vanillaRoot() == vanilla);

    // --- Vanilla textures: category folder, then namespace, then the rest. ---
    // This is the whole `textures/minecraft/<sub>` family: block art, gui,
    // colormap, misc, the environment sheet and the bitmap font pages.
    assert(provider.locate(textures("block/stone.png")) ==
           vanilla / "textures" / "minecraft" / "block" / "stone.png");
    assert(provider.locate(textures("environment/rain.png")) ==
           vanilla / "textures" / "minecraft" / "environment" / "rain.png");
    assert(provider.locate(textures("colormap/grass.png")) ==
           vanilla / "textures" / "minecraft" / "colormap" / "grass.png");
    assert(provider.locate(textures("gui/widgets.png")) ==
           vanilla / "textures" / "minecraft" / "gui" / "widgets.png");
    assert(provider.locate(textures("misc/underwater.png")) ==
           vanilla / "textures" / "minecraft" / "misc" / "underwater.png");
    // The bitmap font pages are textures, so they carry the `textures/` category
    // even though they name the `font/` subfolder.
    assert(provider.locate(textures("font/ascii.png")) ==
           vanilla / "textures" / "minecraft" / "font" / "ascii.png");

    // --- Sounds live under `audio/…`, not `sounds/…`. ---
    assert(provider.locate(sounds("dig/stone1.ogg")) ==
           vanilla / "audio" / "minecraft" / "sounds" / "dig" / "stone1.ogg");

    // --- Translations live under `localization/…`. ---
    assert(provider.locate(lang("en_us.json")) ==
           vanilla / "localization" / "minecraft" / "en_us.json");
    assert(provider.locate(lang("en_us.json", "rebedrock")) ==
           root / "lang" / "rebedrock" / "en_us.json");

    // --- The glyph-width table is the lone tenant of the top-level `fonts/`. ---
    assert(provider.locate(font("glyph_sizes.bin")) ==
           vanilla / "fonts" / "minecraft" / "glyph_sizes.bin");

    // --- This project's own assets sit directly under the resources root. ---
    assert(provider.locate(ResourceLocation{"rebedrock", "animation/pig.animation.json"}) ==
           root / "animation" / "pig.animation.json");
    assert(provider.locate(ResourceLocation{"rebedrock", "entity/zombie/zombie.png"}) ==
           root / "entity" / "zombie" / "zombie.png");

    // --- A namespace other than minecraft threads through the layout. ---
    assert(provider.locate(textures("block/custom.png", "examplemod")) ==
           vanilla / "textures" / "examplemod" / "block" / "custom.png");

    // --- ResourceLocation parsing. ---
    assert((ResourceLocation::parse("minecraft:textures/block/stone.png") ==
            ResourceLocation{"minecraft", "textures/block/stone.png"}));
    // A bare path defaults to the minecraft namespace, as vanilla resolves it.
    assert((ResourceLocation::parse("textures/block/stone.png") ==
            ResourceLocation{"minecraft", "textures/block/stone.png"}));
    assert(textures("block/stone.png").toString() == "minecraft:textures/block/stone.png");

    // exists is just a filesystem probe over locate(); a made-up file is absent.
    assert(!provider.exists(textures("block/does_not_exist_zzz.png")));

    // --- A standard pack maps the same names with no category renames. ---
    // This is the payoff of ResourceLocation carrying the standard content path:
    // the provider that reads a real vanilla/third-party pack is one clean join,
    // `<pack>/assets/<namespace>/<path>`. Sounds sit under `sounds/`, not the
    // legacy `audio/`; translations under `lang/`, not `localization/`.
    {
        const fs::path pack{"my_pack"};
        const StandardPackResourceProvider standard{pack};
        assert(standard.locate(textures("block/stone.png")) ==
               pack / "assets" / "minecraft" / "textures" / "block" / "stone.png");
        assert(standard.locate(sounds("dig/stone1.ogg")) ==
               pack / "assets" / "minecraft" / "sounds" / "dig" / "stone1.ogg");
        assert(standard.locate(lang("en_us.json")) ==
               pack / "assets" / "minecraft" / "lang" / "en_us.json");
        assert(standard.locate(textures("block/custom.png", "examplemod")) ==
               pack / "assets" / "examplemod" / "textures" / "block" / "custom.png");
    }

    // --- A layered stack overrides only the files an overlay actually ships. ---
    {
        const fs::path tmp = fs::temp_directory_path() / "rebedrock_pack_layer_test";
        std::error_code cleanup;
        fs::remove_all(tmp, cleanup);
        const fs::path overlayBlock =
            tmp / "overlay" / "assets" / "minecraft" / "textures" / "block";
        fs::create_directories(overlayBlock);
        {
            std::ofstream file{overlayBlock / "stone.png"};
            file << "overlay";
        }

        const DirectoryResourceProvider base{tmp / "base"};
        const StandardPackResourceProvider overlay{tmp / "overlay"};
        const LayeredResourceProvider stack{base, {&overlay}};

        // stone.png exists in the overlay, so it wins.
        assert(stack.exists(textures("block/stone.png")));
        assert(stack.locate(textures("block/stone.png")) ==
               overlay.locate(textures("block/stone.png")));
        // dirt.png is in neither on disk, so it falls through to the base's path
        // (the overlay does not have it, so the stack resolves against base).
        assert(stack.locate(textures("block/dirt.png")) == base.locate(textures("block/dirt.png")));
        // The subtree root always comes from the base: project assets are never
        // in a pack.
        assert(stack.resourceRoot() == base.resourceRoot());

        // Stack-merged definitions are returned low-to-high even though the
        // resolver stores overlays high-to-low for ordinary top-file-wins
        // lookup. This ordering is what sounds.json merging consumes.
        const fs::path baseSounds = tmp / "base" / "sounds.json";
        const fs::path lowSounds = tmp / "low" / "assets" / "minecraft" / "sounds.json";
        const fs::path highSounds = tmp / "high" / "assets" / "minecraft" / "sounds.json";
        fs::create_directories(baseSounds.parent_path());
        fs::create_directories(lowSounds.parent_path());
        fs::create_directories(highSounds.parent_path());
        {
            std::ofstream file{baseSounds};
            file << "base";
        }
        {
            std::ofstream file{lowSounds};
            file << "low";
        }
        {
            std::ofstream file{highSounds};
            file << "high";
        }
        const StandardPackResourceProvider low{tmp / "low"};
        const StandardPackResourceProvider high{tmp / "high"};
        const LayeredResourceProvider merged{base, {&high, &low}};
        const auto definitions = merged.locateAll(ResourceLocation{"minecraft", "sounds.json"});
        assert((definitions == std::vector<fs::path>{baseSounds, lowSounds, highSounds}));
        const fs::path highLanguage = tmp / "high" / "assets" / "minecraft" / "lang" / "zh_cn.json";
        fs::create_directories(highLanguage.parent_path());
        {
            std::ofstream file{highLanguage};
            file << "{}";
        }
        const auto listed = merged.list("minecraft", "lang/");
        assert(std::ranges::find(listed, ResourceLocation{"minecraft", "lang/zh_cn.json"}) !=
               listed.end());

        fs::remove_all(tmp, cleanup);
    }

    return 0;
}
