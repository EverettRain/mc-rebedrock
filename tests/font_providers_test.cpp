#include "assets/FontProviders.hpp"
#include "assets/ResourceProvider.hpp"
#include "ui/TextFont.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    using namespace mc::assets;

    const fs::path temporary = fs::temp_directory_path() / "rebedrock_font_providers_test";
    std::error_code cleanup;
    fs::remove_all(temporary, cleanup);
    const auto writeFont = [&](std::string_view name, std::string_view contents) {
        const fs::path file = temporary / "assets" / "minecraft" / "font" / name;
        fs::create_directories(file.parent_path());
        std::ofstream output{file};
        output << contents;
    };
    writeFont("default.json", R"({"providers":[
        {"type":"reference","id":"minecraft:include/space"},
        {"type":"reference","id":"minecraft:include/default","filter":{"uniform":false}},
        {"type":"reference","id":"minecraft:include/unifont"}
    ]})");
    writeFont("include/space.json", R"({"providers":[
        {"type":"space","advances":{" ":4,"‌":0}}
    ]})");
    writeFont("include/default.json", R"({"providers":[
        {"type":"bitmap","file":"minecraft:font/ascii.png","ascent":7,
         "chars":[" AB"]}
    ]})");
    writeFont("include/unifont.json", R"({"providers":[
        {"type":"unihex","hex_file":"minecraft:font/jp.zip","filter":{"jp":true}},
        {"type":"unihex","hex_file":"minecraft:font/unifont.zip",
         "size_overrides":[{"from":"一","to":"鿿","left":0,"right":15}]}
    ]})");

    const StandardPackResourceProvider resources{temporary};
    const auto providers = loadFontProviders(resources, "minecraft:default");
    assert(providers.size() == 3U);
    assert(providers[0].kind == FontProviderKind::Space);
    assert(providers[0].advances.size() == 2U);
    assert(providers[1].kind == FontProviderKind::Bitmap);
    assert(providers[1].file == textures("font/ascii.png"));
    assert(providers[1].chars[0][1] == U'A');
    assert(providers[2].kind == FontProviderKind::Unihex);
    assert(providers[2].file == font("unifont.zip"));
    assert(providers[2].sizeOverrides.size() == 1U);

    // uniform=true filters out include/default exactly as default.json requests.
    const auto uniform = loadFontProviders(resources, "minecraft:default", true, false);
    assert(uniform.size() == 2U);
    assert(uniform[0].kind == FontProviderKind::Space);
    assert(uniform[1].kind == FontProviderKind::Unihex);

    // Concrete bitmap providers participate in TextFont with first-provider
    // precedence; a later fallback cannot replace a glyph already supplied.
    mc::ui::TextFont textFont;
    mc::ui::FontGlyph first;
    first.advance = 6.0F;
    first.visible = true;
    mc::ui::FontGlyph fallback;
    fallback.advance = 9.0F;
    textFont.addBitmapGlyph(U'Á', first);
    textFont.addBitmapGlyph(U'Á', fallback);
    assert(textFont.glyph(U'Á').advance == 6.0F);

    fs::remove_all(temporary, cleanup);
    return 0;
}
