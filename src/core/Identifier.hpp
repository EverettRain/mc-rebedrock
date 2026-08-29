#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mc::core {

// The registry namespace every piece of content in this project is filed under.
// Content that mirrors vanilla keeps its `minecraft:` name as a separate alias
// so translation keys and vanilla assets still resolve.
inline constexpr std::string_view kNamespace{"rebedrock"};
inline constexpr std::string_view kVanillaNamespace{"minecraft"};

// A namespaced registry key such as `rebedrock:oak_planks`, stored as its two
// halves so a registry entry costs two views and never builds a string at
// runtime.
struct Identifier final {
    std::string_view space{};
    std::string_view path{};

    [[nodiscard]] constexpr bool empty() const { return path.empty(); }
    [[nodiscard]] constexpr bool operator==(const Identifier&) const = default;

    // Accepts the full `space:path` form as well as a bare `path`, which is what
    // commands and test scenes tend to be typed with. The colon is found by hand
    // so the whole thing stays constant-evaluable on every standard library.
    [[nodiscard]] constexpr bool matches(std::string_view text) const {
        if (empty()) return false;
        std::size_t separator = text.size();
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == ':') {
                separator = index;
                break;
            }
        }
        if (separator == text.size()) return text == path;
        return text.substr(0, separator) == space && text.substr(separator + 1U) == path;
    }

    [[nodiscard]] std::string toString() const {
        std::string result{space};
        result.push_back(':');
        result.append(path);
        return result;
    }

    // Splits a `space:path` string into its two halves without allocating; a
    // bare `path` (no colon) yields an Identifier with an empty namespace, which
    // the caller resolves against whatever default namespace makes sense (the
    // registry treats it as "match any namespace"). This is the inverse of
    // matches(): everything that reads a key off a save file, a command or the
    // wire parses it here rather than re-splitting the colon by hand.
    [[nodiscard]] static constexpr Identifier parse(std::string_view text) {
        std::size_t separator = text.size();
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == ':') {
                separator = index;
                break;
            }
        }
        if (separator == text.size()) return Identifier{std::string_view{}, text};
        return Identifier{text.substr(0, separator), text.substr(separator + 1U)};
    }
};

} // namespace mc::core
