#include "ui/Language.hpp"

#include "core/Json.hpp"
#include "ui/TextFont.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mc::ui {

std::vector<std::string> availableLanguageCodes(
    const std::filesystem::path& localizationRoot) {
    std::vector<std::string> codes{kDefaultLanguageCode};
    std::error_code error;
    if (!std::filesystem::is_directory(localizationRoot, error)) {
        return codes;
    }
    for (const auto& entry : std::filesystem::directory_iterator(localizationRoot, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        auto code = entry.path().stem().string();
        if (code != kDefaultLanguageCode) {
            codes.push_back(std::move(code));
        }
    }
    std::sort(codes.begin() + 1, codes.end());
    return codes;
}

Language Language::fromFile(const std::filesystem::path& file) {
    std::ifstream input{file, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Unable to open language file " + file.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto document = core::Json::parse(buffer.str());
    if (!document.isObject()) {
        throw std::runtime_error("Language file is not a JSON object: " + file.string());
    }
    Language language;
    language.code_ = file.stem().string();
    language.entries_.reserve(document.asObject().size());
    for (const auto& [key, value] : document.asObject()) {
        if (value.isString()) {
            language.entries_.insert_or_assign(key, value.asString());
        }
    }
    return language;
}

std::string_view Language::translate(
    std::string_view key,
    std::string_view fallback) const {
    const auto found = entries_.find(std::string{key});
    if (found == entries_.end() || found->second.empty()) {
        return fallback;
    }
    return found->second;
}

bool Language::contains(std::string_view key) const {
    return entries_.contains(std::string{key});
}

std::set<int> Language::requiredUnicodePages() const {
    std::set<int> pages;
    for (const auto& [key, value] : entries_) {
        for (const char32_t codepoint : decodeUtf8(value)) {
            if (codepoint <= 0xFFFF) {
                pages.insert(static_cast<int>(codepoint >> 8U));
            }
        }
    }
    return pages;
}

std::string translationKey(
    std::string_view prefix, std::string_view space, std::string_view path) {
    std::string key{prefix};
    key.push_back('.');
    key.append(space);
    key.push_back('.');
    key.append(path);
    return key;
}

} // namespace mc::ui
