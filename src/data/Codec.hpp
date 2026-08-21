#pragma once

// A serde-style codec: a POD definition <-> a Json value, both directions, type
// safe and composable. This is D-1's answer to `CraftingSystem.cpp` transcribing
// `bread.json` into C++ by hand and `MiningSystem.cpp` answering drops with a
// `switch(block)` — the recipe/loot/tag data moves out into files and the engine
// keeps one reader/writer instead of a hand copy per data kind.
//
// Deliberately NOT Java `Codec`'s functional pipeline (map/xmap/flatXmap/
// dispatch over a `DynamicOps`). That indirection buys nothing in C++ and costs
// readability: here a type's codec is two plain functions with value semantics,
//
//     mc::data::Codec<T>::write(const T&) -> core::Json
//     mc::data::Codec<T>::read(const core::Json&, T&) -> bool
//
// found by the type, so a struct's codec just calls its fields' codecs. `read`
// returns false rather than throwing when the shape is wrong, so a malformed
// datapack file is skipped, not fatal — the same forward-compatible tolerance
// BlockTags already gives a tag that names blocks this build lacks.
//
// The primary template is left undefined on purpose: asking to encode a type
// with no Codec is a compile error naming that type, not a silent fallthrough.

#include "core/Identifier.hpp"
#include "core/Json.hpp"

#include <cmath>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mc::data {

template <typename T>
struct Codec; // primary template intentionally undefined

// --- scalar leaves -------------------------------------------------------

template <>
struct Codec<bool> {
    static core::Json write(bool value) { return core::Json{value}; }
    static bool read(const core::Json& json, bool& out) {
        if (json.type() != core::Json::Type::Boolean) return false;
        out = json.asBool();
        return true;
    }
};

// Every integer field (a recipe count, a burn time, a weight) shares one codec.
// bool is excluded because it is `std::integral` too and has its own leaf above.
template <typename T>
    requires(std::integral<T> && !std::same_as<T, bool>)
struct Codec<T> {
    static core::Json write(T value) { return core::Json{static_cast<double>(value)}; }
    static bool read(const core::Json& json, T& out) {
        if (!json.isNumber()) return false;
        // Round to the nearest integer so `4` and a tolerant `4.0` both land on 4.
        out = static_cast<T>(std::llround(json.asNumber()));
        return true;
    }
};

template <typename T>
    requires std::floating_point<T>
struct Codec<T> {
    static core::Json write(T value) { return core::Json{static_cast<double>(value)}; }
    static bool read(const core::Json& json, T& out) {
        if (!json.isNumber()) return false;
        out = static_cast<T>(json.asNumber());
        return true;
    }
};

template <>
struct Codec<std::string> {
    static core::Json write(const std::string& value) { return core::Json{value}; }
    static bool read(const core::Json& json, std::string& out) {
        if (!json.isString()) return false;
        out = json.asString();
        return true;
    }
};

// --- composites ----------------------------------------------------------

// A list encodes as a JSON array of its element codec; one bad element fails the
// whole read rather than silently truncating.
template <typename T>
struct Codec<std::vector<T>> {
    static core::Json write(const std::vector<T>& value) {
        core::Json::Array array;
        array.reserve(value.size());
        for (const auto& element : value) {
            array.push_back(Codec<T>::write(element));
        }
        return core::Json{std::move(array)};
    }
    static bool read(const core::Json& json, std::vector<T>& out) {
        if (!json.isArray()) return false;
        out.clear();
        out.reserve(json.size());
        for (std::size_t index = 0; index < json.size(); ++index) {
            T element{};
            if (!Codec<T>::read(json[index], element)) return false;
            out.push_back(std::move(element));
        }
        return true;
    }
};

// An optional encodes its value when present and JSON null when absent, so a
// field can be written explicitly empty. (A field that may be missing from the
// object entirely is ObjectReader::optionalField's job, below.)
template <typename T>
struct Codec<std::optional<T>> {
    static core::Json write(const std::optional<T>& value) {
        return value.has_value() ? Codec<T>::write(*value) : core::Json{};
    }
    static bool read(const core::Json& json, std::optional<T>& out) {
        if (json.isNull()) {
            out.reset();
            return true;
        }
        T value{};
        if (!Codec<T>::read(json, value)) return false;
        out = std::move(value);
        return true;
    }
};

// --- object helpers ------------------------------------------------------

// Builds a JSON object one field at a time. A struct's Codec::write threads this:
// each `.field` dispatches to the field type's own codec, so composition is a
// call chain, not a combinator graph. Member order is the call order.
class ObjectWriter final {
  public:
    template <typename T>
    ObjectWriter& field(std::string_view name, const T& value) {
        members_.emplace_back(std::string{name}, Codec<T>::write(value));
        return *this;
    }
    [[nodiscard]] core::Json take() { return core::Json{std::move(members_)}; }

  private:
    core::Json::Object members_;
};

// Reads named fields off a JSON object, accumulating one success bit. Once a
// required field is missing or mistyped `ok()` is false and the rest short
// circuit, so a struct's Codec::read is a call chain ending in `return r.ok()`.
class ObjectReader final {
  public:
    explicit ObjectReader(const core::Json& json) : json_(json), ok_(json.isObject()) {}

    // A field that must be present and well typed.
    template <typename T>
    ObjectReader& field(std::string_view name, T& out) {
        if (ok_) {
            if (!json_.contains(name) || !Codec<T>::read(json_[name], out)) {
                ok_ = false;
            }
        }
        return *this;
    }

    // A field that may be absent: a missing key leaves `out` at its default and
    // is not an error, but a present-and-mistyped value still fails the read.
    template <typename T>
    ObjectReader& optionalField(std::string_view name, T& out) {
        if (ok_ && json_.contains(name)) {
            if (!Codec<T>::read(json_[name], out)) {
                ok_ = false;
            }
        }
        return *this;
    }

    [[nodiscard]] bool ok() const { return ok_; }

  private:
    const core::Json& json_;
    bool ok_;
};

// --- free helpers --------------------------------------------------------

template <typename T>
[[nodiscard]] core::Json writeJson(const T& value) {
    return Codec<T>::write(value);
}

template <typename T>
[[nodiscard]] bool readJson(const core::Json& json, T& out) {
    return Codec<T>::read(json, out);
}

// The full serialise/parse cycle a datapack value actually travels: encode to a
// Json value, dump to text, parse the text, decode. Returns whether the decoded
// value equals the original — the round-trip property the acceptance test asserts
// and sabotage ① (a field written under one key and read under another) breaks.
template <typename T>
[[nodiscard]] bool roundTripsThroughText(const T& value) {
    const std::string text = Codec<T>::write(value).dump();
    const core::Json reparsed = core::Json::parse(text);
    T restored{};
    return Codec<T>::read(reparsed, restored) && restored == value;
}

} // namespace mc::data
