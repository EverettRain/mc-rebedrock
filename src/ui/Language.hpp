#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::ui {

// The language codes the game ships translations for. en_us is the built-in
// fallback and never needs a file on disk.
inline constexpr const char* kDefaultLanguageCode = "en_us";

[[nodiscard]] std::vector<std::string> availableLanguageCodes(
    const std::filesystem::path& localizationRoot);

// A flat Minecraft-style translation table: "block.minecraft.stone" -> "石头".
class Language final {
  public:
    Language() = default;

    // Parses a vanilla <code>.json language file. Throws on unreadable or
    // malformed input so the caller can fall back to English.
    [[nodiscard]] static Language fromFile(const std::filesystem::path& file);

    [[nodiscard]] const std::string& code() const { return code_; }
    void setCode(std::string code) { code_ = std::move(code); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // Returns the translation for the key, or the fallback when the key is
    // missing. The fallback keeps English text visible for the handful of
    // strings vanilla has no key for.
    [[nodiscard]] std::string_view translate(
        std::string_view key,
        std::string_view fallback) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    // The 256-glyph unicode font pages every translated string needs, so the
    // renderer can upload only those.
    [[nodiscard]] std::set<int> requiredUnicodePages() const;

  private:
    std::string code_ = kDefaultLanguageCode;
    std::unordered_map<std::string, std::string> entries_;
};

// The Minecraft translation key for a namespaced identifier, for example
// ("block", "minecraft", "stone") -> "block.minecraft.stone". The two halves
// come straight out of a registry entry's core::Identifier.
[[nodiscard]] std::string translationKey(
    std::string_view prefix, std::string_view space, std::string_view path);

} // namespace mc::ui
