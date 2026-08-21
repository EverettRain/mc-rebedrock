// BE1 block-entity type identity: the registry lifecycle, name resolution, and
// the block->BE mapping baked onto BlockDefinition.
//
// The block-entity type registry walks the same Bootstrap -> External -> Freeze
// machine every other content registry does, so this pins the same guarantees
// the block registry test pins — built-in ids equal their enum ordinals, a
// `minecraft:` alias resolves beside the `rebedrock:` key, external content
// continues the dense sequence without renumbering built-ins, and registration
// after freeze (or a dereference of an invalid id) aborts rather than silently
// handing out the wrong identity. On top of that it checks the piece BE1 adds
// that blocks did not have: a block knows, in one subscript, which block-entity
// kind it hosts (chest/furnace) and whether it hosts one at all.

#include "world/Block.hpp"
#include "world/BlockEntityType.hpp"

#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

namespace {

// Runs `body` in a child and reports whether it aborted (SIGABRT). The lifecycle
// guarantees are aborts, not return codes, so they are checked in a forked child
// exactly the way block_registry_lifecycle_test does; stderr is dropped so a pass
// stays quiet.
bool aborts(const std::function<void()>& body) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        body();
        _exit(0); // reached only when body did NOT abort
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// A block-entity type this build's own content does not define, standing in for a
// datapack BE a loader parsed into a definition. Only the identity matters to the
// registry.
[[nodiscard]] mc::world::ExternalBlockEntityTypeDef makeExternalType(std::string_view path) {
    mc::world::ExternalBlockEntityTypeDef def{};
    def.definition.kind = mc::world::BlockEntityKind::Chest; // borrowed shape, distinct name
    def.definition.identifier = {"testmod", path};
    def.definition.vanilla = {}; // original content, no vanilla mirror
    def.definition.displayName = "Test";
    return def;
}

} // namespace

int main() {
    using namespace mc;
    using mc::world::Block;
    using mc::world::BlockEntityKind;

    // --- A build with no external content registers exactly the built-ins, and
    //     both keys resolve to the same id. ---
    {
        const auto registry = world::buildBlockEntityTypeRegistry({});
        assert(registry.size() == world::kBuiltinBlockEntityTypeCount);
        assert(registry.phase() == core::RegistryPhase::Freeze);

        const auto chest = registry.byName("rebedrock:chest");
        const auto furnace = registry.byName("rebedrock:furnace");
        assert(chest.valid() && chest.index() == static_cast<std::size_t>(BlockEntityKind::Chest));
        assert(furnace.valid() &&
               furnace.index() == static_cast<std::size_t>(BlockEntityKind::Furnace));

        // The `minecraft:` alias and the bare path both resolve to the same id, so
        // a 1.16.1 save's `minecraft:chest` reaches the type.
        assert(registry.byName("minecraft:chest") == chest);
        assert(registry.byName("chest") == chest);
        assert(registry.byName("minecraft:furnace") == furnace);

        // A name nothing owns is a miss, never a placeholder or an abort.
        assert(!registry.byName("rebedrock:hopper").valid());
        assert(!registry.byName("nonsense").valid());

        // The baked ordinal id matches the id the registry actually assigned.
        assert(world::blockEntityTypeId(BlockEntityKind::Chest) == chest);
        assert(world::blockEntityTypeId(BlockEntityKind::Furnace) == furnace);

        // The definition derefs by that id.
        assert(registry.get(chest).kind == BlockEntityKind::Chest);
        assert(registry.identifier(furnace).toString() == "rebedrock:furnace");
    }

    // --- External content registers after the built-ins and resolves by name,
    //     without moving a single built-in id. ---
    {
        const std::array external{makeExternalType("hopper"), makeExternalType("barrel")};
        const auto registry = world::buildBlockEntityTypeRegistry(external);

        assert(registry.byName("rebedrock:chest").index() ==
               static_cast<std::size_t>(BlockEntityKind::Chest));
        assert(registry.byName("rebedrock:furnace").index() ==
               static_cast<std::size_t>(BlockEntityKind::Furnace));

        assert(registry.size() == world::kBuiltinBlockEntityTypeCount + 2U);
        const auto hopper = registry.byName("testmod:hopper");
        const auto barrel = registry.byName("testmod:barrel");
        assert(hopper.valid() && hopper.index() == world::kBuiltinBlockEntityTypeCount);
        assert(barrel.valid() && barrel.index() == world::kBuiltinBlockEntityTypeCount + 1U);
        assert(registry.phase() == core::RegistryPhase::Freeze);
    }

    // --- Phase guards, each an abort in a forked child. ---

    // registerExternal before the External phase opens.
    assert(aborts([] {
        world::BlockEntityTypeRegistry registry;
        registry.registerExternal(core::Identifier{"testmod", "early"},
                                  world::kBlockEntityTypeRegistry[0]);
    }));

    // registerBuiltin after the External phase opened.
    assert(aborts([] {
        world::BlockEntityTypeRegistry registry;
        registry.beginExternal();
        registry.registerBuiltin(core::Identifier{"rebedrock", "late_builtin"},
                                 world::kBlockEntityTypeRegistry[0]);
    }));

    // A duplicate name is a collision, not a silent overwrite.
    assert(aborts([] {
        world::BlockEntityTypeRegistry registry;
        registry.registerBuiltin(core::Identifier{"rebedrock", "chest"},
                                 world::kBlockEntityTypeRegistry[0]);
        registry.registerBuiltin(core::Identifier{"rebedrock", "chest"},
                                 world::kBlockEntityTypeRegistry[1]);
    }));

    // Registering after freeze.
    assert(aborts([] {
        auto registry = world::buildBlockEntityTypeRegistry({});
        registry.registerExternal(core::Identifier{"testmod", "afterfreeze"},
                                  world::kBlockEntityTypeRegistry[0]);
    }));

    // Dereferencing an invalid id.
    assert(aborts([] {
        const auto registry = world::buildBlockEntityTypeRegistry({});
        (void)registry.get(core::BlockEntityTypeId::invalid());
    }));

    // --- The block->BE mapping: a cell learns which block entity it hosts, and
    //     whether it hosts one, in one indexed load off the block table. ---
    {
        const auto& registry = world::blockEntityTypeRegistry();

        // Chest and furnace host their block entity; the baked id matches the
        // registry's id by name (the two are the same identity).
        assert(world::hasBlockEntity(Block::Chest));
        assert(world::blockEntityTypeOf(Block::Chest) == registry.byName("chest"));
        assert(registry.get(world::blockEntityTypeOf(Block::Chest)).kind == BlockEntityKind::Chest);

        assert(world::hasBlockEntity(Block::Furnace));
        assert(world::blockEntityTypeOf(Block::Furnace) == registry.byName("furnace"));
        assert(registry.get(world::blockEntityTypeOf(Block::Furnace)).kind ==
               BlockEntityKind::Furnace);

        // Ordinary blocks host none: the pre-filter rejects them and their type id
        // is invalid, so a placement never tries to build a block entity for
        // stone, dirt or a plank.
        for (const Block block :
             {Block::Stone, Block::Dirt, Block::Air, Block::OakPlanks, Block::CraftingTable}) {
            assert(!world::hasBlockEntity(block));
            assert(!world::blockEntityTypeOf(block).valid());
        }
    }

    // --- The process singleton is frozen for real. ---
    assert(world::blockEntityTypeRegistry().phase() == core::RegistryPhase::Freeze);

    std::puts("block_entity_type_registry_test: OK");
    return 0;
}
