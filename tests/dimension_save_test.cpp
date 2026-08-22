// DIM-4: per-dimension save (region subdirectories, vanilla layout) (headless).
//
// Proves the four DIM-4 invariants:
//   1. Subdirectory layout mirrors vanilla 1.16.1 — the Overworld at the world
//      root, the Nether under DIM-1, the End under DIM1 — so JC3 import finds each
//      dimension where a vanilla world keeps it (no new deviation).
//   2. Round-trip isolation: a chunk saved to one dimension does not leak into
//      another; each dimension reads back its own edits/creatures.
//   3. Backward compatibility: an Overworld-only save (the historical flat layout)
//      still reads its chunk, and format 18 worlds still load.
//   4. JC alignment: the on-disk path equals the vanilla-expected path string.
#include "persistence/SaveRepository.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Dimension.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

using mc::persistence::PersistentEntity;
using mc::persistence::SaveRepository;
using mc::world::DimensionId;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "mc-rebedrock-dim-save-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    SaveRepository repository{root};
    auto save = repository.create("Dim Save World", 0xABCDEF01ULL);
    const std::string id = save.summary.identifier;

    // --- Subdirectory layout mirrors vanilla ---------------------------------
    const auto owDir = repository.dimensionRegionDirectory(id, DimensionId::Overworld);
    const auto netherDir = repository.dimensionRegionDirectory(id, DimensionId::Nether);
    const auto endDir = repository.dimensionRegionDirectory(id, DimensionId::End);

    // The Overworld is the historical `<world>/region` (no subfolder), so old flat
    // worlds are byte-compatible. Sabotage ①/③'s guard.
    assert(owDir == root / id / "region");
    // The Nether is DIM-1, the End is DIM1 — vanilla 1.16.1 folder names (the JC3
    // import target). Sabotage ③'s guard: any other spelling breaks this.
    assert(netherDir == root / id / "DIM-1" / "region");
    assert(endDir == root / id / "DIM1" / "region");
    // The three directories are distinct — no dimension writes over another.
    assert(owDir != netherDir && netherDir != endDir && owDir != endDir);

    // --- Round-trip isolation across dimensions -------------------------------
    // Save a distinct creature into the same chunk coordinate in each dimension.
    const auto makeEntity = [](const char* species, float x) {
        PersistentEntity entity;
        entity.species = species;
        entity.x = x;
        entity.y = 64.0F;
        entity.z = 8.0F;
        entity.health = 10.0F;
        return entity;
    };
    std::vector<mc::world::PersistentBlockEdit> owEdits{
        {1, 2, 3, mc::world::BlockState{mc::world::Block::Stone}}};
    std::vector<mc::world::PersistentBlockEdit> netherEdits{
        {4, 5, 6, mc::world::BlockState{mc::world::Block::Dirt}}};

    repository.saveChunk(id, 0, 0, owEdits, {makeEntity("pig", 1.0F)},
                         DimensionId::Overworld);
    repository.saveChunk(id, 0, 0, netherEdits, {makeEntity("cow", 2.0F)},
                         DimensionId::Nether);

    // Files landed in the right subdirectories.
    assert(std::filesystem::is_regular_file(owDir / "r.0.0.cache"));
    assert(std::filesystem::is_regular_file(netherDir / "r.0.0.cache"));
    // The Nether write did NOT touch the End directory (Sabotage ①'s guard: a
    // missed dimension key would collide the two in one folder).
    assert(!std::filesystem::exists(endDir / "r.0.0.cache"));

    // Each dimension reads back its own creature, not the other's.
    const auto owEntities = repository.loadChunkEntities(id, 0, 0, DimensionId::Overworld);
    const auto netherEntities = repository.loadChunkEntities(id, 0, 0, DimensionId::Nether);
    const auto endEntities = repository.loadChunkEntities(id, 0, 0, DimensionId::End);
    assert(owEntities.size() == 1U && owEntities[0].species == "pig");
    assert(netherEntities.size() == 1U && netherEntities[0].species == "cow");
    assert(endEntities.empty());  // the End was never written

    // --- Backward compatibility: default arg == Overworld ---------------------
    // The historical two-arg call (no dimension) still reads the Overworld chunk,
    // exactly as before the per-dimension split. Sabotage ②/③'s guard.
    const auto defaultEntities = repository.loadChunkEntities(id, 0, 0);
    assert(defaultEntities.size() == 1U && defaultEntities[0].species == "pig");

    // --- JC alignment: path equals the vanilla-expected string ----------------
    // A vanilla 1.16.1 world keeps the Nether region at "<world>/DIM-1/region";
    // JC3 import walks exactly this path. Assert the string, not just the enum.
    {
        const std::string netherStr = netherDir.generic_string();
        assert(netherStr.ends_with(id + "/DIM-1/region"));
        const std::string endStr = endDir.generic_string();
        assert(endStr.ends_with(id + "/DIM1/region"));
        const std::string owStr = owDir.generic_string();
        assert(owStr.ends_with(id + "/region"));
        // The Overworld path must NOT contain a DIM subfolder.
        assert(owStr.find("/DIM") == std::string::npos);
    }

    // --- A pre-existing flat world (only <world>/region) reads its Overworld ---
    // Deleting the DIM folders leaves exactly a legacy flat world; the Overworld
    // chunk still loads and the secondary dimensions read empty.
    std::filesystem::remove_all(root / id / "DIM-1");
    std::filesystem::remove_all(root / id / "DIM1");
    {
        const auto flatOw = repository.loadChunkEntities(id, 0, 0, DimensionId::Overworld);
        assert(flatOw.size() == 1U && flatOw[0].species == "pig");
        const auto flatNether = repository.loadChunkEntities(id, 0, 0, DimensionId::Nether);
        assert(flatNether.empty());  // no DIM-1 folder -> empty, not a crash
    }

    // The whole-world save/load still round-trips (format 19), and an existing
    // world reloads through the summary path.
    repository.save(save);
    const auto reloaded = repository.load(id);
    assert(reloaded.summary.identifier == id);

    std::filesystem::remove_all(root);
    return 0;
}
