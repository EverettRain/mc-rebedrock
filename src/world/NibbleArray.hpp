#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mc::world {

// Compact 4-bit storage used by the vanilla lighting engine. Uniform arrays
// keep no backing allocation, which is important for empty chunk sections.
class NibbleArray final {
  public:
    static constexpr std::size_t kValueCount = 16U * 16U * 16U;
    static constexpr std::size_t kByteCount = kValueCount / 2U;

    explicit NibbleArray(std::uint8_t value = 0U) : uniformValue_(clamp(value)) {}

    [[nodiscard]] std::uint8_t get(std::size_t index) const {
        if (index >= kValueCount) {
            throw std::out_of_range("NibbleArray index is outside 0..4095");
        }
        if (bytes_.empty()) return uniformValue_;
        const std::uint8_t packed = bytes_[index >> 1U];
        return (index & 1U) == 0U ? static_cast<std::uint8_t>(packed & 0x0FU)
                                  : static_cast<std::uint8_t>(packed >> 4U);
    }

    bool set(std::size_t index, std::uint8_t value) {
        if (index >= kValueCount) {
            throw std::out_of_range("NibbleArray index is outside 0..4095");
        }
        value = clamp(value);
        if (bytes_.empty()) {
            if (value == uniformValue_) return false;
            bytes_.assign(kByteCount, static_cast<std::uint8_t>(uniformValue_ |
                                                                 (uniformValue_ << 4U)));
        }
        std::uint8_t& packed = bytes_[index >> 1U];
        const std::uint8_t previous = (index & 1U) == 0U
                                          ? static_cast<std::uint8_t>(packed & 0x0FU)
                                          : static_cast<std::uint8_t>(packed >> 4U);
        if (previous == value) return false;
        if ((index & 1U) == 0U) {
            packed = static_cast<std::uint8_t>((packed & 0xF0U) | value);
        } else {
            packed = static_cast<std::uint8_t>((packed & 0x0FU) |
                                               static_cast<std::uint8_t>(value << 4U));
        }
        return true;
    }

    void fill(std::uint8_t value) {
        uniformValue_ = clamp(value);
        bytes_.clear();
    }

    [[nodiscard]] bool uniform() const { return bytes_.empty(); }

  private:
    [[nodiscard]] static constexpr std::uint8_t clamp(std::uint8_t value) {
        return static_cast<std::uint8_t>(value & 0x0FU);
    }

    std::vector<std::uint8_t> bytes_;
    std::uint8_t uniformValue_ = 0U;
};

} // namespace mc::world
