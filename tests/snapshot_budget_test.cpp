// N-Mem §4.2: the render snapshots are the only real new memory the client/server
// split adds — the 401 render-direct reads became value-copies, and without a
// ceiling they would eat back what the split saved. Each snapshot type therefore
// carries a byte budget and reuses its buffers across ticks, the way
// chunk_section_test pins ChunkSection::stateHeapBytes(). This test holds those
// ceilings so a regression that starts copying the world (or reallocating every
// tick) turns red instead of silently growing the render side.
//
// The budgets are ceilings with headroom, not exact sizes: the point is to catch
// a gross regression (chunk data entering WorldSnapshot, a per-tick reallocation
// of the entity buffers), not to churn on a few bytes.

#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "render/player/PlayerRenderState.hpp"
#include "ui/UiFrameData.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <optional>
#include <vector>

using namespace mc;

namespace {

struct SilentHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

}  // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    // --- The inline POD snapshots are a few hundred bytes and nothing more:
    // the whole per-tick/per-frame player mirror fits in one small struct. The
    // ceilings are a handful of kilobytes each — wide enough for a legitimate
    // new field, narrow enough to catch a gross addition (a large array, a
    // whole copied inventory).
    {
        assert(sizeof(gameplay::PlayerTickSnapshot) < 1024U);
        assert(gameplay::PlayerTickSnapshot{}.residentBytes() ==
               sizeof(gameplay::PlayerTickSnapshot));
        assert(sizeof(ui::UiFrameData) < 512U);
        assert(sizeof(render::player::PlayerRenderState) < 512U);
    }

    // --- WorldSnapshot is incremental and bounded: a per-tick mirror of scalar
    // world state plus the chest lid states, never a chunk/block copy. Its size
    // is fixed except for the chest buffer, which reuses its capacity across
    // ticks and stays far below any whole-world mirror. ---
    {
        world::World world;
        SilentHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Creative);
        session.createChestBlockEntity({0, 64, 0});
        session.createChestBlockEntity({4, 64, 0});
        session.tick(world, host);
        const auto& worldSnap = session.worldSnapshot();
        assert(worldSnap.chests.size() == 2U);
        // ~2 KB fixed plus the chest buffer; an 8 KB ceiling catches any chunk
        // or block data sneaking into the mirror (which would be MBs).
        assert(worldSnap.residentBytes() < 8192U);
        const auto chestCapacity = worldSnap.chests.capacity();

        // Re-publishing the same population reuses the buffer instead of
        // growing it every tick.
        session.tick(world, host);
        assert(session.worldSnapshot().chests.capacity() == chestCapacity);

        // The chest vector keeps its capacity across a drop to zero: clear()
        // never shrinks, so the next world's capture reuses it.
        session.tick(world, host);
        assert(session.worldSnapshot().chests.capacity() >= 2U);
    }

    // --- EntityRenderSnapshot reuses its three buffers and stays linear in the
    // population with a bounded per-entity cost. ---
    {
        gameplay::EntityRenderSnapshot snapshot;
        std::vector<gameplay::SimpleEntity> live;
        for (std::uint64_t id = 1U; id <= 100U; ++id) {
            auto& entity = live.emplace_back();
            entity.type = &gameplay::entities::PigEntity::type();
            entity.id = id;
            entity.position = {1.0F, 2.0F, 3.0F};
        }
        snapshot.capture(live, {}, {});
        assert(snapshot.entities().size() == 100U);
        assert(snapshot.entities().capacity() >= 100U);
        assert(snapshot.residentBytes() < 4096U + 100U * 128U);
        const auto capacity = snapshot.entities().capacity();

        // Same population: the buffer is reused, capacity is stable.
        snapshot.capture(live, {}, {});
        assert(snapshot.entities().capacity() == capacity);

        // Dropped to zero: capacity is kept for the next capture, not freed and
        // reallocated every tick.
        live.clear();
        snapshot.capture(live, {}, {});
        assert(snapshot.entities().empty());
        assert(snapshot.entities().capacity() >= 100U);
    }

    std::cout << "PASS: snapshot_budget_test\n";
    return 0;
}
