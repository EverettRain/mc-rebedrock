// R0-5 external registration phase + freeze gate.
//
// The block registry walks Bootstrap -> External -> Freeze. Built-ins register
// first so their ids equal their enum ordinals no matter what external content a
// world carries (the id-stability guarantee the save/network layers rely on);
// external content registers in the open phase and continues the dense id
// sequence; and once frozen the table refuses any further registration. This
// pins all three: a fake external block registers and resolves by name, built-in
// ids do not move, and registering after freeze aborts.

#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"

#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <span>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

namespace {

// Runs `body` in a child process and returns whether it aborted (SIGABRT). The
// lifecycle guarantees are aborts, not return codes, so they are checked in a
// forked child the way registry_test does; stderr is dropped so a pass stays
// quiet.
bool aborts(const std::function<void()>& body) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        body();
        _exit(0);  // reached only when body did NOT abort
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// A block this build's own content does not define, standing in for a datapack
// block a loader parsed into a BlockDefinition. It borrows an existing
// definition's shape and only swaps the identity, which is all the registry
// cares about.
[[nodiscard]] mc::world::ExternalBlockDef makeExternalBlock(std::string_view path) {
    mc::world::ExternalBlockDef def{mc::world::blockDefinition(mc::world::Block::Stone)};
    def.definition.identifier.space = "testmod";
    def.definition.identifier.path = path;
    def.definition.vanilla = {};  // original content, no vanilla mirror
    return def;
}

}  // namespace

int main() {
    using namespace mc;
    using mc::world::Block;

    // --- A build with no external content registers exactly the built-ins. ---
    {
        const auto registry = world::buildBlockRegistry({});
        assert(registry.size() == world::kBuiltinBlockCount);
        assert(registry.phase() == core::RegistryPhase::Freeze);
        assert(registry.byName("rebedrock:stone").index() ==
               static_cast<std::size_t>(Block::Stone));
    }

    // --- External content registers after the built-ins and resolves by name,
    //     without disturbing a single built-in id. ---
    {
        const std::array external{makeExternalBlock("widget"), makeExternalBlock("gadget")};
        const auto registry = world::buildBlockRegistry(external);

        // Every built-in still sits at its enum ordinal: external content did not
        // renumber anything below it.
        for (std::size_t ordinal = 0; ordinal < world::kBuiltinBlockCount; ++ordinal) {
            const auto block = static_cast<Block>(ordinal);
            assert(registry.byName(world::blockDefinition(block).identifier).index() == ordinal);
        }

        // The externals continue the dense id sequence in registration order.
        assert(registry.size() == world::kBuiltinBlockCount + 2U);
        const auto widget = registry.byName("testmod:widget");
        const auto gadget = registry.byName("testmod:gadget");
        assert(widget.valid() && widget.index() == world::kBuiltinBlockCount);
        assert(gadget.valid() && gadget.index() == world::kBuiltinBlockCount + 1U);
        // The name round-trips back out of the registry.
        assert(registry.identifier(widget).toString() == "testmod:widget");
        assert(registry.phase() == core::RegistryPhase::Freeze);
    }

    // --- The freeze gate: registering after the registry is frozen aborts,
    //     rather than silently handing out an id no frozen consumer expects. ---
    {
        assert(aborts([] {
            auto registry = world::buildBlockRegistry({});
            registry.registerExternal(core::Identifier{"testmod", "late"},
                                      world::blockDefinition(Block::Stone));
        }));
    }

    // --- The process singleton is frozen for real. ---
    assert(world::blockRegistry().phase() == core::RegistryPhase::Freeze);

    return 0;
}
