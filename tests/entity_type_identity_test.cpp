// E1 — entity-type identity on the shared R0 core::Registry.
//
// Covers what E1 moved: name resolution through the rebedrock: id / minecraft:
// alias / bare path, the networkId round-trip, id stability across a rerun of
// registerBuiltinEntities(), the three-phase Bootstrap/External/Freeze guards
// (aborts, checked in a forked child like registry_test.cpp), and the
// UnknownEntity placeholder that keeps a species this build cannot resolve from
// vanishing on a save round-trip.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Registry.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/UnknownEntity.hpp"
#include "persistence/SaveRepository.hpp"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using mc::gameplay::entities::EntityType;
using mc::gameplay::entities::EntityTypeRegistry;
using mc::gameplay::entities::MobCategory;
using mc::gameplay::entities::UnknownEntityAi;

// Runs `body` in a child process and returns whether it aborted (SIGABRT).
// Anything the child prints to stderr is dropped so a passing run stays quiet.
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

// The one inert AI a locally-built test type points at.
const UnknownEntityAi kTestAi;

EntityType makeType(std::string_view path) {
    return EntityType::Builder::create(MobCategory::Creature, kTestAi).sized(0.5F, 0.5F).build(path);
}

// Registry#get by name through every key form; an unknown name misses cleanly
// (nullptr), never a placeholder — that is what keeps /summon from conjuring one.
void testNameResolution() {
    const auto& registry = mc::gameplay::entities::entityTypeRegistry();
    const auto* pig = registry.byId("pig");
    assert(pig != nullptr);
    assert(registry.byId("minecraft:pig") == pig);
    assert(registry.byId("rebedrock:pig") == pig);
    assert(pig == &mc::gameplay::entities::builtinSpecies("pig"));
    // A name no species owns is a miss, not an abort and not a placeholder.
    assert(registry.byId("not_a_species") == nullptr);
    assert(registry.byId("minecraft:enderdragon") == nullptr);
    // idOf exposes the same resolution as a dense id.
    assert(registry.idOf("pig") == pig->typeId());
    assert(!registry.idOf("not_a_species").valid());
}

// byNetworkId is the reverse of EntityType#networkId; the three built-in ids are
// stable across a rerun of registerBuiltinEntities(), and an id past the table
// is a miss (nullptr), not a get() abort.
void testNetworkIdRoundTrip() {
    const auto& registry = mc::gameplay::entities::entityTypeRegistry();
    const auto* cow = registry.byId("cow");
    assert(cow != nullptr);
    assert(registry.byNetworkId(cow->networkId()) == cow);
    assert(registry.byNetworkId(registry.byId("pig")->networkId()) == &mc::gameplay::entities::builtinSpecies("pig"));
    assert(registry.byNetworkId(registry.byId("zombie")->networkId()) == &mc::gameplay::entities::builtinSpecies("zombie"));

    // Rerunning the built-in registration does not renumber anyone: the statics
    // are built once, so the ids the first run assigned are final.
    const std::uint16_t cowId = cow->networkId();
    const std::size_t sizeBefore = registry.size();
    mc::gameplay::entities::registerBuiltinEntities();
    assert(registry.size() == sizeBefore);
    assert(registry.byId("cow")->networkId() == cowId);

    // An id past the live table misses instead of aborting (sabotage③ target).
    assert(registry.byNetworkId(static_cast<std::uint16_t>(registry.size())) == nullptr);
    assert(registry.byNetworkId(9999U) == nullptr);
    // And crucially it does NOT abort — verified in a child so a regression that
    // drops the range check (an unchecked get) is caught rather than crashing us.
    assert(!aborts([] {
        (void)mc::gameplay::entities::entityTypeRegistry().byNetworkId(9999U);
    }));
}

