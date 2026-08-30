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

namespace {

constexpr std::size_t kFnvOffset = sizeof(std::size_t) == 8U
    ? static_cast<std::size_t>(1469598103934665603ULL)
    : static_cast<std::size_t>(2166136261U);
constexpr std::size_t kFnvPrime = sizeof(std::size_t) == 8U
    ? static_cast<std::size_t>(1099511628211ULL)
    : static_cast<std::size_t>(16777619U);

void hashAppend(std::size_t& hash, std::string_view text) noexcept {
    for (const char character : text) {
        hash ^= static_cast<std::size_t>(static_cast<unsigned char>(character));
        hash *= kFnvPrime;
    }
}

} // namespace

std::size_t TranslationKeyHash::operator()(std::string_view key) const noexcept {
    std::size_t hash = kFnvOffset;
    hashAppend(hash, key);
    return hash;
}

std::size_t TranslationKeyHash::operator()(const TranslationKeyView& key) const noexcept {
    std::size_t hash = kFnvOffset;
    hashAppend(hash, key.prefix);
    hashAppend(hash, ".");
    hashAppend(hash, key.space);
    hashAppend(hash, ".");
    hashAppend(hash, key.path);
    return hash;
}

bool TranslationKeyEqual::operator()(const std::string& left,
                                     const TranslationKeyView& right) const noexcept {
    const std::size_t expectedSize =
        right.prefix.size() + right.space.size() + right.path.size() + 2U;
    if (left.size() != expectedSize || !std::string_view{left}.starts_with(right.prefix)) {
        return false;
    }
    std::size_t offset = right.prefix.size();
    if (left[offset++] != '.') return false;
    if (std::string_view{left}.substr(offset, right.space.size()) != right.space) return false;
    offset += right.space.size();
    if (left[offset++] != '.') return false;
    return std::string_view{left}.substr(offset) == right.path;
}

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

Language Language::fromJsonText(std::string_view text) {
    const auto document = core::Json::parse(text);
    if (!document.isObject()) {
        throw std::runtime_error("Language file is not a JSON object");
    }
    Language language;
    // 语言代码由调用方给出
    // 合并出来的层没有文件名可供提取，而那正是走路径的形式白送的一样东西
    language.entries_.reserve(document.asObject().size());
    for (const auto& [key, value] : document.asObject()) {
        if (value.isString()) {
            language.entries_.insert_or_assign(key, value.asString());
        }
    }
    return language;
}

Language Language::fromFile(const std::filesystem::path& file) {
    std::ifstream input{file, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Unable to open language file " + file.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Language language = fromJsonText(buffer.str());
    language.code_ = file.stem().string();
    return language;
}

Language Language::fromProvider(const assets::ResourceProvider& provider, std::string_view code) {
    Language language;
    language.code_ = std::string{code};
    const auto mergeStack = [&](const assets::ResourceLocation& location) {
        for (const auto& bytes : provider.readAllBytes(location)) {
            const Language layer = fromJsonText(
                std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            for (const auto& [key, value] : layer.entries_) {
                language.entries_.insert_or_assign(key, value);
            }
        }
    };
    // vanilla 总是先建好 en_us，再把选中的语言叠上去
    // 这里用 exists() 而不是 locateAll()
    // 要路径会让 zip 资源包把每个语言文件都解压一遍，只为回答它在不在
    if (!provider.exists(assets::lang("en_us.json"))) {
        throw std::runtime_error("The resource stack has no lang/en_us.json");
    }
    mergeStack(assets::lang("en_us.json"));
    if (code != kDefaultLanguageCode) {
        if (!provider.exists(assets::lang(std::string{code} + ".json"))) {
            throw std::runtime_error("The resource stack has no lang/" + std::string{code} +
                                     ".json");
        }
        mergeStack(assets::lang(std::string{code} + ".json"));
    }

    // ClientLanguage 会为每一个资源命名空间加载翻译
    // 本项目自己的选项因此放在 rebedrock 命名空间下，叠在 vanilla 之后
    // 这样它们能复用同一个选中的语言，而不必去改 Mojang 的语言文件
    // 只有英文是必需的：某个语言若没有本项目的翻译，项目字符串保持英文，vanilla 的键仍然是本地化的
    const auto mergeProjectLanguage = [&](std::string_view languageCode) {
        const auto location = assets::lang(std::string{languageCode} + ".json", "rebedrock");
        if (provider.exists(location)) {
            mergeStack(location);
        }
    };
    mergeProjectLanguage(kDefaultLanguageCode);
    if (code != kDefaultLanguageCode) {
        mergeProjectLanguage(code);
    }

    // 26.1 在加载之后套用 deprecated.json：被移除的键消失，被改名的键挪到它们现在的 ID 上
    for (const auto& bytes : provider.readAllBytes(
             assets::ResourceLocation{"minecraft", "lang/deprecated.json"})) {
        const auto document = core::Json::parse(
            std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
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
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second.empty()) {
        return fallback;
    }
    return found->second;
}

std::string_view Language::translate(std::string_view prefix, std::string_view space,
                                     std::string_view path, std::string_view fallback) const {
    const auto found = entries_.find(TranslationKeyView{prefix, space, path});
    if (found == entries_.end() || found->second.empty()) {
        return fallback;
    }
    return found->second;
}

bool Language::contains(std::string_view key) const { return entries_.contains(key); }

std::string formatTranslation(std::string_view pattern,
                              std::span<const std::string_view> arguments) {
    std::string output;
    output.reserve(pattern.size() + arguments.size() * 8U);
    std::size_t automaticIndex = 0U;
    for (std::size_t index = 0U; index < pattern.size();) {
        if (pattern[index] != '%') {
            output.push_back(pattern[index++]);
            continue;
        }
        if (index + 1U < pattern.size() && pattern[index + 1U] == '%') {
            output.push_back('%');
            index += 2U;
            continue;
        }

        std::size_t argumentIndex = automaticIndex;
        std::size_t tokenEnd = index + 1U;
        std::size_t numeric = 0U;
        bool numbered = false;
        while (tokenEnd < pattern.size() && pattern[tokenEnd] >= '0' &&
               pattern[tokenEnd] <= '9') {
            numbered = true;
            numeric = numeric * 10U + static_cast<std::size_t>(pattern[tokenEnd] - '0');
            ++tokenEnd;
        }
        if (numbered && tokenEnd < pattern.size() && pattern[tokenEnd] == '$') {
            ++tokenEnd;
            argumentIndex = numeric == 0U ? arguments.size() : numeric - 1U;
        } else if (numbered) {
            output.push_back(pattern[index++]);
            continue;
        }
        if (tokenEnd < pattern.size() && pattern[tokenEnd] == 's') {
            if (argumentIndex < arguments.size()) {
                output.append(arguments[argumentIndex]);
            }
            if (!numbered) {
                ++automaticIndex;
            }
            index = tokenEnd + 1U;
            continue;
        }
        output.push_back(pattern[index++]);
    }
    return output;
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
