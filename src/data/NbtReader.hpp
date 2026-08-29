#pragma once

// A read-only, schema-directed NBT cursor. STRUCT-0's answer to "read the vanilla
// structure `.nbt`" without porting Java's `CompoundTag` object graph (a boxed
// tag per value, a HashMap per compound) — the very shape `SaveStream.hpp` already
// rejected for this project's own saves.
//
// This is NOT a DOM. It builds nothing. A caller walks the byte stream forward
// once, pulling the handful of tags it knows the schema names and calling `skip`
// on the rest. Because the structure schema is fixed and known (size / palette /
// blocks / entities / DataVersion), no general tag tree is needed: the reader
// exposes typed scalar reads, list/compound headers, and a type-directed `skip`
// that steps over any payload it does not care about.
//
// NBT is big-endian; every scalar read converts from the wire order on any host,
// the mirror of SaveStream's little-endian byte layer. Every read is bounds
// checked against the buffer end and sets `fail()` on overrun rather than reading
// past it, so a truncated or malformed file is a clean parse failure, never a
// crash — the same forward-compatible tolerance the codec path gives a datapack.
//
// Reusable beyond structures: the JC2 Anvil/NBT world reader (je-save-compat) can
// share this cursor; it differs from STRUCT only in the schema it walks, not in
// the byte mechanics.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace mc::data {

// The 12 NBT tag ids (TAG_End..TAG_Long_Array), wire values fixed by the format.
enum class NbtTag : std::uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

class NbtReader final {
  public:
    explicit NbtReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] std::size_t position() const { return position_; }
    [[nodiscard]] bool atEnd() const { return position_ >= bytes_.size(); }

    // --- scalar reads (big-endian) ---------------------------------------
    std::int8_t readByte() { return static_cast<std::int8_t>(readRaw<std::uint8_t>()); }
    std::int16_t readShort() { return static_cast<std::int16_t>(readRaw<std::uint16_t>()); }
    std::int32_t readInt() { return static_cast<std::int32_t>(readRaw<std::uint32_t>()); }
    std::int64_t readLong() { return static_cast<std::int64_t>(readRaw<std::uint64_t>()); }
    float readFloat() {
        const std::uint32_t bits = readRaw<std::uint32_t>();
        float value = 0.0F;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    double readDouble() {
        const std::uint64_t bits = readRaw<std::uint64_t>();
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // A modified-UTF-8 string, length-prefixed by an unsigned short. This build
    // reads the bytes verbatim (structure identifiers/properties are plain
    // ASCII); full modified-UTF-8 decoding is unnecessary until a value needs it.
    std::string readString() {
        const std::uint16_t length = readRaw<std::uint16_t>();
        if (failed_ || position_ + length > bytes_.size()) {
            fail();
            return {};
        }
        std::string out(reinterpret_cast<const char*>(bytes_.data() + position_), length);
        position_ += length;
        return out;
    }

    // --- structure headers -----------------------------------------------

    // The tag id + name of the next named tag inside a compound. `End` closes the
    // compound and carries no name. Used to walk a compound member by member.
    struct Named final {
        NbtTag tag = NbtTag::End;
        std::string name;
    };
    [[nodiscard]] Named readNamed() {
        const auto tag = static_cast<NbtTag>(readRaw<std::uint8_t>());
        if (failed_ || tag == NbtTag::End) {
            return {NbtTag::End, {}};
        }
        return {tag, readString()};
    }

    // A list header: element tag + count. NBT lists are homogeneous.
    struct ListHeader final {
        NbtTag element = NbtTag::End;
        std::int32_t length = 0;
    };
    [[nodiscard]] ListHeader readListHeader() {
        const auto element = static_cast<NbtTag>(readRaw<std::uint8_t>());
        const std::int32_t length = readInt();
        if (length < 0) {
            fail();
            return {NbtTag::End, 0};
        }
        return {element, length};
    }

    // Steps the cursor over one *payload* of the given tag (the value only; the
    // id/name were already consumed by readNamed for a compound member, or are
    // implicit for a list element). Recurses through nested lists/compounds. This
    // is the "skip what the schema doesn't name" primitive.
    void skipPayload(NbtTag tag) {
        switch (tag) {
        case NbtTag::End:
            return;
        case NbtTag::Byte:
            advance(1);
            return;
        case NbtTag::Short:
            advance(2);
            return;
        case NbtTag::Int:
        case NbtTag::Float:
            advance(4);
            return;
        case NbtTag::Long:
        case NbtTag::Double:
            advance(8);
            return;
        case NbtTag::ByteArray:
            advance(arrayBytes(1));
            return;
        case NbtTag::IntArray:
            advance(arrayBytes(4));
            return;
        case NbtTag::LongArray:
            advance(arrayBytes(8));
            return;
        case NbtTag::String: {
            const std::uint16_t length = readRaw<std::uint16_t>();
            advance(length);
            return;
        }
        case NbtTag::List: {
            const ListHeader header = readListHeader();
            for (std::int32_t index = 0; index < header.length && !failed_; ++index) {
                skipPayload(header.element);
            }
            return;
        }
        case NbtTag::Compound: {
            for (;;) {
                const Named member = readNamed();
                if (failed_ || member.tag == NbtTag::End) {
                    return;
                }
                skipPayload(member.tag);
            }
        }
        }
    }

  private:
    template <typename T>
    T readRaw() {
        if (position_ + sizeof(T) > bytes_.size()) {
            fail();
            return T{};
        }
        T value = 0;
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            value = static_cast<T>((value << 8) | bytes_[position_ + byte]);
        }
        position_ += sizeof(T);
        return value;
    }

    // Length-prefixed array payload (byte/int/long array): a 4-byte count times
    // the element width, returned as a byte span to advance over.
    std::size_t arrayBytes(std::size_t elementWidth) {
        const std::int32_t count = readInt();
        if (count < 0) {
            fail();
            return 0;
        }
        return static_cast<std::size_t>(count) * elementWidth;
    }

    void advance(std::size_t count) {
        if (position_ + count > bytes_.size()) {
            fail();
            position_ = bytes_.size();
            return;
        }
        position_ += count;
    }

    void fail() { failed_ = true; }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

} // namespace mc::data
