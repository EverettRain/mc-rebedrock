// R0-1 registry + id foundation.
//
// Covers the four moving parts the identity底座 stands on: the strong dense id
// types, the Identifier interner, the generic Registry with its three-phase
// lifecycle, and the built-in block registry poured out of kBlockRegistry.
//
// The lifecycle guarantees are aborts, not return codes, so they are checked in
// a forked child: `aborts(body)` runs `body` in a subprocess and reports
// whether it died on SIGABRT. That is how a "must abort" contract is asserted
// without taking the test process down with it.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/IdentifierInterner.hpp"
#include "core/Registry.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

namespace {

using mc::core::Identifier;
using mc::core::kNamespace;
using mc::core::kVanillaNamespace;

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

using IntBlockRegistry = mc::core::Registry<int, mc::core::BlockId>;

Identifier rebedrock(std::string_view path) { return Identifier{kNamespace, path}; }

void testStrongIds() {
    using mc::core::BlockId;
    using mc::core::ItemId;

    // Distinct types, two bytes, deref = index.
    static_assert(!std::is_same_v<BlockId, ItemId>);
    static_assert(sizeof(BlockId) == 2);

    assert(!BlockId::invalid().valid());
    assert(!BlockId{}.valid());
    const BlockId five = BlockId::of(5);
    assert(five.valid());
    assert(five.value() == 5);
    assert(five.index() == 5U);
    assert(five == BlockId::of(5));
    assert(five != BlockId::of(6));
    assert(BlockId::of(4) < BlockId::of(5));
}

void testIdentifierParse() {
    assert(Identifier::parse("rebedrock:stone") == rebedrock("stone"));
    const Identifier bare = Identifier::parse("stone");
    assert(bare.space.empty());
    assert(bare.path == "stone");
    // A colon inside the path only splits on the first one.
    assert(Identifier::parse("a:b:c") == (Identifier{"a", "b:c"}));
}

void testInterner() {
    mc::core::IdentifierInterner interner;
    const auto foo = interner.intern(rebedrock("foo"));
    const auto fooAgain = interner.intern(rebedrock("foo"));
    const auto bar = interner.intern(rebedrock("bar"));
    // Same key -> same id, distinct key -> distinct id, dense from zero.
    assert(foo == fooAgain);
    assert(foo != bar);
    assert(foo.value() == 0);
    assert(bar.value() == 1);
    assert(interner.size() == 2U);
    // find never inserts; a miss is an invalid id, not a new slot.
    assert(interner.find(rebedrock("foo")) == foo);
    assert(!interner.find(rebedrock("missing")).valid());
    assert(interner.size() == 2U);
    // Reverse resolves to the canonical identifier, viewing owned storage.
    assert(interner.identifier(foo) == rebedrock("foo"));
    // The key was copied: an Identifier over a temporary string still round
    // trips after the temporary is gone.
    {
        const std::string transient = "ephemeral";
        interner.intern(Identifier{kNamespace, transient});
    }
    assert(interner.identifier(interner.find(rebedrock("ephemeral"))) == rebedrock("ephemeral"));
}

void testRegistryRoundTrip() {
    IntBlockRegistry registry;
    assert(registry.phase() == mc::core::RegistryPhase::Bootstrap);

    const auto alpha = registry.registerBuiltin(rebedrock("alpha"), 10);
    const auto beta = registry.registerBuiltin(rebedrock("beta"), 20);
    // Ids are dense and assigned in registration order.
    assert(alpha.value() == 0);
    assert(beta.value() == 1);
    assert(registry.size() == 2U);

    // register -> byName -> get -> reverse-name all agree.
    assert(registry.byName(rebedrock("alpha")) == alpha);
    assert(registry.byName("rebedrock:beta") == beta);
    assert(registry.get(alpha) == 10);
    assert(registry.get(beta) == 20);
    assert(registry.identifier(alpha) == rebedrock("alpha"));

    // Aliases: an extra name pointing at an existing id, reachable by full name
    // and (boundary convenience) bare name.
    registry.alias(Identifier{kVanillaNamespace, "alpha"}, alpha);
    assert(registry.byName("minecraft:alpha") == alpha);
    assert(registry.byName("alpha") == alpha);

    // A name nobody owns is an invalid id, not an abort.
    assert(!registry.byName("rebedrock:missing").valid());
    assert(!registry.byName(rebedrock("missing")).valid());

    // External content registers after the phase opens, continuing the dense id
    // sequence; freeze then locks the table.
    registry.beginExternal();
    assert(registry.phase() == mc::core::RegistryPhase::External);
    const auto gamma = registry.registerExternal(rebedrock("gamma"), 30);
    assert(gamma.value() == 2);
    assert(registry.byName("rebedrock:gamma") == gamma);
    registry.freeze();
    assert(registry.phase() == mc::core::RegistryPhase::Freeze);
}

void testLifecycleAborts() {
    // registerBuiltin is Bootstrap-only.
    assert(aborts([] {
        IntBlockRegistry r;
        r.beginExternal();
        r.registerBuiltin(rebedrock("x"), 1);
    }));
    // registerExternal is External-only.
    assert(aborts([] {
        IntBlockRegistry r;
        r.registerExternal(rebedrock("x"), 1);
    }));
    // No registration survives a freeze.
    assert(aborts([] {
        IntBlockRegistry r;
        r.freeze();
        r.registerBuiltin(rebedrock("x"), 1);
    }));
    // The phase machine only steps forward, once.
    assert(aborts([] {
        IntBlockRegistry r;
        r.beginExternal();
        r.beginExternal();
    }));
    assert(aborts([] {
        IntBlockRegistry r;
        r.freeze();
        r.freeze();
    }));
    // Two pieces of content cannot claim one name.
    assert(aborts([] {
        IntBlockRegistry r;
        r.registerBuiltin(rebedrock("x"), 1);
        r.registerBuiltin(rebedrock("x"), 2);
    }));
    // An alias cannot shadow a name already bound.
    assert(aborts([] {
        IntBlockRegistry r;
        const auto a = r.registerBuiltin(rebedrock("a"), 1);
        r.registerBuiltin(rebedrock("b"), 2);
        r.alias(rebedrock("a"), a);
    }));
    // Deref of an invalid / never-assigned id aborts.
    assert(aborts([] {
        IntBlockRegistry r;
        (void)r.get(mc::core::BlockId::invalid());
    }));
    assert(aborts([] {
        IntBlockRegistry r;
        (void)r.get(mc::core::BlockId::of(0));
    }));
    // Aliasing to an id the registry never handed out aborts.
    assert(aborts([] {
        IntBlockRegistry r;
        r.alias(rebedrock("a"), mc::core::BlockId::of(3));
    }));
}

void testBlockRegistry() {
    const mc::world::BlockRegistry& registry = mc::world::blockRegistry();
    // One entry per block, and the table is frozen for consumers.
    assert(registry.size() == static_cast<std::size_t>(mc::world::Block::Count));
    assert(registry.phase() == mc::core::RegistryPhase::Freeze);

    const mc::core::BlockId stone = registry.byName("rebedrock:stone");
    assert(stone.valid());
    // The vanilla alias and the bare name reach the same id.
    assert(registry.byName("minecraft:stone") == stone);
    assert(registry.byName("stone") == stone);
    // Built-in id equals the enum ordinal (R0-2 relies on this).
    assert(stone.index() == static_cast<std::size_t>(mc::world::Block::Stone));
    assert(registry.get(stone).block == mc::world::Block::Stone);
    assert(registry.identifier(stone) == rebedrock("stone"));

    // Air is the first-registered block, id 0, reachable through every key form.
    const mc::core::BlockId air = registry.byName("rebedrock:air");
    assert(air.value() == 0);
    assert(registry.byName("minecraft:air") == air);
    assert(registry.get(air).block == mc::world::Block::Air);

    // A name no block owns misses cleanly.
    assert(!registry.byName("rebedrock:not_a_block").valid());

    // Every block round-trips: name -> id -> canonical name.
    for (const mc::world::BlockDefinition& definition : mc::world::kBlockRegistry) {
        const mc::core::BlockId id = registry.byName(definition.identifier);
        assert(id.valid());
        assert(id.index() == static_cast<std::size_t>(definition.block));
        assert(registry.identifier(id) == definition.identifier);
        assert(registry.get(id).block == definition.block);
    }
}

} // namespace

int main() {
    testStrongIds();
    testIdentifierParse();
    testInterner();
    testRegistryRoundTrip();
    testLifecycleAborts();
    testBlockRegistry();
    return 0;
}
