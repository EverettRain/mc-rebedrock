#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mc::core {

// A minimal, dependency-free JSON value used to drive the data-driven
// animation pipeline. The project already vendors single-header libraries
// (miniaudio, stb_image) rather than pulling large dependencies, so the
// animation system parses its Bedrock-style geometry and animation files
// with this self-contained reader instead of a third-party JSON library.
//
// The reader supports the full JSON grammar (objects, arrays, strings with
// escapes, numbers, booleans and null). Object member order is preserved so
// diagnostics and round-trips stay stable.
class Json final {
  public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    using Array = std::vector<Json>;
    // Ordered map: Bedrock geometry relies on stable bone iteration order.
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    explicit Json(bool value) : type_(Type::Boolean), boolean_(value) {}
    explicit Json(double value) : type_(Type::Number), number_(value) {}
    explicit Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    explicit Json(Array value) : type_(Type::Array), array_(std::move(value)) {}
    explicit Json(Object value) : type_(Type::Object), object_(std::move(value)) {}

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool isNull() const { return type_ == Type::Null; }
    [[nodiscard]] bool isNumber() const { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const { return type_ == Type::Object; }

    [[nodiscard]] bool asBool(bool fallback = false) const {
        return type_ == Type::Boolean ? boolean_ : fallback;
    }
    [[nodiscard]] double asNumber(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    [[nodiscard]] float asFloat(float fallback = 0.0F) const {
        return type_ == Type::Number ? static_cast<float>(number_) : fallback;
    }
    [[nodiscard]] const std::string& asString() const { return string_; }
    [[nodiscard]] const Array& asArray() const { return array_; }
    [[nodiscard]] const Object& asObject() const { return object_; }

    // Object lookup. Returns a shared static null value when the key or the
    // underlying type is missing so call sites can chain safely.
    [[nodiscard]] const Json& operator[](std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    // Array lookup with bounds check; out-of-range returns null.
    [[nodiscard]] const Json& operator[](std::size_t index) const;
    [[nodiscard]] std::size_t size() const;

    // Parses UTF-8 text. Throws std::runtime_error with line/column on error.
    [[nodiscard]] static Json parse(std::string_view text);

  private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

} // namespace mc::core
