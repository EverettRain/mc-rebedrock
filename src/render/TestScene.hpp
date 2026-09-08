#pragma once

// 测试场景的命令行开关
// 渲染回归用：把世界换成一个内容确定的小场景，截图因此能逐帧对比
// 场景可以是单个方块的各生长阶段，也可以是受控的遮挡场景
// 正常游戏不经过这里

#include "render/BlockPreviewCamera.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::render {

// The pattern's layers map to y (first layer is the bottom), its rows to z (first
// row is the north edge, so a written scene reads like a map seen from above),
// and its columns to x. `SceneCell` itself is declared with the camera that
// frames it (BlockPreviewCamera.hpp) — one declaration, so the parser and the
// camera cannot disagree about which axis is which.

// The scene's extent, in cells, on each axis. Small on purpose: the preview
// world is a single chunk and the camera frames the whole structure, so a scene
// larger than this is not a preview any more.
inline constexpr int kMaxSceneExtent = 8;

struct TestSceneOptions final {
    world::Block block = world::Block::Stone;
    // RN-15c: the whole state, not just the block. `--stage` can only rotate the
    // six orientations, and the properties this line most needs to look at are
    // open/half/hinge/powered/delay/locked/in_wall — so the scene spec accepts
    // `oak_trapdoor[open=true,half=top]` and this carries the result.
    world::BlockState state{world::Block::Stone};
    // Whether the spec named `facing` itself. When it did, `--stage` must not
    // also spin the block: the two would fight and the picture would silently
    // not be the state that was asked for.
    bool stateSetsFacing = false;
    // The properties as they were spelled on the command line, in order. Only
    // used to name the output directory, so the same command always writes the
    // same path (RN-15's determinism rule reaches the file names too).
    std::vector<std::string> stateSpec;
    int stage = 0;
    // 渲染一个受控的遮挡测试场景：平整石台加埋在下面的洞穴，再开一个地表开口
    // 这样遮挡查询的结果是可预期的
    bool occlusionScene = false;

    // RN-17: the structured multi-block scene, empty for the single-block form.
    //
    // The single-block form is left exactly as it was, down to its directory
    // name, because RN-15's stored baselines are keyed on that name and the
    // whole value of this tool is that two runs are comparable.
    std::vector<SceneCell> sceneCells;
    // Extent in cells: x = columns, y = layers, z = rows.
    glm::ivec3 sceneSize{0, 0, 0};
    // The `--scene` pattern and the `--key` legend entries as they were spelled,
    // in order. Only used to name the output directory — which must therefore be
    // a function of the command line and nothing else (RN-15 §4's determinism
    // rule reaches the file names too).
    std::string scenePattern;
    std::vector<std::string> sceneLegend;

    [[nodiscard]] bool isScene() const { return !sceneCells.empty(); }

    // RN-15d: render the block from the eight corner viewpoints, write one PNG
    // each under `previewRoot`, and exit. Non-zero exit if any one of them fails
    // — seven images out of eight, silently, is the worst outcome for something
    // an automation diffs.
    bool exportPreview = false;
    // RN-11b：仅隐藏导出可用，默认保持旧预览不投影。固定实体是验收夹具，不是场景编辑器。
    bool sunShadows = false;
    bool shadowEntities = false;
    std::optional<std::uint32_t> sunTick;
    // RN-38：天气。0..1 的两条渐变量，与 vanilla 的 rainLevel / thunderLevel 同义。
    //
    // 出图从前把天气写死成晴（applyPreviewDeterminism 的第一行），于是任何与降雨有关的
    // 判断——RN-36 的「雨天影子该变浅」是第一个——都只能靠临时改代码打探针去量，
    // 而探针不是仪器：它不进版本库、不进目录名、下一个人重现不了。
    // 任何会改变画面的世界状态都应当是命令行上的一个参数，这两条是补上的第一批。
    float rainGradient = 0.0F;
    float thunderGradient = 0.0F;
    // Square, and fixed rather than taken from the window: an export whose size
    // depends on the monitor it ran on cannot be compared with one from another
    // machine, and RN-15 is a comparison tool before it is anything else.
    std::uint32_t previewSize = 512U;
    std::filesystem::path previewRoot{"export/blocks-preview"};

    [[nodiscard]] bool operator==(const TestSceneOptions&) const = default;
};

// The directory one export writes into.
//
// SINGLE BLOCK (unchanged since RN-15b, and it must stay unchanged — the stored
// baselines are keyed on it): the block's identifier with `:` replaced by `_`,
// plus one `__<property>-<value>` segment per property the spec named, in the
// order it named them.
//
// `:` is legal in a POSIX path and not on Windows, and this project ships both;
// replacing it is the choice RN-15b records rather than dropping the namespace,
// because a datapack block one day sharing a path with a built-in would otherwise
// overwrite its pictures.
//
// SCENE (C): `scene__<pattern>__<legend>...`, where the pattern is the `--scene`
// string with `/` -> `-`, `;` -> `+` and space -> `.`, and each legend entry is
// `<char>-<identifier>` followed by `.<property>-<value>` per property. The
// PATTERN is part of the name, not just the size: `sg/gs` and `gs/sg` are two
// different pictures and must not share a directory. A name longer than
// `kMaxPreviewDirectoryName` is cut and given an 8-hex FNV-1a suffix of the full
// name, so it stays both short enough for every filesystem and unique.
inline constexpr std::size_t kMaxPreviewDirectoryName = 120;

[[nodiscard]] std::string previewDirectoryName(const TestSceneOptions& options);

// RN-17 追加 --scene <图案> --key <字符>=<方块规格>（可重复）
//   图案：层用 `;` 分隔（第一层在底部，y 向上），层内的行用 `/` 分隔（第一行在北边，
//   z 向南递增），行内每个字符是一格（x 向东递增）。空格 = 空气，其余字符必须在
//   --key 里出现。**不做任何空白裁剪** —— 空格就是空气，裁掉它就没法在边上留空。
//   例：--scene "  s/ss/ggg" --key s=oak_stairs[facing=north] --key g=grass_block
// 命令行形式为 --test-scene <方块规格> [--stage <0..9>]
// 方块规格是 `<数字 id|minecraft:id|裸名>`，可选带 `[属性=值,...]`
// 另有 --occlusion-scene 选受控遮挡场景
// RN-15d 追加 --export-preview [--preview-size N] [--preview-out <目录>]
// 没有 --test-scene 时返回 nullopt
// 参数写错直接抛，免得自动化跑着跑着悄悄渲染了错误的资源还当成功
[[nodiscard]] std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments);

} // namespace mc::render
