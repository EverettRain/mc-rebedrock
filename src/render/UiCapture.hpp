#pragma once

// UI-2：界面截图通道的命令行开关
//
// 全仓在这之前唯一的画面回读是 SceneReadback 的 writeSceneImagePng，而它只有一个调用点
// ——方块预览导出的八机位循环。界面一张图都出不来，于是「与 26.1 1:1」这句话不可验收：
// 既不能与参考图对照，也不能证明下一次改动没有碰坏上一个屏幕。这里补的就是那条入口。
//
// 和 --test-scene 是两件事，因此是两套参数、两条循环：那边拍的是世界里的一个方块，
// 这边停在一个前端页面上拍界面。两者都给了就是对「我在拍什么」有两个答案，直接报错。
//
// ★ 同一个屏幕在不同 GUI scale 下是不同的版面（spec §5 的版面全是逻辑像素上的整数运算，
// 而逻辑画布 = ceil(帧缓冲 / scale)），所以只拍一档等于没拍：默认就拍两档。
//
// 确定性是这条通道的验收条件，不是对它的描述——同一条命令行跑两遍必须逐字节相同。
// 所以输出路径只能是命令行的函数，与时钟、鼠标、options 文件一概无关。

#include "ui/PageStack.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::render {

// 帧缓冲尺寸的上下界。下界是 spec §1.1 的最小逻辑画布 320x240——比它还小的窗口
// 连一档 GUI 缩放都撑不住，拍出来的不是任何一个真实版面。
inline constexpr std::uint32_t kMinUiCaptureWidth = 320U;
inline constexpr std::uint32_t kMinUiCaptureHeight = 240U;
inline constexpr std::uint32_t kMaxUiCaptureExtent = 4096U;

// GUI 缩放的合法档位。0 是 Minecraft 的 "Auto"，其余按 spec §1.1 被 N 夹住。
inline constexpr int kMaxUiCaptureGuiScale = 8;

// 截图期间光标被钉到的那个点（帧缓冲像素）。
//
// 按钮的悬停高亮读的是光标位置，而拍摄时窗口是隐藏的，指针停在哪儿不由我们决定。
// 钉到画布外的一个点，于是**没有任何控件处于悬停态**——这是这个常量存在的全部理由，
// 也是 ui_capture_test 对它断言的那条性质。
//
// 实测记录（别据此以为这个 knob 可以删）：本容器的 Xvfb + 隐藏窗口下，GLFW 读回的
// 光标位置本来就恒定，所以拿掉这行也不会让两遍的图不同。它防的是别的情形——可见
// 窗口、别的平台、或者将来从主循环之后再驱动一次拍摄。
inline constexpr float kUiCaptureCursorX = -1.0F;
inline constexpr float kUiCaptureCursorY = -1.0F;

struct UiCaptureOptions final {
    // 要拍的页面，按命令行给出的顺序，不去重（重复会被解析拒绝）
    std::vector<ui::PageId> pages;
    // 要拍的 GUI 缩放档，0 表示 Auto
    std::vector<int> guiScales{2, 3};
    std::uint32_t width = 1280U;
    std::uint32_t height = 720U;
    std::filesystem::path root{"export/ui-preview"};

    [[nodiscard]] bool operator==(const UiCaptureOptions&) const = default;
};

// 页名与 PageId 的双向映射。名字是命令行拼写，短横线分词。
[[nodiscard]] std::string_view uiCapturePageName(ui::PageId page);
[[nodiscard]] std::optional<ui::PageId> uiCapturePageFromName(std::string_view name);

// 这个页面要不要一个打开着的世界才成立。要的话，这条通道现在拍不了它：
// 世界内容不是命令行的函数，拍出来的图既不可复现也不是任何人想对照的东西。
[[nodiscard]] bool uiCapturePageNeedsWorld(ui::PageId page);

// 一张图的输出路径：<root>/<页名>/scale-<档>.png，Auto 档写作 scale-auto.png。
// 路径必须只由命令行决定——确定性这条规则一直管到文件名。
[[nodiscard]] std::filesystem::path uiCaptureImagePath(const UiCaptureOptions& options,
                                                       ui::PageId page, int guiScale);

// 一次运行应当写出多少张图。少写一张而静默退出 0，是自动化对照最坏的结果。
[[nodiscard]] std::size_t uiCaptureImageCount(const UiCaptureOptions& options);

// 命令行形式：
//   --ui-shot <页名>[,<页名>...]   可重复，累加
//   --ui-scale <档>[,<档>...]      默认 2,3；0 = Auto
//   --ui-size  <宽>x<高>           默认 1280x720
//   --ui-out   <目录>              默认 export/ui-preview
// 没有 --ui-shot 时返回 nullopt。参数写错直接抛，免得自动化跑着跑着悄悄拍了别的屏幕
// 还当成功——这与 parseTestSceneArguments 的理由是同一条。
[[nodiscard]] std::optional<UiCaptureOptions> parseUiCaptureArguments(
    std::span<const std::string_view> arguments);

} // namespace mc::render
