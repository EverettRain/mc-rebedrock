#pragma once

#include "gameplay/GameMode.hpp"
#include "persistence/SaveRepository.hpp"
#include "ui/Language.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextField.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::ui {

// 在 [0, Count) 这段下标上与 core::CreativeCategory 一一对应
// 页签下标因此能直接强转成它的内容分类，见 activeCreativeCatalog
// 末尾额外多一个"背包"伪页签，那是生存背包视图，不是内容分类
// 两个枚举保持同序，这次强转才是可靠的
enum class CreativeTab : std::uint8_t {
    BuildingBlocks,
    ColoredBlocks,
    NaturalBlocks,
    Functional,
    Redstone,
    Tools,
    Combat,
    FoodAndDrink,
    Ingredients,
    SpawnEggs,
    Inventory,
};

struct DisplayResolution final {
    int width = 0;
    int height = 0;
};

// "分辨率"按钮循环的窗口尺寸表
// vanilla 列的是显示器的全屏显示模式
// 本项目渲染进一个可缩放窗口，所以这里给的是一组常见窗口尺寸（从 4:3 笔记本到 16:9 桌面屏）
// 窗口本身仍可任意拖拽或最大化，这张表只驱动菜单里的循环
inline constexpr std::array<DisplayResolution, 16> kDisplayResolutions{{
    {800, 600},
    {960, 540},
    {960, 720},
    {1024, 768},
    {1152, 864},
    {1280, 720},
    {1280, 800},
    {1366, 768},
    {1440, 900},
    {1536, 864},
    {1600, 900},
    {1680, 1050},
    {1920, 1080},
    {2048, 1152},
    {2560, 1440},
    {3840, 2160},
}};

// 前端菜单状态的所有者，含页栈、世界列表、语言列表和选项界面的各项选择
// 渲染器通过这些字段驱动它，把 Vulkan、GLFW、音频那套管线留在自己那边
// 要碰渲染器的那部分菜单逻辑仍住在渲染器的处理函数里，它们读写这里的状态
// 那类逻辑包括存档读写、光标捕获和音频
class MenuSystem final {
  public:
    ui::PageStack pageStack;
    std::vector<persistence::SaveSummary> saveSummaries;
    std::size_t selectedWorldIndex = 0U;
    std::size_t worldListFirstIndex = 0U;
    // UI-1: 名称输入框的完整状态（值 + 光标 + 选区 + 横向滚动），不再是一个裸串
    TextFieldState createWorldName =
        textFieldWithValue("New World", kWorldNameFieldRules, TextFieldMetrics{});
    gameplay::GameMode createWorldGameMode = gameplay::GameMode::Survival;
    // 正在创建的世界是否允许作弊
    // vanilla 在创建界面上默认关闭，由玩家在创建前自行打开
    bool createWorldAllowCommands = false;
    TextFieldState editWorldName;
    std::string editWorldIdentifier;
    std::string saveStatus;
    std::vector<std::string> languageCodes{std::string{kDefaultLanguageCode}};
    std::vector<std::string> languageDisplayNames;
    // 点击某一行只改这个草稿选择
    // 与 26.1 一致，昂贵的资源重载由 Done 提交，而不是浏览时每点一下就来一次
    std::string pendingLanguageCode{kDefaultLanguageCode};
    std::string languageStatus;
    std::size_t languageListFirstIndex = 0U;
    bool languageScrollbarDragging = false;
    // 按键设置列表的滚动偏移与滚动条拖拽状态
    std::size_t controlsListFirstIndex = 0U;
    bool controlsScrollbarDragging = false;
    // UI-6d：三段式设置页（视频设置 / 控制 / 高级图形）那张 OptionsList 的滚动偏移。
    //
    // ★ 三页共用一个字段，与 26.1 一致：那三屏是**三个屏幕对象**，进哪一屏都是新建
    //   一个 OptionsList、滚动位置从 0 起。共用一个字段而在换页时归零（`resetPageState`）
    //   就是这个语义；每页各存一个反而会"退出去再进来还停在半截"。
    std::size_t optionsListFirstIndex = 0U;
    bool optionsOpen = false;
    bool viewDistanceSliderDragging = false;
    bool simulationDistanceSliderDragging = false;
    bool masterVolumeSliderDragging = false;
    // UI-4 / GUI spec §1.4：键盘焦点的控件下标，npos 表示没有焦点。
    // 焦点是**屏幕状态**不是控件状态，所以住在这里而不是 Widget 里——页面每帧重建，
    // 把焦点存进控件会在重建时丢掉。
    //
    // 焦点跟着页面走：换页即失效。只记下标而不记页面，翻页后那个下标会指向新页面上
    // 完全不相干的一个控件，而且**不会有任何东西报错**——按 Enter 就触发了别的动作。
    std::size_t focusedWidget = static_cast<std::size_t>(-1);
    PageId focusedPage = PageId::Title;

    [[nodiscard]] std::size_t focusFor(PageId page) const {
        return page == focusedPage ? focusedWidget : static_cast<std::size_t>(-1);
    }
    void setFocus(PageId page, std::size_t index) {
        focusedPage = page;
        focusedWidget = index;
    }
    // UI-4：主菜单的 splash。候选行在启动时从资源包读一次；`splashLine` 是这一次进入
    // 标题屏抽中的那行——每次回到标题屏重抽，与 vanilla 一致。
    std::vector<std::string> splashLines;
    std::string splashLine;
    int guiScaleSetting = 0;
    // UI-3：强制 Unicode 字体也参与 GUI 缩放的求解——26.1 `Window.calculateScale` 在
    // 打开它时把档位抬到偶数（unicode 字形按半尺寸绘制，奇数档会让半像素落不到整数纹素上）。
    // 与 guiScaleSetting 一样，是 options 的镜像，供每一处 HudLayout 构造读取。
    bool forceUnicodeFont = false;
    std::size_t resolutionIndex = 0;
    CreativeTab creativeTab = CreativeTab::BuildingBlocks;
    std::size_t creativeScrollRow = 0;
};

} // namespace mc::ui
