#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

// The byte layer every save block is built from: little-endian scalars, the
// self-describing {tag, size, version} frame, and the palettes that keep
// registry content out of the record bodies.
//
// This is deliberately not a port of NBT. NBT pays a boxed tag object, a string
// key and a hash map per compound; the same guarantees — self-description,
// forward compatibility, sparse storage — come out of a four-field frame that
// costs ten bytes once per block instead of per value. See
// [[save-format-nbt-guidance]].
namespace mc::persistence {

inline constexpr std::size_t kMaximumIdentifierLength = 256U;
// Every block starts with u32 tag + u32 size + u16 version.
inline constexpr std::size_t kBlockHeaderBytes = 10U;

template <typename Integer>
void appendInteger(std::vector<std::uint8_t>& bytes, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned converted = static_cast<Unsigned>(value);
    // Written a byte at a time rather than memcpy'd so the layout is
    // little-endian on every host, whatever the machine's own order is.
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(converted >> (index * 8U)));
    }
}

template <typename Integer>
[[nodiscard]] Integer readInteger(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    if (cursor + sizeof(Integer) > bytes.size()) {
        throw std::runtime_error("Save data ended unexpectedly");
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    // Accumulate in a type at least as wide as int: shifting a narrower one
    // promotes to int anyway, and assigning that back narrows on every step.
    using Wide = std::conditional_t<(sizeof(Unsigned) < sizeof(unsigned int)),
                                    unsigned int, Unsigned>;
    Wide value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Wide>(bytes[cursor++]) << (index * 8U);
    }
    return static_cast<Integer>(static_cast<Unsigned>(value));
}