// The three-phase lifecycle guards, checked on a local facade so the global
// singleton is untouched. Same contract the block registry forks against.
void testLifecycleAborts() {
    // registerBuiltin is Bootstrap-only.
    assert(aborts([] {
        EntityTypeRegistry r;
        r.beginExternal();
        EntityType t = makeType("x");
        r.registerBuiltin(t);
    }));
    // registerExternal is External-only.
    assert(aborts([] {
        EntityTypeRegistry r;
        EntityType t = makeType("x");
        r.registerExternal(t);
    }));
    // No registration survives a freeze.
    assert(aborts([] {
        EntityTypeRegistry r;
        r.freeze();
        EntityType t = makeType("x");
        r.registerBuiltin(t);
    }));
    // Two species cannot claim one name (sabotage① target).
    assert(aborts([] {
        EntityTypeRegistry r;
        EntityType a = makeType("dup");
        EntityType b = makeType("dup");
        r.registerBuiltin(a);
        r.registerBuiltin(b);
    }));
    // The happy path does not abort: builtin, then external after the phase
    // opens, then freeze — and both names resolve.
    assert(!aborts([] {
        EntityTypeRegistry r;
        EntityType builtin = makeType("home_builtin");
        r.registerBuiltin(builtin);
        r.beginExternal();
        EntityType external = makeType("home_external");
        r.registerExternal(external);
        r.freeze();
        if (r.byId("home_builtin") != &builtin) mc::core::registryAbort("builtin lost");
        if (r.byId("minecraft:home_external") != &external) mc::core::registryAbort("alias lost");
    }));
}

// UnknownEntity: a saved species this build cannot resolve round-trips by name
// through a SaveRepository region file instead of being dropped from the world.
void testUnknownEntityRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() /
                      "mc_rebedrock_e1_unknown_entity_test";
    std::filesystem::remove_all(root);
    mc::persistence::SaveRepository repository{root};
    auto game = repository.create("UnknownHerd", 7ULL);
    repository.save(game);

    // One chunk carrying three known species and one a removed mod left behind.
    std::vector<mc::persistence::PersistentEntity> stored;
    stored.push_back({"pig", 1.5F, 64.0F, 1.5F, 0.0F, 0, 0, 0, 10.0F, 0, 0U, 0U});
    stored.push_back({"cow", 2.5F, 64.0F, 2.5F, 0.0F, 0, 0, 0, 10.0F, 0, 0U, 0U});
    stored.push_back({"zombie", 3.5F, 64.0F, 3.5F, 0.0F, 0, 0, 0, 20.0F, 0, 0U, 0U});
    stored.push_back({"dragon", 4.5F, 64.0F, 4.5F, 0.0F, 0, 0, 0, 30.0F, 0, 0U, 0U});
    repository.saveChunk(game.summary.identifier, 0, 0, {}, stored);

    const auto loaded = repository.loadChunkEntities(game.summary.identifier, 0, 0);
    // The disk layer preserves every species string, unknown ones included.
    assert(loaded.size() == 4U);

    // The restore path GameRuntime runs: resolve each record and spawn it. The
    // unknown resolves to a placeholder rather than being dropped, so the herd
    // survives the round-trip whole (sabotage② target: a silent drop makes this
    // count short by one).
    mc::gameplay::EntitySystem revived;
    std::uint32_t nextId = 1U;
    for (const auto& record : loaded) {
        const auto& type = mc::gameplay::entities::resolveEntityTypeForRestore(record.species);
        revived.spawn({record.x, record.y, record.z}, type, nextId++);
    }
    assert(revived.entities().size() == 4U);

    // The known ones resolve to the real registry types; the unknown resolves to
    // an inert placeholder that still remembers its name, so a re-save writes
    // "dragon" back out unchanged.
    assert(&mc::gameplay::entities::resolveEntityTypeForRestore("pig") == &mc::gameplay::entities::builtinSpecies("pig"));
    assert(&mc::gameplay::entities::resolveEntityTypeForRestore("cow") == &mc::gameplay::entities::builtinSpecies("cow"));
    const auto& dragon = mc::gameplay::entities::resolveEntityTypeForRestore("dragon");
    assert(dragon.id().path == "dragon");
    assert(dragon.category() == MobCategory::Misc);
    // byId stays strict: the placeholder is reachable only through the restore
    // resolver, never a general name lookup a command would use.
    assert(mc::gameplay::entities::entityTypeRegistry().byId("dragon") == nullptr);
    // Interning is idempotent: the same name resolves to the very same placeholder
    // so repeated load/save cycles do not grow the table without bound.
    assert(&mc::gameplay::entities::resolveEntityTypeForRestore("dragon") == &dragon);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    testNameResolution();
    testNetworkIdRoundTrip();
    testLifecycleAborts();
    testUnknownEntityRoundTrip();
    return 0;
}
