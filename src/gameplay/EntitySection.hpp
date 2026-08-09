#pragma once

#include "world/WorldConstants.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace mc::gameplay {

// The 16×16×16 block section a position falls in — the unit of the spatial
// hash that keeps nearby-entity queries O(k) instead of O(n). It mirrors
// vanilla's WorldChunk per-section entity buckets (chunkX/chunkZ are the
// 16-block chunk coords, sectionY the 16-block vertical layer), so a neighbour
// query is a 3×3×3 walk instead of a sweep of the whole herd. Shared by the
// world-entity system (pushing, raycast) and the item system (player magnet).
struct EntitySection final {
    int chunkX = 0;
    int sectionY = 0;
    int chunkZ = 0;

    [[nodiscard]] bool operator==(const EntitySection&) const = default;
};

struct EntitySectionHash final {
    [[nodiscard]] std::size_t operator()(const EntitySection& section) const noexcept {
        // 21 bits per coordinate packed without overlap into a 64-bit key; a
        // collision then needs a coordinate difference of 2^21 sections
        // (~33 million blocks), which no world session will ever reach.
        const auto field = [](int value) {
            return static_cast<std::uint32_t>(value) & 0x1FFFFFU;
        };
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(field(section.chunkX)) << 42U) |
            (static_cast<std::uint64_t>(field(section.sectionY)) << 21U) |
            static_cast<std::uint64_t>(field(section.chunkZ));
        return std::hash<std::uint64_t>{}(packed);
    }
};

// The section key for an absolute block position (floor of each /16 axis).
[[nodiscard]] inline EntitySection entitySectionOf(glm::vec3 position) {
    constexpr float kSize = static_cast<float>(world::kSectionSize);
    return {static_cast<int>(std::floor(position.x / kSize)),
            static_cast<int>(std::floor(position.y / kSize)),
            static_cast<int>(std::floor(position.z / kSize))};
}

} // namespace mc::gameplay