inline void appendString(std::vector<std::uint8_t>& bytes, std::string_view text) {
    appendInteger(bytes, static_cast<std::uint16_t>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

[[nodiscard]] inline std::string readString(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor) {
    const auto length = readInteger<std::uint16_t>(bytes, cursor);
    if (length > kMaximumIdentifierLength || cursor + length > bytes.size()) {
        throw std::runtime_error("Save data contains an oversized string");
    }
    std::string text(reinterpret_cast<const char*>(bytes.data() + cursor), length);
    cursor += length;
    return text;
}

inline void appendFloat(std::vector<std::uint8_t>& bytes, float value) {
    appendInteger(bytes, std::bit_cast<std::uint32_t>(value));
}

inline void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    appendInteger(bytes, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] inline float readFloat(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return std::bit_cast<float>(readInteger<std::uint32_t>(bytes, cursor));
}

[[nodiscard]] inline double readDouble(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return std::bit_cast<double>(readInteger<std::uint64_t>(bytes, cursor));
}

// Builds a four-character block tag at compile time: 'C','H','N','K' reads as
// "CHNK" in a hex dump, which is the point of having one.
[[nodiscard]] constexpr std::uint32_t blockTag(const char (&text)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

// Writes a block frame and patches its size on destruction. Every owner's writer
// opens one of these and appends its body; nobody hand-patches the size field,
// which is what the four copies of that loop used to do.
class SaveBlockWriter final {
  public:
    SaveBlockWriter(std::vector<std::uint8_t>& bytes, std::uint32_t tag, std::uint16_t version)
        : bytes_(bytes), start_(bytes.size()) {
        appendInteger(bytes_, tag);
        appendInteger(bytes_, static_cast<std::uint32_t>(0U));  // size, patched below
        appendInteger(bytes_, version);
    }

    SaveBlockWriter(const SaveBlockWriter&) = delete;
    SaveBlockWriter& operator=(const SaveBlockWriter&) = delete;
    SaveBlockWriter(SaveBlockWriter&&) = delete;
    SaveBlockWriter& operator=(SaveBlockWriter&&) = delete;

    ~SaveBlockWriter() {
        const auto size = static_cast<std::uint32_t>(bytes_.size() - start_);
        for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
            bytes_[start_ + sizeof(std::uint32_t) + offset] =
                static_cast<std::uint8_t>(size >> (offset * 8U));
        }
    }

  private:
    std::vector<std::uint8_t>& bytes_;
    std::size_t start_;
};

// A block's header, already validated: where its body starts and ends, and what
// version wrote it. A reader that does not recognise the version jumps to `end`.
struct SaveBlockHeader final {
    std::uint32_t tag = 0U;
    std::uint16_t version = 0U;
    std::size_t bodyStart = 0U;
    std::size_t end = 0U;
};

[[nodiscard]] inline SaveBlockHeader readBlockHeader(
    std::span<const std::uint8_t> payload,
    std::size_t& cursor,
    const char* what) {
    const std::size_t start = cursor;
    if (start + kBlockHeaderBytes > payload.size()) {
        throw std::runtime_error(std::string{"world.dat "} + what + " block is truncated");
    }
    SaveBlockHeader header;
    header.tag = readInteger<std::uint32_t>(payload, cursor);
    const auto size = readInteger<std::uint32_t>(payload, cursor);
    if (size < kBlockHeaderBytes || static_cast<std::size_t>(size) > payload.size() - start) {
        throw std::runtime_error(std::string{"world.dat "} + what + " block is malformed");
    }
    header.version = readInteger<std::uint16_t>(payload, cursor);
    header.bodyStart = cursor;
    header.end = start + size;
    return header;
}

// Maps registry values a save mentions to compact indices. `Empty` is always
// index 0, so an index a reader cannot resolve still means "nothing here".
//
// Two shapes, because the two registries are shaped differently: blocks are a
// dense integer identity (a BlockId), items are pointers. Java would reach for a
// HashMap in both cases; a hash per stack is real cost on a world with a hundred
// thousand stored stacks, and for a dense id an array indexed by the id already
// gives O(1) with no hashing.
//
// The reverse index is sized at construction to the id domain the caller passes
// — `blockCount()` for blocks — rather than a compile-time ceiling, so the
// palette holds however many ids the registry hands out. This is what removed
// the old 256 cap: an id past the domain is a caller bug (it never came from
// this registry), which throws rather than silently colliding.
template <typename Value>
class DensePalette final {
  public:
    DensePalette(Value empty, std::size_t domain)
        : entries_{empty}, indices_(domain, kAbsent) {
        indices_[slotOf(empty)] = 0U;
    }

    [[nodiscard]] std::uint16_t indexOf(Value value) {
        const auto slot = slotOf(value);
        if (slot >= indices_.size()) {
            throw std::runtime_error("Save references a value outside the palette domain");
        }
        auto& index = indices_[slot];
        if (index != kAbsent) {
            return index;
        }
        index = static_cast<std::uint16_t>(entries_.size());
        entries_.push_back(value);
        return index;
    }

    [[nodiscard]] std::span<const Value> entries() const { return entries_; }

  private:
    // The reverse-index slot a value occupies. A dense id (BlockId) answers with
    // its `index()`; a plain enum or integer casts straight to size_t.
    [[nodiscard]] static std::size_t slotOf(Value value) {
        if constexpr (requires { value.index(); }) {
            return value.index();
        } else {
            return static_cast<std::size_t>(value);
        }
    }

    static constexpr std::uint16_t kAbsent = 0xFFFFU;
    std::vector<Value> entries_;
    // Index 0 is reserved for Empty, so "absent" cannot be spelled as 0.
    std::vector<std::uint16_t> indices_;
};

template <typename Value, Value Empty>
class HashPalette final {
  public:
    [[nodiscard]] std::uint16_t indexOf(Value value) {
        const auto existing = indices_.find(value);
        if (existing != indices_.end()) {
            return existing->second;
        }
        const auto index = static_cast<std::uint16_t>(entries_.size());
        entries_.push_back(value);
        indices_.emplace(value, index);
        return index;
    }

    [[nodiscard]] std::span<const Value> entries() const { return entries_; }

  private:
    std::vector<Value> entries_{Empty};
    std::unordered_map<Value, std::uint16_t> indices_{{Empty, 0U}};
};

} // namespace mc::persistence
