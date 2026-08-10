#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::render {

// A falling rain drop, CPU-simulated. 粒子雨 and 异步粒子雨 consume these same
// drops so their draw backends remain directly comparable. 贴图雨 follows the
// vanilla column renderer instead; the drops still drive its impact splashes
// and weather audio, but are not turned into giant billboards.
struct RainDrop final {
    glm::vec3 position{0.0F};
    float size = 0.03F;
    // Per-drop cross-wind offset in blocks/s, fixed at spawn so each drop keeps
    // a consistent (slightly different) slant while the whole field follows the
    // frame's base wind direction.
    glm::vec2 windJitter{0.0F};
    // The surface this drop falls toward: the top of the topmost collision or
    // fluid block in its column below the spawn ceiling. A drop splashes the
    // frame its position crosses it; -1 means free fall (nothing found in the
    // probe span) until it respawns below the camera. `columnX/Z` remember the
    // column the value was computed for, so wind drift into a new column
    // re-fetches that column's surface — a cheap cache read, never a world scan.
    float targetSurface = -1.0F;
    bool surfaceWater = false;
    int columnX = 0;
    int columnZ = 0;
};

// A drop landing on a surface during the last update: the position to emit the
// splash from, whether the landing block was water (surface splash) or solid
// ground, and the horizontal spray direction. A wall splash carries a non-zero
// `direction` (the wall's outward normal, away from the face the drop hit) so
// the droplets bounce back off the wall in a fixed fan; ground/water splashes
// leave it zero and spray radially. The renderer drains these into the
// particle system.
struct RainSplash final {
    glm::vec3 position{0.0F};
    bool onWater = false;
    glm::vec2 direction{0.0F};
    // tickRainSplashing's sampled RAIN particle is one original-style upward
    // droplet; simulated drop landings use the existing multi-droplet spray.
    bool sampledImpact = false;
};

// Advances the rain for one frame. Collision is NOT per drop per frame: only
// the first drop to enter a column probes the world (scanning the column for
// its topmost surface, then caching it); every drop after that just falls to
// the cached surface and splashes there. That turns a large storm from
// thousands of world lookups a frame into a handful of probes plus cheap cache
// reads, which is what lets the field span a much bigger box (taller and
// wider) so tall roofs stop passing rain through and splashes appear farther
// out. The cache refreshes on a short staleness window and clears when it
// outgrows its cap, so edits to the world and a moving camera re-probe.
class RainSystem final {
  public:
    // Advances the rain for one frame: grows/shrinks the population toward
    // targetCount (scaled by rain intensity), falls each drop, and splashes the
    // ones that reach their column's surface, respawning them at the top.
    void update(float deltaSeconds, const glm::vec3& cameraPosition, float intensity,
                std::size_t targetCount, const world::World& world, const glm::vec2& wind);

    // WorldRenderer#tickRainSplashing's visual half for texture rain. At 20 TPS
    // it samples nearby MOTION_BLOCKING columns and appends one impact event at
    // the exact solid collision top or fluid surface. This is independent of
    // the texture mode's deliberately small simulated-drop population.
    void emitTextureImpacts(float deltaSeconds, const glm::vec3& cameraPosition,
                            float intensity, const world::World& world);

    // Selects the collision strategy. With the cache on (default) the first
    // drop to enter a column probes its surface and the rest fall to the cached
    // value; with it off every drop probes the block at its own position every
    // frame — the original full-fidelity path, for machines with headroom.
    void setCollisionCache(bool use) { useCollisionCache_ = use; }
    [[nodiscard]] bool collisionCacheEnabled() const { return useCollisionCache_; }

    [[nodiscard]] const std::vector<RainDrop>& drops() const { return drops_; }
    // Splash events collected by the last update; the renderer drains them into
    // a particle system, and the next update clears the list.
    [[nodiscard]] const std::vector<RainSplash>& splashes() const { return splashes_; }
    // The number of world.block calls the last update's collision handling made
    // (probe scans only). Diagnostic: quantifies the surface cache turning the
    // old per-drop-per-frame cost into a handful of probes.
    [[nodiscard]] std::size_t lastUpdateLookups() const { return lastUpdateLookups_; }

    // Returns the top of the first precipitation-blocking surface in a column,
    // using the same bounded cache as falling drops. The vanilla-style texture
    // renderer uses this like MOTION_BLOCKING heightmap data: its vertical rain
    // strip starts at this height, keeping rain above roofs instead of drawing
    // the strip through the room below. -1 means no surface was found within
    // the probe span below `ceiling`.
    [[nodiscard]] float precipitationSurfaceY(const world::World& world, int blockX, int blockZ,
                                              float ceiling);

  private:
    // One cached column surface: the top Y of the topmost collision/fluid block
    // at-or-below the spawn ceiling, whether it is water, and when it was probed.
    struct ColumnSurface final {
        float surfaceY = -1.0F;
        bool water = false;
        float probedAt = -1000.0F;
    };

    // The cached surface for a column, probing (and caching) on a miss or when
    // the entry has gone stale. The probe scans down from the ceiling — the top
    // of the spawn window — so it finds roofs and treetops, not just the ground.
    [[nodiscard]] ColumnSurface columnSurface(const world::World& world, int blockX, int blockZ,
                                              float ceiling);

    // Picks a fresh column in the box around the camera, places the drop above
    // that column's surface (so it always falls a little way and lands, never
    // materialising under cover), and records its target surface.
    void respawnDrop(RainDrop& drop, const glm::vec3& cameraPosition, const world::World& world);
    // The cache-less respawn for the direct per-drop collision path: a random
    // position in the box with no surface bookkeeping, since every frame probes
    // the world itself.
    void respawnDropFree(RainDrop& drop, const glm::vec3& cameraPosition);

    [[nodiscard]] float randomUnit();
    [[nodiscard]] static std::uint64_t columnKey(int blockX, int blockZ);
    // Packed key for a wall face hit: (blockX, blockZ, blockY).
    [[nodiscard]] static std::uint64_t wallKey(int blockX, int blockZ, int blockY);

    std::vector<RainDrop> drops_;
    std::vector<RainSplash> splashes_;
    std::unordered_map<std::uint64_t, ColumnSurface> surfaceCache_;
    // Wall faces a drop has drifted into: (column, height) → the horizontal
    // direction away from the face (the wall's outward normal), cached the
    // first time a drop hits it so every later drop reaching the same face
    // splashes in the same fixed direction without re-probing the world.
    std::unordered_map<std::uint64_t, glm::vec2> wallCache_;
    std::uint32_t randomState_ = 0x5EED41U;
    float timeSeconds_ = 0.0F;
    float textureImpactAccumulator_ = 0.0F;
    std::uint64_t textureImpactTick_ = 0U;
    std::size_t lastUpdateLookups_ = 0U;
    bool useCollisionCache_ = true;
};

} // namespace mc::render
