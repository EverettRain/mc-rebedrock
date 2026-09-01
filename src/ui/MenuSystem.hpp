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
    bool optionsOpen = false;
    bool viewDistanceSliderDragging = false;
    bool simulationDistanceSliderDragging = false;
    bool masterVolumeSliderDragging = false;
    int guiScaleSetting = 0;
    std::size_t resolutionIndex = 0;
    CreativeTab creativeTab = CreativeTab::BuildingBlocks;
    std::size_t creativeScrollRow = 0;
};

} // namespace mc::ui
