// DDC-0: the runtime enchantment-identity registry.
//
// Covers the foundation DDC-0 lays: the constexpr enchantment table
// (Enchantment.hpp's baked default) poured into a runtime core::Registry with a
// three-phase lifecycle and a datapack-override External slot, plus the DataStore
// baked->External bridge extended to the EnchantmentDefinition Def type. The
// point is to prove the runtime identity layer is byte-for-byte equivalent to the
// constexpr table (golden parity), that ids stay dense and stable, and that the
// External/DataStore path a datapack will use claims fresh ids without disturbing
// the baked ones.
//
// The lifecycle guarantees are aborts, so a "must abort" contract is checked in a
// forked child, the same idiom registry_test uses.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Registry.hpp"
#include "data/DataStore.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EnchantmentRegistry.hpp"

#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace mc;
using namespace mc::gameplay;

namespace {

bool aborts(const std::function<void()>& body) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        body();
        _exit(0);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// Every baked default is present, one dense id per EnchantmentId, and the runtime
// id equals the enum ordinal (so a stored ordinal subscripts the table directly).
void testBakedDefaultsPresent() {
    const EnchantmentRegistry& registry = enchantmentRegistry();
    assert(registry.size() == kEnchantmentCount);
    assert(registry.phase() == core::RegistryPhase::Freeze);

    for (std::size_t index = 0; index < kEnchantmentCount; ++index) {
        const auto enumId = static_cast<EnchantmentId>(index);
        const core::EnchantmentTypeId typeId = enchantmentTypeId(enumId);
        assert(typeId.valid());
        assert(typeId.index() == index);
        // The runtime dense id equals the enum ordinal: registration ran in enum
        // order, the DDC-0 id-stability guarantee a save relies on.
        assert(registry.byName(enchantmentVanillaName(enumId)) == typeId);
    }
    std::cout << "testBakedDefaultsPresent OK\n";
}

// The runtime Def is byte-for-byte the constexpr Def — the golden parity DDC-0
// must not break: moving the fill from compile time to load time changes when the
// table is built, not what it holds.
void testRuntimeMatchesConstexpr() {
    const EnchantmentRegistry& registry = enchantmentRegistry();
    for (std::size_t index = 0; index < kEnchantmentCount; ++index) {
        const auto enumId = static_cast<EnchantmentId>(index);
        const EnchantmentDefinition& runtime = registry.get(enchantmentTypeId(enumId));
        const EnchantmentDefinition& baked = enchantmentDefinition(enumId);
        assert(runtime.id == baked.id);
        assert(runtime.vanillaName == baked.vanillaName);
        assert(runtime.minLevel == baked.minLevel);
        assert(runtime.maxLevel == baked.maxLevel);
        assert(runtime.rarity == baked.rarity);
        assert(runtime.category == baked.category);
        assert(runtime.cost.minBase == baked.cost.minBase);
        assert(runtime.cost.minPerLevel == baked.cost.minPerLevel);
        assert(runtime.cost.maxOffset == baked.cost.maxOffset);
        assert(runtime.treasureOnly == baked.treasureOnly);
        assert(runtime.curse == baked.curse);
        assert(runtime.availableForRandomSelection == baked.availableForRandomSelection);
    }
    std::cout << "testRuntimeMatchesConstexpr OK\n";
}

// Name resolution: the `rebedrock:` key, the `minecraft:` alias, and the bare
// path all reach the same id; the stable save name reverses.
void testNameResolutionAndAlias() {
    const core::EnchantmentTypeId sharpness = enchantmentTypeByName("sharpness");
    assert(sharpness.valid());
    assert(enchantmentTypeByName("rebedrock:sharpness") == sharpness);
    assert(enchantmentTypeByName("minecraft:sharpness") == sharpness);
    assert(enchantmentTypeId(EnchantmentId::Sharpness) == sharpness);
    assert(enchantmentTypeName(sharpness) == "sharpness");

    // A name no enchantment owns misses cleanly, never aborts.
    assert(!enchantmentTypeByName("rebedrock:not_an_enchantment").valid());
    assert(enchantmentTypeName(core::EnchantmentTypeId::invalid()).empty());
    std::cout << "testNameResolutionAndAlias OK\n";
}

// The External slot: a datapack-style enchantment registered after the baked
// floor claims the next dense id (kEnchantmentCount), and the baked ids are
// undisturbed — the id-stability guarantee under an override.
void testExternalClaimsNextId() {
    const EnchantmentDefinition custom{EnchantmentId::Count, "example_custom_ench", 1, 3,
                                       EnchantmentRarity::Rare, EnchantmentCategory::Weapon,
                                       {5, 8, 20}, false, false, true};
    const std::array<EnchantmentDefinition, 1> external{custom};
    const EnchantmentRegistry registry = buildEnchantmentRegistry(external);

    assert(registry.size() == kEnchantmentCount + 1U);
    // Baked ids unchanged.
    assert(registry.byName("sharpness").index() ==
           static_cast<std::size_t>(EnchantmentId::Sharpness));
    // External content took the next dense id, reachable by name.
    const core::EnchantmentTypeId customId = registry.byName("example_custom_ench");
    assert(customId.valid());
    assert(customId.index() == kEnchantmentCount);
    assert(registry.get(customId).maxLevel == 3);
    std::cout << "testExternalClaimsNextId OK\n";
}

// The DataStore bridge, extended to the EnchantmentDefinition Def type. DDC-0's
// deliverable is that the baked floor flows through the same generic DataStore
// bridge recipes/loot/blockentities use — baked default underneath, the overlay
// (DDC-1's JSON) and registerAdditionsInto's External hand-off inherited from the
// generic store. This proves the Def type instantiates and round-trips through
// that bridge, and that the additions-only contract holds: a floor-only store
// registers nothing extra into a registry already carrying the built-ins.
void testDataStoreBridge() {
    data::DataStore<EnchantmentDefinition> store = bakedEnchantmentStore();
    assert(store.size() == kEnchantmentCount);
    assert(store.contains("rebedrock:sharpness"));
    // find() round-trips the baked Def by name.
    const EnchantmentDefinition* sharp = store.find("rebedrock:sharpness");
    assert(sharp != nullptr);
    assert(sharp->maxLevel == enchantmentMaxLevel(EnchantmentId::Sharpness));
    // The baked floor is built-in, never an overlay addition.
    for (const auto& entry : store.entries()) {
        assert(!entry.fromOverlay);
    }

    // A registry with the baked built-ins in Bootstrap, opened to External so a
    // datapack's additions could be hung on it. A floor-only store has no
    // additions, so registerAdditionsInto is a no-op — the additions-only
    // contract that keeps baked ids stable while external content gets fresh ids.
    EnchantmentRegistry registry;
    for (const EnchantmentDefinition& def : kEnchantmentTable) {
        registerEnchantment(registry, def, /*external=*/false);
    }
    registry.beginExternal();
    store.registerAdditionsInto(registry);
    registry.freeze();

    assert(registry.size() == kEnchantmentCount);
    assert(registry.byName("sharpness").index() ==
           static_cast<std::size_t>(EnchantmentId::Sharpness));
    std::cout << "testDataStoreBridge OK\n";
}

// The lifecycle aborts the runtime layer inherits from core::Registry — the
// sabotage safety net: a mis-phased or duplicate registration dies rather than
// silently corrupting identity.
void testLifecycleAborts() {
    // registerExternal before beginExternal aborts.
    assert(aborts([] {
        EnchantmentRegistry r;
        const EnchantmentDefinition def{EnchantmentId::Count, "x", 1, 1,
                                        EnchantmentRarity::Common, EnchantmentCategory::Weapon,
                                        {1, 1, 1}, false, false, true};
        registerEnchantment(r, def, /*external=*/true);
    }));
    // A duplicate name aborts (two enchantments claiming one key).
    assert(aborts([] {
        EnchantmentRegistry r;
        const EnchantmentDefinition def{EnchantmentId::Count, "dup", 1, 1,
                                        EnchantmentRarity::Common, EnchantmentCategory::Weapon,
                                        {1, 1, 1}, false, false, true};
        registerEnchantment(r, def, /*external=*/false);
        registerEnchantment(r, def, /*external=*/false);
    }));
    // No registration survives freeze.
    assert(aborts([] {
        EnchantmentRegistry r = buildEnchantmentRegistry({});
        const EnchantmentDefinition def{EnchantmentId::Count, "late", 1, 1,
                                        EnchantmentRarity::Common, EnchantmentCategory::Weapon,
                                        {1, 1, 1}, false, false, true};
        r.registerBuiltin(core::Identifier{core::kNamespace, def.vanillaName}, def);
    }));
    // Deref of an id the registry never handed out aborts.
    assert(aborts([] {
        const EnchantmentRegistry& r = enchantmentRegistry();
        (void)r.get(core::EnchantmentTypeId::of(
            static_cast<core::EnchantmentTypeId::Value>(kEnchantmentCount + 100U)));
    }));
    std::cout << "testLifecycleAborts OK\n";
}

}  // namespace

int main() {
    testBakedDefaultsPresent();
    testRuntimeMatchesConstexpr();
    testNameResolutionAndAlias();
    testExternalClaimsNextId();
    testDataStoreBridge();
    testLifecycleAborts();
    std::cout << "enchantment_runtime_registry_test: all tests passed\n";
    return 0;
}
