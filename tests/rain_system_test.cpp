#include "render/RainSystem.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

// Exercises the rain collision and range directly: a chamber world with a roof
// slab proves drops never pass through a roof into the space below, splashes
// only appear on the surfaces the cached probe found, and the field reaches
// beyond the old ±16-box. No renderer, no Vulkan — the collision path is pure
// CPU and unit-testable.
int main() {
    mc::world::World world;
    // An 8x8-chunk stone floor at y=0 covering x,z in [-32, 95]: wide enough
    // that the ±24 spawn box plus a drop's per-drop wind jitter (±0.3 blocks/s
    // of sideways drift) never pushes a drop past the loaded edge into the void
    // — the void free-fall path is for genuinely unloaded terrain, not the box.
    // A roof slab at block y=6 (top face y=7) over the central 9x9 (blocks x,z
    // in 4..12) tests that drops stop on a roof instead of passing through.
    constexpr int kChunkSpan = 8;
    for (int chunkZ = 0; chunkZ < kChunkSpan; ++chunkZ) {
        for (int chunkX = 0; chunkX < kChunkSpan; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX - 2, chunkZ - 2}, std::move(chunk));
        }
    }
    for (int z = 4; z <= 12; ++z) {
        for (int x = 4; x <= 12; ++x) {
            world.setBlock(x, 6, z, mc::world::Block::Stone);
        }
    }

    mc::render::RainSystem rain;
    // The camera eye sits inside the chamber: floor top at y=1, roof top at y=7.
    const glm::vec3 camera{8.0F, 3.5F, 8.0F};
    const glm::vec2 wind{0.0F, 0.0F};
    for (int frame = 0; frame < 240; ++frame) {
        rain.update(1.0F / 60.0F, camera, 1.0F, 1500U, world, wind);
    }
    assert(rain.drops().size() == 1500U);

    // The texture renderer consumes the same cached MOTION_BLOCKING result to
    // clamp its vertical strips: the central column starts on the roof, while
    // a column outside the slab starts on the floor.
    assert(rain.precipitationSurfaceY(world, 8, 8, camera.y + 32.0F) == 7.0F);
    assert(rain.precipitationSurfaceY(world, 20, 8, camera.y + 32.0F) == 1.0F);

    // The roof fix: a drop over the roof slab (blockX/Z in 4..12) never falls
    // below it — the cached surface is the roof top, so drops land on y=7
    // instead of passing through into the chamber at y<6. Drops outside the
    // slab land on the floor at y=1 and never dig under it.
    for (const auto& drop : rain.drops()) {
        if (drop.position.x >= 4.0F && drop.position.x < 13.0F && drop.position.z >= 4.0F &&
            drop.position.z < 13.0F) {
            assert(drop.position.y >= 6.0F);
        } else {
            assert(drop.position.y >= 1.0F);
        }
    }

    // Splashes appear exactly on the two cached surfaces — the roof top (7) and
    // the floor (1) — never mid-air from a drop that clipped a surface.
    for (const auto& splash : rain.splashes()) {
        assert(splash.position.y == 7.0F || splash.position.y == 1.0F);
    }

    // Texture rain follows WorldRenderer#tickRainSplashing instead of waiting
    // for its small sound/drop population to land. Give the left side a water
    // surface at y=1 and the right side solid ground at y=1; one full-rain tick
    // must sample visible impacts on both kinds of surface.
    for (int z = -2; z <= 18; ++z) {
        for (int x = -2; x <= 18; ++x) {
            world.setBlock(x, 1, z, x <= 3 ? mc::world::Block::Water
                                           : mc::world::Block::Stone);
        }
    }
    mc::render::RainSystem textureRain;
    textureRain.emitTextureImpacts(1.0F / 20.0F, camera, 1.0F, world);
    bool sawWaterImpact = false;
    bool sawGroundImpact = false;
    for (const auto& impact : textureRain.splashes()) {
        assert(impact.sampledImpact);
        sawWaterImpact = sawWaterImpact || impact.onWater;
        sawGroundImpact = sawGroundImpact || !impact.onWater;
    }
    assert(sawWaterImpact);
    assert(sawGroundImpact);

    // 世界底不是 0：列探测必须能一直扫到 kMinY
    // 探测下界曾写死字面量 0，深板岩层（y < 0）里的任何表面都探测不到，
    // 那里的雨滴只会一路自由落体，既不落地也不溅射
    // 探测上界同样曾误用 kWorldHeight-1（那是行数 384，不是坐标上界 320）
    {
        mc::world::World deepWorld;
        for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
            for (int chunkX = -1; chunkX <= 1; ++chunkX) {
                deepWorld.setChunk({chunkX, chunkZ}, mc::world::Chunk{});
            }
        }
        // 唯一的表面在 y = -20，整段在旧的探测下界之下
        for (int z = -8; z <= 8; ++z) {
            for (int x = -8; x <= 8; ++x) {
                deepWorld.setBlock(x, -20, z, mc::world::Block::Stone);
            }
        }
        mc::render::RainSystem deepRain;
        const glm::vec3 deepCamera{0.0F, 5.0F, 0.0F};
        assert(deepRain.precipitationSurfaceY(deepWorld, 0, 0, deepCamera.y + 32.0F) == -19.0F);
    }

    // The wider field: with the ±24 box, drops reach beyond the old ±16 so the
    // rain (and its splashes) read at a distance.
    float farthest = 0.0F;
    for (const auto& drop : rain.drops()) {
        farthest = std::max(farthest, std::abs(drop.position.x - camera.x));
        farthest = std::max(farthest, std::abs(drop.position.z - camera.z));
    }
    assert(farthest > 16.0F);

    // The cache-less path must behave the same: with the collision cache
    // disabled, drops still stop on the roof instead of passing through it.
    {
        mc::render::RainSystem direct;
        direct.setCollisionCache(false);
        for (int frame = 0; frame < 240; ++frame) {
            direct.update(1.0F / 60.0F, camera, 1.0F, 1500U, world, wind);
        }
        for (const auto& drop : direct.drops()) {
            const bool overRoof = drop.position.x >= 4.0F && drop.position.x < 13.0F &&
                                  drop.position.z >= 4.0F && drop.position.z < 13.0F;
            assert(drop.position.y >= (overRoof ? 6.0F : 1.0F));
        }
    }

    // Wind-blown rain that drifts sideways into a wall splashes on the wall's
    // side, not just on top surfaces: a tall wall across the field with the
    // wind carrying drops into it produces splashes at wall height.
    mc::world::World wallWorld;
    for (int chunkZ = 0; chunkZ < 5; ++chunkZ) {
        for (int chunkX = 0; chunkX < 5; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            wallWorld.setChunk({chunkX - 1, chunkZ - 1}, std::move(chunk));
        }
    }
    for (int z = -16; z <= 32; ++z) {
        for (int y = 1; y <= 25; ++y) {
            wallWorld.setBlock(11, y, z, mc::world::Block::Stone);
        }
    }
    mc::render::RainSystem wallRain;
    const glm::vec3 wallCamera{6.0F, 2.0F, 8.0F};
    const glm::vec2 wallWind{6.0F, 0.0F}; // +x, straight into the wall
    bool sawWallSplash = false;
    bool sawDirectionalWallSplash = false;
    for (int frame = 0; frame < 240; ++frame) {
        wallRain.update(1.0F / 60.0F, wallCamera, 1.0F, 1500U, wallWorld, wallWind);
        for (const auto& splash : wallRain.splashes()) {
            if (std::abs(splash.position.x - 11.0F) < 1.0F && splash.position.y > 1.0F &&
                splash.position.y <= 26.0F) {
                sawWallSplash = true;
                // The wind blows +x into the wall, so the cached wall-face
                // direction must spray the droplets back toward -x.
                if (splash.direction.x < -0.5F) {
                    sawDirectionalWallSplash = true;
                }
            }
        }
    }
    assert(sawWallSplash);
    assert(sawDirectionalWallSplash);

    return 0;
}
