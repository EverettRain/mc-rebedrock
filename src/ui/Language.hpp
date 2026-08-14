#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::assets {
class ResourceProvider;
}

namespace mc::ui {

// Non-owning composite lookup used by description ids. `entries_` still owns
// ordinary JSON keys as strings; its transparent hash/equality can compare this
// three-part view directly, avoiding a temporary concatenated std::string.
struct TranslationKeyView final {
    std::string_view prefix;
    std::string_view space;
    std::string_view path;
};

struct TranslationKeyHash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& key) const noexcept {
        return (*this)(std::string_view{key});
    }
    [[nodiscard]] std::size_t operator()(const TranslationKeyView& key) const noexcept;
};

struct TranslationKeyEqual final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left,
                                  std::string_view right) const noexcept {
        return left == right;
    }
    [[nodiscard]] bool operator()(const std::string& left,
                                  const TranslationKeyView& right) const noexcept;
    [[nodiscard]] bool operator()(const TranslationKeyView& left,
                                  const std::string& right) const noexcept {
        return (*this)(right, left);
    }
};

// The language codes the game ships translations for. en_us is the built-in
// fallback and never needs a file on disk.
inline constexpr const char* kDefaultLanguageCode = "en_us";

struct LanguageInfo final {
    std::string code;
    std::string name;
    std::string region;
    bool bidirectional = false;

    [[nodiscard]] std::string displayName() const;
};

// Reads only the small language block in each pack.mcmeta. In particular this
// does not enumerate or parse lang/*.json, so a language menu with hundreds of
// translations remains effectively constant-time to open.
[[nodiscard]] std::vector<LanguageInfo>
availableLanguages(const assets::ResourceProvider& provider);

[[nodiscard]] std::vector<std::string>
availableLanguageCodes(const std::filesystem::path& localizationRoot);

[[nodiscard]] std::vector<std::string>
availableLanguageCodes(const assets::ResourceProvider& provider);

// A flat Minecraft-style translation table: "block.minecraft.stone" -> "石头".
class Language final {
  public:
    Language() = default;

    // Parses a vanilla <code>.json language file. Throws on unreadable or
    // malformed input so the caller can fall back to English.
    [[nodiscard]] static Language fromFile(const std::filesystem::path& file);
    // Parses language JSON already in memory. The merged path uses this so a
    // zipped pack never has to materialise its language files on disk.
    [[nodiscard]] static Language fromJsonText(std::string_view text);
    [[nodiscard]] static Language fromProvider(const assets::ResourceProvider& provider,
                                               std::string_view code);

    [[nodiscard]] const std::string& code() const { return code_; }
    void setCode(std::string code) { code_ = std::move(code); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // Returns the translation for the key, or the fallback when the key is
    // missing. The fallback keeps English text visible for the handful of
    // strings vanilla has no key for.
    [[nodiscard]] std::string_view translate(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] std::string_view translate(std::string_view prefix, std::string_view space,
                                             std::string_view path,
                                             std::string_view fallback) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    // The 256-glyph unicode font pages every translated string needs, so the
    // renderer can upload only those.
    [[nodiscard]] std::set<int> requiredUnicodePages() const;

  private:
    std::string code_ = kDefaultLanguageCode;
    std::unordered_map<std::string, std::string, TranslationKeyHash, TranslationKeyEqual> entries_;
};

// Formats the `%s` / `%1$s` placeholders used by Java language JSON. `%%`
// becomes one literal percent sign. Keeping this independent from rendering
// lets option labels use `options.generic_value` and `options.percent_value`
// instead of hard-coding English punctuation/order around translated pieces.
[[nodiscard]] std::string formatTranslation(
    std::string_view pattern, std::span<const std::string_view> arguments);

struct LanguageLoadResult final {
    std::string code;
    Language language;
    std::string error;
};

// Prepares the selected translation stack off the render thread. The result is
// applied by the caller at a frame boundary, keeping the old Language valid and
// visible until the replacement is complete.
class AsyncLanguageLoader final {
  public:
    explicit AsyncLanguageLoader(const assets::ResourceProvider& provider);
    ~AsyncLanguageLoader() = default;

    AsyncLanguageLoader(const AsyncLanguageLoader&) = delete;
    AsyncLanguageLoader& operator=(const AsyncLanguageLoader&) = delete;

    [[nodiscard]] bool start(std::string code);
    [[nodiscard]] std::optional<LanguageLoadResult> poll();
    [[nodiscard]] bool busy() const { return busy_; }

  private:
    const assets::ResourceProvider* provider_ = nullptr;
    std::future<LanguageLoadResult> pending_;
    bool busy_ = false;
};

// The Minecraft translation key for a namespaced identifier, for example
// ("block", "minecraft", "stone") -> "block.minecraft.stone". The two halves
// come straight out of a registry entry's core::Identifier.
[[nodiscard]] std::string translationKey(std::string_view prefix, std::string_view space,
                                         std::string_view path);

} // namespace mc::ui
