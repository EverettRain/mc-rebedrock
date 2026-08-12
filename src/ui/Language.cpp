#include "ui/Language.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "ui/TextFont.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mc::ui {

std::string LanguageInfo::displayName() const {
    if (region.empty()) {
        return name.empty() ? code : name;
    }
    return (name.empty() ? code : name) + " (" + region + ")";
}

std::vector<LanguageInfo> availableLanguages(const assets::ResourceProvider& provider) {
    std::vector<LanguageInfo> result;
    for (auto language : provider.languages()) {
        result.push_back(LanguageInfo{std::move(language.code), std::move(language.name),
                                      std::move(language.region), language.bidirectional});
    }
    if (std::ranges::find(result, kDefaultLanguageCode, &LanguageInfo::code) == result.end()) {
        result.push_back(LanguageInfo{kDefaultLanguageCode, "English", "United States", false});
    }
    std::ranges::sort(result, {}, &LanguageInfo::code);
    return result;
}

std::vector<std::string> availableLanguageCodes(const assets::ResourceProvider& provider) {
    std::vector<std::string> codes;
    for (auto language : availableLanguages(provider)) {
        codes.push_back(std::move(language.code));
    }
    return codes;
}

std::vector<std::string> availableLanguageCodes(const std::filesystem::path& localizationRoot) {
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

Language Language::fromProvider(const assets::ResourceProvider& provider, std::string_view code) {
    Language language;
    language.code_ = std::string{code};
    const auto mergeStack = [&](const assets::ResourceLocation& location) {
        for (const auto& file : provider.locateAll(location)) {
            const Language layer = fromFile(file);
            for (const auto& [key, value] : layer.entries_) {
                language.entries_.insert_or_assign(key, value);
            }
        }
    };
    // Vanilla always builds en_us first, then overlays the selected language.
    if (provider.locateAll(assets::lang("en_us.json")).empty()) {
        throw std::runtime_error("The resource stack has no lang/en_us.json");
    }
    mergeStack(assets::lang("en_us.json"));
    if (code != kDefaultLanguageCode) {
        if (provider.locateAll(assets::lang(std::string{code} + ".json")).empty()) {
            throw std::runtime_error("The resource stack has no lang/" + std::string{code} +
                                     ".json");
        }
        mergeStack(assets::lang(std::string{code} + ".json"));
    }

    // 26.1 applies deprecated.json after loading: removed keys disappear and
    // renamed keys move to their current IDs.
    for (const auto& file :
         provider.locateAll(assets::ResourceLocation{"minecraft", "lang/deprecated.json"})) {
        std::ifstream input{file, std::ios::binary};
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto document = core::Json::parse(buffer.str());
        const auto& removed = document["removed"];
        if (removed.isArray()) {
            for (std::size_t index = 0; index < removed.size(); ++index) {
                if (removed[index].isString()) {
                    language.entries_.erase(removed[index].asString());
                }
            }
        }
        const auto& renamed = document["renamed"];
        if (renamed.isObject()) {
            for (const auto& [from, to] : renamed.asObject()) {
                if (!to.isString()) {
                    continue;
                }
                auto node = language.entries_.extract(from);
                if (!node.empty()) {
                    language.entries_.erase(to.asString());
                    node.key() = to.asString();
                    language.entries_.insert(std::move(node));
                }
            }
        }
    }
    return language;
}

std::string_view Language::translate(std::string_view key, std::string_view fallback) const {
    const auto found = entries_.find(std::string{key});
    if (found == entries_.end() || found->second.empty()) {
        return fallback;
    }
    return found->second;
}

bool Language::contains(std::string_view key) const { return entries_.contains(std::string{key}); }

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

AsyncLanguageLoader::AsyncLanguageLoader(const assets::ResourceProvider& provider)
    : provider_(&provider) {}

bool AsyncLanguageLoader::start(std::string code) {
    if (busy_ || provider_ == nullptr || code.empty()) {
        return false;
    }
    busy_ = true;
    const auto* provider = provider_;
    pending_ = std::async(std::launch::async, [provider, code = std::move(code)]() mutable {
        LanguageLoadResult result;
        result.code = std::move(code);
        try {
            result.language = Language::fromProvider(*provider, result.code);
        } catch (const std::exception& exception) {
            result.error = exception.what();
        }
        return result;
    });
    return true;
}

std::optional<LanguageLoadResult> AsyncLanguageLoader::poll() {
    if (!busy_ || !pending_.valid() ||
        pending_.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
        return std::nullopt;
    }
    busy_ = false;
    try {
        return pending_.get();
    } catch (const std::exception& exception) {
        LanguageLoadResult result;
        result.error = exception.what();
        return result;
    }
}

std::string translationKey(std::string_view prefix, std::string_view space, std::string_view path) {
    std::string key{prefix};
    key.push_back('.');
    key.append(space);
    key.push_back('.');
    key.append(path);
    return key;
}

} // namespace mc::ui
