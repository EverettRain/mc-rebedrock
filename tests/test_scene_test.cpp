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

#include <glm/vec3.hpp>

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
    {
        const auto preview = accept({"--scene"sv, "ssssssss/ssssssss/ssssssss/ssssssss/ssssssss/ssssssss/ssssssss/ssssssss"sv, "--key"sv, "s=stone"sv,
            "--export-preview"sv, "--sun-shadows"sv, "--shadow-entities"sv, "--sun-tick"sv, "3000"sv});
        assert(preview.sunShadows && preview.shadowEntities && preview.sunTick == 3000U);
        assert(rejects({"--test-scene"sv, "stone"sv, "--sun-shadows"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--sun-tick"sv, "24000"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--sun-tick"sv, "x"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--sun-tick"sv}));
        auto other = preview;
        other.sunShadows = false;
        assert(mc::render::previewDirectoryName(other) != mc::render::previewDirectoryName(preview));
        other = preview; other.sunTick = 6000U;
        assert(mc::render::previewDirectoryName(other) != mc::render::previewDirectoryName(preview));

        // RN-38：天气是命令行参数，不是一次临时改代码的探针。
        //
        // 它必须进输出目录名，理由与其余每一项相同：RN-15 的确定性规则要求输出路径是
        // 命令行的函数——否则「晴天那一版」与「雨天那一版」互相覆盖，而覆盖是静默的。
        const auto rainy = accept({"--test-scene"sv, "stone"sv, "--export-preview"sv,
                                   "--rain"sv, "1"sv, "--thunder"sv, "0.5"sv});
        assert(rainy.rainGradient == 1.0F && rainy.thunderGradient == 0.5F);
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--rain"sv, "2"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--rain"sv, "x"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--rain"sv}));
        assert(rejects({"--test-scene"sv, "stone"sv, "--export-preview"sv, "--thunder"sv, "-1"sv}));
        const auto dry = accept({"--test-scene"sv, "stone"sv, "--export-preview"sv});
        assert(mc::render::previewDirectoryName(rainy) != mc::render::previewDirectoryName(dry));
        // 只改雷暴那一条也要换名字：两条 gradient 各自都会改变画面
        auto onlyRain = rainy; onlyRain.thunderGradient = 0.0F;
        assert(mc::render::previewDirectoryName(onlyRain) !=
               mc::render::previewDirectoryName(rainy));
        // 晴天的名字不带天气后缀，既有基线因此不作废
        assert(mc::render::previewDirectoryName(dry) == "rebedrock_stone");
    }

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

    // --- RN-17: the structured scene. ---
    //
    // The pattern is the crafting table's shape: layers separated by `;`, rows by
    // `/`, one character per cell, and a legend of `--key`s. Everything the
    // single-block spec accepts, a legend entry accepts — because it is the same
    // parser, which is the point of the refactor that made it return a value
    // instead of writing into the options.
    {
        // The first use case this exists for: a stair standing on grass. One
        // column, one row, two layers — grass at the bottom, stair on top.
        const auto onGrass = accept({"--scene"sv, "g;s"sv, "--key"sv, "g=grass_block"sv,
                                     "--key"sv, "s=oak_stairs[facing=north,half=bottom]"sv});
        assert(onGrass.isScene());
        assert(onGrass.sceneSize == glm::ivec3(1, 2, 1));
        assert(onGrass.sceneCells.size() == 2U);
        // Layer 0 is the BOTTOM. A scene written bottom-up that photographed
        // top-down would put the stair underground and nothing would say so.
        assert(onGrass.sceneCells[0].offset == glm::ivec3(0, 0, 0));
        assert(onGrass.sceneCells[0].state.block() == Block::Grass);
        assert(onGrass.sceneCells[1].offset == glm::ivec3(0, 1, 0));
        assert(onGrass.sceneCells[1].state.block() == Block::OakStairs);
        // The legend's blockstate spec is the single-block spec, in full.
        assert(onGrass.sceneCells[1].state.orientation() == BlockOrientation::North);
        assert(onGrass.sceneCells[1].state.stairHalf() == SlabPortion::Bottom);

        // The second: two stairs stacked. One key, used twice.
        const auto stacked = accept({"--scene"sv, "s;s"sv, "--key"sv, "s=oak_stairs"sv});
        assert(stacked.sceneCells.size() == 2U);
        assert(stacked.sceneCells[0].offset == glm::ivec3(0, 0, 0));
        assert(stacked.sceneCells[1].offset == glm::ivec3(0, 1, 0));

        // Axes, all three at once, on a scene where every cell is distinguishable.
        // Columns run east (+x), rows run south (+z), layers run up (+y). Getting
        // any pair of these swapped produces a picture that looks plausible and is
        // of a different structure.
        const auto axes = accept({"--scene"sv, "ab/cd;e /  "sv, "--key"sv, "a=stone"sv, "--key"sv,
                                  "b=dirt"sv, "--key"sv, "c=sand"sv, "--key"sv, "d=gravel"sv,
                                  "--key"sv, "e=cobblestone"sv});
        assert(axes.sceneSize == glm::ivec3(2, 2, 2));
        assert(axes.sceneCells.size() == 5U); // the trailing space is air
        const auto cellAt = [&axes](glm::ivec3 offset) {
            for (const auto& cell : axes.sceneCells) {
                if (cell.offset == offset) return cell.state.block();
            }
            return Block::Air;
        };
        assert(cellAt({0, 0, 0}) == Block::Stone);      // layer 0, row 0, column 0
        assert(cellAt({1, 0, 0}) == Block::Dirt);       // one column east
        assert(cellAt({0, 0, 1}) == Block::Sand);       // one row south
        assert(cellAt({1, 0, 1}) == Block::Gravel);
        assert(cellAt({0, 1, 0}) == Block::Cobblestone); // one layer up
        assert(cellAt({1, 1, 0}) == Block::Air);         // the space

        // `--key` may be written before OR after `--scene`: a command line whose
        // meaning depends on flag order is one people will get wrong.
        const auto keyFirst = accept({"--key"sv, "s=oak_stairs"sv, "--scene"sv, "s"sv});
        assert(keyFirst.sceneCells.size() == 1U);
    }

    // --- RN-17: everything malformed about a scene throws, too. ---
    {
        // A symbol with no key, and a key no symbol uses. The second one matters
        // as much as the first: it is what a typo in the pattern looks like, and
        // without it the picture would simply be missing a block.
        assert(rejects({"--scene"sv, "sx"sv, "--key"sv, "s=stone"sv}));
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--key"sv, "g=dirt"sv}));
        // A ragged pattern. This is the one mistake that would otherwise shift a
        // whole row sideways and still render.
        //
        // Both directions, and the LONGER one is the case that matters: a row
        // shorter than the first would be read past its end and (by luck) hit a
        // character no key defines, so it throws even with the check gone. A row
        // that is longer simply loses its tail — no error, no sign, a picture of
        // a structure nobody asked for. Removing the length check has to fail
        // HERE, or the check is only being tested by undefined behaviour.
        assert(rejects({"--scene"sv, "ss/s"sv, "--key"sv, "s=stone"sv}));
        assert(rejects({"--scene"sv, "s/ss"sv, "--key"sv, "s=stone"sv}));
        assert(rejects({"--scene"sv, "s;ss"sv, "--key"sv, "s=stone"sv}));
        assert(rejects({"--scene"sv, "s/s;s"sv, "--key"sv, "s=stone"sv}));
        // Nothing to photograph.
        assert(rejects({"--scene"sv, "   "sv}));
        assert(rejects({"--scene"sv, ""sv}));
        assert(rejects({"--scene"sv, "/"sv, "--key"sv, "s=stone"sv}));
        // Malformed keys.
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "stone"sv}));      // no '='
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "ss=stone"sv}));   // two-char symbol
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s="sv}));         // no block
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "*=stone"sv}));    // not path-safe
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--key"sv, "s=dirt"sv}));
        // The legend inherits every blockstate rejection the single spec has.
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone[open=false]"sv}));
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=furnace[facing=down]"sv}));
        // `--key` without `--scene` is a legend for nothing.
        assert(rejects({"--key"sv, "s=stone"sv}));
        // Two answers to "what am I photographing".
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--test-scene"sv, "dirt"sv}));
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--occlusion-scene"sv}));
        // `--stage` spins a block through six orientations; every cell of a scene
        // already names its own state, so the two could only fight.
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--stage"sv, "2"sv}));
        assert(rejects({"--scene"sv, "s"sv, "--key"sv, "s=stone"sv, "--scene"sv, "ss"sv}));
        // Bigger than the preview world allows.
        assert(rejects({"--scene"sv, "sssssssss"sv, "--key"sv, "s=stone"sv}));
        assert(rejects({"--scene"sv, "s;s;s;s;s;s;s;s;s"sv, "--key"sv, "s=stone"sv}));
    }

    // --- RN-17: the scene's directory name. ---
    {
        const auto onGrass = accept({"--scene"sv, "g;s"sv, "--key"sv, "g=grass_block"sv,
                                     "--key"sv, "s=oak_stairs[facing=north]"sv});
        const std::string name = mc::render::previewDirectoryName(onGrass);
        assert(name == "scene__g+s__g-rebedrock_grass_block__s-rebedrock_oak_stairs.facing-north");
        assert(name.find(':') == std::string::npos);
        // Same command, same directory.
        assert(mc::render::previewDirectoryName(
                   accept({"--scene"sv, "g;s"sv, "--key"sv, "g=grass_block"sv, "--key"sv,
                           "s=oak_stairs[facing=north]"sv})) == name);
        // The PATTERN is in the name, not just its size and legend. These two
        // scenes have the same size and the same legend and are different
        // pictures; sharing a directory would have the second run overwrite the
        // first with nothing to show that it had.
        const auto ab = accept({"--scene"sv, "sd"sv, "--key"sv, "s=stone"sv, "--key"sv,
                                "d=dirt"sv});
        const auto ba = accept({"--scene"sv, "ds"sv, "--key"sv, "s=stone"sv, "--key"sv,
                                "d=dirt"sv});
        assert(mc::render::previewDirectoryName(ab) != mc::render::previewDirectoryName(ba));
        // A space is air and shows in the name as `.`, so a gap is part of the
        // identity too.
        const auto gap = accept({"--scene"sv, "s s"sv, "--key"sv, "s=stone"sv});
        assert(mc::render::previewDirectoryName(gap) == "scene__s.s__s-rebedrock_stone");
        // And a long one is cut and hashed rather than refused — but stays unique.
        const auto longA =
            accept({"--scene"sv, "abcdefg;abcdefg"sv, "--key"sv, "a=oak_stairs[facing=north]"sv,
                    "--key"sv, "b=oak_stairs[facing=south]"sv, "--key"sv,
                    "c=oak_stairs[facing=east]"sv, "--key"sv, "d=oak_stairs[facing=west]"sv,
                    "--key"sv, "e=oak_trapdoor[open=true]"sv, "--key"sv,
                    "f=oak_trapdoor[open=false]"sv, "--key"sv, "g=grass_block"sv});
        const auto longB =
            accept({"--scene"sv, "abcdefg;abcdefg"sv, "--key"sv, "a=oak_stairs[facing=north]"sv,
                    "--key"sv, "b=oak_stairs[facing=south]"sv, "--key"sv,
                    "c=oak_stairs[facing=east]"sv, "--key"sv, "d=oak_stairs[facing=west]"sv,
                    "--key"sv, "e=oak_trapdoor[open=true]"sv, "--key"sv,
                    "f=oak_trapdoor[open=false]"sv, "--key"sv, "g=dirt"sv});
        const std::string cutA = mc::render::previewDirectoryName(longA);
        const std::string cutB = mc::render::previewDirectoryName(longB);
        assert(cutA.size() <= mc::render::kMaxPreviewDirectoryName);
        assert(cutB.size() <= mc::render::kMaxPreviewDirectoryName);
        // They share every character up to the cut; only the hash separates them,
        // which is exactly the case a plain truncation would collide on.
        assert(cutA != cutB);
    }

    // --- RN-17: the single-block form is untouched. ---
    //
    // RN-15's stored baselines (export/blocks-preview-verify) are keyed on the
    // single-block directory name. Adding the scene form must not rename them.
    {
        const auto plain = accept({"--test-scene"sv, "oak_stairs"sv});
        assert(!plain.isScene());
        assert(plain.sceneCells.empty());
        assert(mc::render::previewDirectoryName(plain) == "rebedrock_oak_stairs");
        const auto stated = accept({"--test-scene"sv, "oak_trapdoor[open=true,half=top]"sv});
        assert(mc::render::previewDirectoryName(stated) ==
               "rebedrock_oak_trapdoor__open-true__half-top");
        // And `--stage` still works on it.
        assert(accept({"--test-scene"sv, "furnace"sv, "--stage"sv, "3"sv}).stage == 3);
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
