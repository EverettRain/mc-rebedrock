#include "assets/ResourceProvider.hpp"
#include "ui/Language.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {

// A catalog-only provider makes the regression test behavioural: the old
// implementation called list("lang/") and would throw here, while the 26.1
// path must use only pack.mcmeta metadata via languages().
class CatalogOnlyProvider final : public mc::assets::ResourceProvider {
  public:
    [[nodiscard]] std::filesystem::path
    locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation&) const override { return false; }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view, std::string_view,
        mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
        throw std::runtime_error("language JSON enumeration is forbidden");
    }
    [[nodiscard]] std::vector<mc::assets::PackLanguage> languages() const override {
        return {{"fr_fr", "Français", "France", false}};
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
};

class DelayedLanguageProvider final : public mc::assets::ResourceProvider {
  public:
    explicit DelayedLanguageProvider(std::filesystem::path english) : english_(std::move(english)) {}

    [[nodiscard]] std::filesystem::path
    locate(const mc::assets::ResourceLocation& location) const override {
        return location == mc::assets::lang("en_us.json") ? english_ : std::filesystem::path{};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& location) const override {
        return location == mc::assets::lang("en_us.json");
    }
    [[nodiscard]] std::vector<std::filesystem::path>
    locateAll(const mc::assets::ResourceLocation& location) const override {
        if (location != mc::assets::lang("en_us.json")) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        return {english_};
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }

