#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>

// Every consumer asks the provider for a file instead of rebuilding the layout
// by hand, so this pins each mapping to the exact physical path it must produce.
// If a later change moves a file, the mismatch shows up here rather than as a
// blank texture or a silent missing sound that only a GPU run would reveal.
//
// The built-in provider now owns rebedrock's OWN assets only. Everything
// Mojang-shaped — textures, sounds, the font, the `minecraft` lang tables — must
// resolve to an empty path so the layered stack falls through to the player's
// resource pack; the bundled `resources/vanilla/<version>/…` tree it used to map is
// gone. Those empties are asserted below: a provider that starts placing vanilla
// files again would shadow the pack.
int main() {
    using namespace mc::assets;
    namespace fs = std::filesystem;

    const fs::path root{"resources"};
    const DirectoryResourceProvider provider{root};

    assert(provider.resourceRoot() == root);

    // --- Vanilla content belongs to the pack, never to the built-in root. ---
    // The whole `textures/minecraft/<sub>` family (block art, gui, colormap,
    // misc, the environment sheet, the bitmap font pages), the sounds and the
    // `minecraft` translation tables all resolve to nothing here.
    assert(provider.locate(textures("block/stone.png")).empty());
    assert(provider.locate(textures("environment/rain.png")).empty());
    assert(provider.locate(textures("colormap/grass.png")).empty());
    assert(provider.locate(textures("gui/widgets.png")).empty());
    assert(provider.locate(textures("misc/underwater.png")).empty());
    assert(provider.locate(textures("font/ascii.png")).empty());
    assert(provider.locate(sounds("dig/stone1.ogg")).empty());
    assert(provider.locate(lang("en_us.json")).empty());
    assert(provider.locate(font("glyph_sizes.bin")).empty());
    // A namespace other than minecraft is still pack content, not ours.
    assert(provider.locate(textures("block/custom.png", "examplemod")).empty());
    // Nothing vanilla can be found through it either, so the layered stack's
    // `exists()` probe always defers to the packs above it.
    assert(!provider.exists(textures("block/stone.png")));

    // --- ReBedrock's own translations keep their namespace under lang/. ---
    assert(provider.locate(lang("en_us.json", "rebedrock")) ==
           root / "lang" / "rebedrock" / "en_us.json");

    // --- This project's own assets sit directly under the resources root. ---
    assert(provider.locate(ResourceLocation{"rebedrock", "animation/pig.animation.json"}) ==
           root / "animation" / "pig.animation.json");
    assert(provider.locate(ResourceLocation{"rebedrock", "entity/zombie/zombie.png"}) ==
           root / "entity" / "zombie" / "zombie.png");

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
