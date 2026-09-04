// The test-scene command line, and (RN-15c) its blockstate spec.
//
// The discipline this file exists to hold: a malformed argument THROWS. The save
// loader's rule is the opposite — skip a property it does not understand, so a
// world from a newer build still opens — and copying that here would let an
// automation photograph a state nobody asked for and file it as a baseline.
// `parseTestSceneArguments` has carried the "throw" rule for the block id since
// it was written; RN-15c extends it over the state.

#include "core/PackArguments.hpp"
#include "render/TestScene.hpp"

#include <array>
#include <cassert>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::string_view_literals;
using mc::render::parseTestSceneArguments;

[[nodiscard]] bool rejects(std::initializer_list<std::string_view> arguments) {
    const std::vector<std::string_view> stored{arguments};
    try {
        static_cast<void>(parseTestSceneArguments(stored));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

[[nodiscard]] bool rejectsPack(std::initializer_list<std::string_view> arguments) {
    const std::vector<std::string_view> stored{arguments};
    try {
        static_cast<void>(mc::parsePackArguments(stored));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

[[nodiscard]] mc::render::TestSceneOptions accept(
    std::initializer_list<std::string_view> arguments) {
    const std::vector<std::string_view> stored{arguments};
    const auto scene = parseTestSceneArguments(stored);
    assert(scene.has_value());
    return *scene;
}

} // namespace

int main() {
    using mc::world::Block;
    using mc::world::BlockOrientation;
    using mc::world::DoorHinge;
    using mc::world::SlabPortion;

    // --- What was there before RN-15c, unchanged. ---
    {
        const auto scene = accept({"--test-scene"sv, "minecraft:furnace"sv, "--stage"sv, "3"sv});
        assert(scene.block == Block::Furnace);
        assert(scene.stage == 3);
        assert(scene.state == mc::world::BlockState{Block::Furnace});
        assert(!scene.stateSetsFacing);
        assert(!scene.exportPreview);
        const std::array none{"--unrelated"sv};
        assert(!parseTestSceneArguments(none).has_value());
        assert(rejects({"--test-scene"sv, "missing"sv}));
        assert(rejects({"--stage"sv, "1"sv}));           // --stage without a scene
        assert(rejects({"--test-scene"sv, "stone"sv, "--stage"sv, "10"sv}));
    }

    // --- RN-15c: the state spec. `--stage` can only rotate six orientations,
    //     and the properties this line needs to photograph are the other ones. ---
    {
        const auto scene =
            accept({"--test-scene"sv, "oak_trapdoor[open=true,half=top]"sv});
        assert(scene.block == Block::OakTrapdoor);
        assert(scene.state.open());
        assert(scene.state.trapdoorHalf() == SlabPortion::Top);
        assert(!scene.stateSetsFacing);

        const auto door = accept({"--test-scene"sv, "oak_door[hinge=right,half=upper]"sv});
        assert(door.state.hinge() == DoorHinge::Right);
        assert(door.state.isDoorUpperHalf());

        const auto diode = accept({"--test-scene"sv, "repeater[delay=3,powered=true]"sv});
        assert(diode.state.repeaterDelay() == 3);
        assert(diode.state.powered());

        const auto comparator = accept({"--test-scene"sv, "comparator[mode=subtract]"sv});
        assert(comparator.state.comparatorSubtract());

        // `facing` is recorded separately, because the single-block scene also
        // spins the block from `--stage`. The two must not both drive it.
        const auto facing = accept({"--test-scene"sv, "furnace[facing=west]"sv});
        assert(facing.stateSetsFacing);
        assert(facing.state.orientation() == BlockOrientation::West);
    }

    // --- Everything malformed throws. Each of these is a way an automation could
    //     otherwise render the wrong thing and report success. ---
    {
        // A property this build has no notion of.
        assert(rejects({"--test-scene"sv, "stone[nonsense=1]"sv}));
        // A property this build HAS, but this block does not declare — the case
        // that would otherwise silently render the block's default state.
        assert(rejects({"--test-scene"sv, "stone[open=true]"sv}));
        assert(rejects({"--test-scene"sv, "oak_stairs[delay=2]"sv}));
        // The same, with a value that maps to ordinal 0. This one is the reason
        // the "does this block declare it" check has to exist separately: a
        // property the block does not have reports a value count of 1, so 0 is
        // "in range" and the range check below lets it past. Without both checks
        // this renders plain stone and reports success.
        assert(rejects({"--test-scene"sv, "stone[open=false]"sv}));
        // A value the property does not take.
        assert(rejects({"--test-scene"sv, "oak_trapdoor[half=sideways]"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[open=maybe]"sv}));
        // A vanilla value the mapping refuses outright: `delay` is 1..4, so 9 is
        // not a delay at all and never reaches the block.
        assert(rejects({"--test-scene"sv, "repeater[delay=9]"sv}));
        // And the case only the range check catches: a value that is perfectly
        // real for the NAME but outside this block's axis. `facing` spans six
        // directions; a furnace's is horizontal-only, four wide. BlockStateTable
        // CLAMPS such a value to 0 rather than refusing it, so without the check
        // the picture would be a north-facing furnace labelled `facing=down`.
        assert(rejects({"--test-scene"sv, "furnace[facing=down]"sv}));
        assert(rejects({"--test-scene"sv, "oak_stairs[facing=up]"sv}));
        // Malformed brackets and pairs.
        assert(rejects({"--test-scene"sv, "oak_trapdoor[open=true"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[]"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[open]"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[=true]"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[open=]"sv}));
        assert(rejects({"--test-scene"sv, "oak_trapdoor[open=true,]"sv}));
    }

    // --- RN-15d: the export switches. ---
    {
        const auto scene = accept({"--test-scene"sv, "oak_stairs"sv, "--export-preview"sv,
                                   "--preview-size"sv, "256"sv, "--preview-out"sv, "/tmp/x"sv});
        assert(scene.exportPreview);
        assert(scene.previewSize == 256U);
        assert(scene.previewRoot == std::filesystem::path{"/tmp/x"});
        // A default that does not depend on the window: an export sized by the
        // monitor it ran on cannot be diffed against one from another machine.
        assert(accept({"--test-scene"sv, "stone"sv}).previewSize == 512U);
        assert(rejects({"--test-scene"sv, "stone"sv, "--preview-size"sv, "7"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--preview-size"sv, "9000"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--preview-size"sv, "big"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--preview-out"sv}));
        // The occlusion scene has no single block to photograph.
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv,
                        "--occlusion-scene"sv}));
    }

    // --- The output directory name. Deterministic, filesystem-safe on both
    //     platforms this project ships, and it says which state it is. ---
    {
        const auto plain = accept({"--test-scene"sv, "oak_stairs"sv});
        assert(mc::render::previewDirectoryName(plain) == "rebedrock_oak_stairs");
        const auto stated = accept({"--test-scene"sv, "oak_trapdoor[open=true,half=top]"sv});
        assert(mc::render::previewDirectoryName(stated) ==
               "rebedrock_oak_trapdoor__open-true__half-top");
        // No `:` survives: legal in a POSIX path, illegal on Windows.
        const auto name = mc::render::previewDirectoryName(stated);
        assert(name.find(':') == std::string::npos);
        // Same command, same directory — every time.
        assert(mc::render::previewDirectoryName(
                   accept({"--test-scene"sv, "oak_trapdoor[open=true,half=top]"sv})) == name);
    }

    // --- RN-15d: `--pack`. This build ships no Mojang assets, so an export with
    //     no pack behind it renders eight pictures of missing textures — the
    //     hardest kind of failure to notice, because the files are all there. ---
    {
        const std::array none{"--test-scene"sv, "stone"sv};
        assert(mc::parsePackArguments(none).empty());
        const std::array one{"--pack"sv, "/packs/vanilla.zip"sv};
        const std::vector<std::string> single{"/packs/vanilla.zip"};
        assert(mc::parsePackArguments(one) == single);
        // Several, in the order given: the stack's order is the caller's to
        // decide, so the parser must not sort or deduplicate.
        const std::array many{"--pack"sv, "/a"sv, "--test-scene"sv, "stone"sv, "--pack"sv,
                              "/b"sv};
        const std::vector<std::string> expected{"/a", "/b"};
        assert(mc::parsePackArguments(many) == expected);
        assert(rejectsPack({"--pack"sv}));
        // An empty path is `std::filesystem`'s current directory, so `--pack ""`
        // would quietly mount the working directory as a resource pack.
        assert(rejectsPack({"--pack"sv, ""sv}));
    }

    return 0;
}
