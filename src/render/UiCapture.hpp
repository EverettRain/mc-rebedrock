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

#include "gameplay/ScreenTypes.hpp"
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

// 一次拍摄的**目标**。
//
// ★ 它不是 `PageId`，因为容器界面根本不是一个 PageId：背包、箱子、工作台……都是
//   `PageId::Game` **之上**的 `inventoryOpen + containerScreen` 组合（drawHud 走完
//   `PageDrawKind::InGame` 之后那三条 if）。把目标写成 PageId，容器屏就永远拍不到——
//   这正是 A0-0 之前的状态，也是偏差表 D14 的余项。
//
// 三个字段刻意都是**命令行的函数**：拍什么必须只由命令行决定，这条规矩一直管到文件名。
struct UiCaptureTarget final {
    ui::PageId page = ui::PageId::Title;
    // 有值 = 在游戏内 HUD 之上开着这一块容器界面。
    std::optional<gameplay::ContainerScreen> container{};
    // 创造模式。★ **创造背包不是一个独立的枚举值**，它是
    // `ContainerScreen::PlayerInventory + GameMode::Creative`（HudRenderer 里那三条
    // if 就是这么判的）。所以它在这里是一根**独立的轴**，不是第七块容器屏。
    bool creative = false;
    // 创造背包的哪一个页签。只在 `creative` 为真时有意义。
    //
    // ★ 它必须是一根**自己的轴**：创造背包的绘制分成两支——背包页签（玩家 36 格 +
    //   护甲/副手 + 删除框）与内容页签（9x5=45 格的只读目录 + 页签行 + 滚动条），
    //   两支画的东西几乎没有交集。只给一个"创造"目标，另一支就一张图都没有，
    //   而 A1 要拆的正是这两支。
    bool creativeCatalog = false;

    [[nodiscard]] bool operator==(const UiCaptureTarget&) const = default;
};

struct UiCaptureOptions final {
    // 要拍的目标，按命令行给出的顺序，不去重（重复会被解析拒绝）
    std::vector<UiCaptureTarget> targets;
    // 要拍的 GUI 缩放档，0 表示 Auto
    std::vector<int> guiScales{2, 3};
    std::uint32_t width = 1280U;
    std::uint32_t height = 720U;
    std::filesystem::path root{"export/ui-preview"};

    [[nodiscard]] bool operator==(const UiCaptureOptions&) const = default;
};

// 目标名与目标的双向映射。名字是命令行拼写，短横线分词。
//
// 一张表两个方向读，于是名字与目标不可能各说各话；表的覆盖性由两条 constexpr 断言
// 钉住（每个 PageId 一个目标、每块 ContainerScreen 至少一个目标），**用 `Count`
// 哨兵而不是"当时的最后一个枚举值"**——后者在追加时静默通过（README 护栏 25）。
[[nodiscard]] std::string_view uiCaptureTargetName(const UiCaptureTarget& target);
[[nodiscard]] std::optional<UiCaptureTarget> uiCaptureTargetFromName(std::string_view name);

// 这个页面要不要一个打开着的世界才成立。
//
// UI-6-0 之前这同时是一条**拒绝**规则：解析期就把这四页挡掉，理由是"世界内容不是
// 命令行的函数"。那句话对的是*真实*世界——随机种子、异步区块流送、每帧推进的模拟。
// 现在它只是一个提问：渲染器据此为这一页打开那份**固定的世界夹具**（`--test-scene`
// 已经在用的单方块场景：固定方块、固定光照、固定日时、不起模拟线程），拍完再关。
//
// 关掉夹具靠的是 `worldReady`，不是拆掉世界：无世界的页面会整屏铺全景，
// 把世界画面盖掉（全景是一次全屏三角形）。这也是为什么这两类页面能在**同一次运行**
// 里混着拍——UI-5 的十屏基线因此不受影响。
[[nodiscard]] bool uiCapturePageNeedsWorld(ui::PageId page);

// 这一页画的时候游戏是不是暂停的。
// 只有游戏内 HUD 不是：其余三页都是盖在世界上的界面，而 `paused` 决定 drawHud 走哪条分支。
[[nodiscard]] bool uiCapturePageIsPaused(ui::PageId page);

// 这一页画的时候**世界画面看得见**吗（渲染器的 `worldReady`）。
//
// 它与 uiCapturePageNeedsWorld **不是同一个问题**，loading 就是那个差集：它属于世界
// 会话（要夹具在场），但画的时候还没有世界画面——`drawHud` 的 `!worldReady` 分支画的
// 是全景加一行进度。给它 worldReady = true 会直接跳过那一屏，`loading/scale-N.png` 里
// 拍到的是游戏内 HUD：一张完全正确的 HUD，只是文件名写着 loading。
//
// 抽成纯函数是因为这条判断原本长在渲染器的翻译单元里，而那里没有任何测试看得见——
// 「如果一处改动不改变任何函数的返回值，它就还没有被任何断言覆盖」（护栏 12）。
[[nodiscard]] bool uiCapturePageShowsWorld(ui::PageId page);

// 一张图的输出路径：<root>/<目标名>/scale-<档>.png，Auto 档写作 scale-auto.png。
// 路径必须只由命令行决定——确定性这条规则一直管到文件名。
[[nodiscard]] std::filesystem::path uiCaptureImagePath(const UiCaptureOptions& options,
                                                       const UiCaptureTarget& target,
                                                       int guiScale);

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