  private:
    std::filesystem::path english_;
};

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace mc;

    const CatalogOnlyProvider catalogOnly;
    const auto catalogOnlyCodes = ui::availableLanguageCodes(catalogOnly);
    assert((catalogOnlyCodes == std::vector<std::string>{"en_us", "fr_fr"}));

    const fs::path temporary = fs::temp_directory_path() / "rebedrock_language_provider_test";
    std::error_code cleanup;
    fs::remove_all(temporary, cleanup);
    const auto write = [](const fs::path& file, std::string_view contents) {
        fs::create_directories(file.parent_path());
        std::ofstream output{file};
        output << contents;
    };
    const auto langFile = [&](std::string_view pack, std::string_view name) {
        return temporary / pack / "assets" / "minecraft" / "lang" / name;
    };
    const auto projectLangFile = [&](std::string_view pack, std::string_view name) {
        return temporary / pack / "assets" / "rebedrock" / "lang" / name;
    };

    write(temporary / "base" / "pack.mcmeta",
          R"({"pack":{"pack_format":1},"language":{"en_us":{"name":"English","region":"United States","bidirectional":false}}})");
    write(temporary / "low" / "pack.mcmeta",
          R"({"pack":{"pack_format":1},"language":{"zh_cn":{"name":"简体中文","region":"中国","bidirectional":false}}})");
    write(temporary / "high" / "pack.mcmeta",
          R"({"pack":{"pack_format":1},"language":{"zh_cn":{"name":"中文","region":"中国大陆","bidirectional":false},"zz_test":{"name":"Metadata only","region":"Invalid JSON below","bidirectional":false}}})");

    write(langFile("base", "en_us.json"),
          R"({"base.only":"English fallback","shared":"base","old.key":"Moved","item.minecraft.apple":"Apple","block.minecraft.stone":"Stone"})");
    write(langFile("low", "en_us.json"), R"({"shared":"low"})");
    write(langFile("low", "zh_cn.json"), R"({"shared":"低"})");
    write(langFile("high", "zh_cn.json"), R"({"shared":"高"})");
    write(langFile("base", "fr_fr.json"), R"({"shared":"français"})");
    write(projectLangFile("base", "en_us.json"),
          R"({"options.rebedrock.test":"Project English","project.layer":"base"})");
    write(projectLangFile("low", "zh_cn.json"),
          R"({"options.rebedrock.test":"项目中文","project.layer":"low"})");
    write(projectLangFile("high", "zh_cn.json"), R"({"project.layer":"high"})");
    // Catalog construction must not touch this declared language file. Before
    // the pack.mcmeta catalog path this made the language menu parse every
    // translation and fail (or freeze on real packs with 144 large files).
    write(langFile("high", "zz_test.json"), "this is intentionally not json");
    write(langFile("base", "deprecated.json"), R"({"removed":[],"renamed":{"old.key":"new.key"}})");

    // The loader must return control to the render thread immediately even
    // when provider I/O is slow. A synchronous implementation takes at least
    // 400 ms here because en_us is probed and then loaded.
    const DelayedLanguageProvider delayed{langFile("base", "en_us.json")};
    ui::AsyncLanguageLoader loader{delayed};
    const auto loadStarted = std::chrono::steady_clock::now();
    if (!loader.start("en_us")) {
        return 10;
    }
    const auto startDuration = std::chrono::steady_clock::now() - loadStarted;
    if (startDuration >= std::chrono::milliseconds{100}) {
        return 11;
    }
    std::optional<ui::LanguageLoadResult> prepared;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!prepared.has_value() && std::chrono::steady_clock::now() < deadline) {
        prepared = loader.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!prepared.has_value()) {
        return 12;
    }
    if (!prepared->error.empty()) {
        return 13;
    }
    if (prepared->language.translate("base.only", "missing") != "English fallback") {
        return 14;
    }

    const assets::StandardPackResourceProvider base{temporary / "base"};
    const assets::StandardPackResourceProvider low{temporary / "low"};
    const assets::StandardPackResourceProvider high{temporary / "high"};
    const assets::LayeredResourceProvider provider{base, {&high, &low}};

    const auto codes = ui::availableLanguageCodes(provider);
    assert(std::ranges::find(codes, "en_us") != codes.end());
    assert(std::ranges::find(codes, "zh_cn") != codes.end());
    assert(std::ranges::find(codes, "zz_test") != codes.end());
    const auto catalog = ui::availableLanguages(provider);
    const auto chinese = std::ranges::find(catalog, "zh_cn", &ui::LanguageInfo::code);
    assert(chinese != catalog.end());
    assert(chinese->displayName() == "中文 (中国大陆)");

    const auto language = ui::Language::fromProvider(provider, "zh_cn");
    assert(language.translate("base.only", "missing") == "English fallback");
    assert(language.translate("shared", "missing") == "高");
    // 26.1 loads all namespaces for en_us and the selected locale. ReBedrock's
    // English layer is therefore the fallback for custom fields, selected
    // project translations overlay it, and ordinary pack priority still wins.
    assert(language.translate("options.rebedrock.test", "missing") == "项目中文");
    assert(language.translate("project.layer", "missing") == "high");
    assert(!language.contains("old.key"));
    assert(language.translate("new.key", "missing") == "Moved");
    // Structured description-id lookup hashes the three string views as though
    // they were one JSON key, without allocating a concatenated key per call.
    assert(language.translate("item", "minecraft", "apple", "missing") == "Apple");
    assert(language.translate("block", "minecraft", "stone", "missing") == "Stone");
    assert(language.translate("item", "rebedrock", "missing", "fallback") == "fallback");

    // A vanilla locale for which ReBedrock has no authored file remains fully
    // usable: vanilla fields use that locale and only project-specific fields
    // fall back to the project English layer.
    const auto french = ui::Language::fromProvider(provider, "fr_fr");
    assert(french.translate("shared", "missing") == "français");
    assert(french.translate("options.rebedrock.test", "missing") == "Project English");

    const std::array<std::string_view, 2> optionArguments{"界面尺寸", "自动"};
    assert(ui::formatTranslation("%s：%s", optionArguments) == "界面尺寸：自动");
    const std::array<std::string_view, 2> reorderedArguments{"first", "second"};
    assert(ui::formatTranslation("%2$s / %1$s / 100%%", reorderedArguments) ==
           "second / first / 100%");

    fs::remove_all(temporary, cleanup);
    return 0;
}
