#pragma once

#include "gameplay/Difficulty.hpp"
#include "gameplay/GameMode.hpp"
#include "persistence/SaveRepository.hpp"
#include "ui/Language.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextField.hpp"
#include "ui/WidgetId.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

// Java 的 String.hashCode，逐单元复刻
//
// 为什么要连"按 UTF-16 码元"这一条也照抄：种子字符串落进的是**世界生成**，
// 同一个名字在 Java 版与本作里必须长出同一张地图，否则 JC 那条格式桥上
// "同名种子同世界"这句话就不成立了。按 UTF-8 字节哈希对纯 ASCII 恰好一致，
// 而对中文名字会静默给出另一个世界——这正是最难被发现的那类偏差。
// 代理对按 Java 的规矩拆成两个码元参与哈希。
//
// 溢出走无符号回绕，那是 Java int 乘加的定义；末尾那次转换在 C++20 里是模运算，
// 与 Java 的 int 位型一致。
[[nodiscard]] constexpr std::int32_t javaStringHash(std::string_view text) {
    std::uint32_t hash = 0U;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint = lead;
        std::size_t continuations = 0U;
        if (lead >= 0xF0U) {
            codepoint = lead & 0x07U;
            continuations = 3U;
        } else if (lead >= 0xE0U) {
            codepoint = lead & 0x0FU;
            continuations = 2U;
        } else if (lead >= 0xC0U) {
            codepoint = lead & 0x1FU;
            continuations = 1U;
        }
        // 截断或不合法的序列按单字节处理：这里的输入来自输入框，不保证是完整的 UTF-8
        if (index + continuations >= text.size()) {
            continuations = 0U;
            codepoint = lead;
        }
        for (std::size_t offset = 1U; offset <= continuations; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                continuations = 0U;
                codepoint = lead;
                break;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        index += continuations + 1U;
        if (codepoint > 0xFFFFU) {
            const std::uint32_t surrogate = codepoint - 0x10000U;
            hash = hash * 31U + (0xD800U + (surrogate >> 10U));
            hash = hash * 31U + (0xDC00U + (surrogate & 0x3FFU));
        } else {
            hash = hash * 31U + codepoint;
        }
    }
    return static_cast<std::int32_t>(hash);
}

// 创建世界那个种子输入框里的字符串是什么意思，规则同 26.1 的
// `WorldOptions.parseSeed`：先 trim，空串表示"随机"（返回 nullopt，由调用方掷一个），
// 能被 Long.parseLong 吃下的当十进制整数直接用，其余取字符串哈希。
//
// 返回 nullopt 而不是"就地掷一个随机数"，是为了让这一层保持纯函数：随机源属于
// 调用方，测试才能把三条分支都钉死。
[[nodiscard]] inline std::optional<std::uint64_t> parseWorldSeed(std::string_view text) {
    // Java 的 String.trim 砍的是所有 <= ' ' 的字符，不是 locale 的 isspace
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    // from_chars 不认前导 '+'，而 Long.parseLong 认，所以先自己剥掉一个
    std::string_view body = text;
    if (body.front() == '+') {
        body.remove_prefix(1);
    }
    std::int64_t value = 0;
    const char* const last = body.data() + body.size();
    const auto parsed = std::from_chars(body.data(), last, value);
    // 必须**整串**被吃掉：Long.parseLong("12x") 是抛异常，不是解析出 12
    if (parsed.ec == std::errc{} && parsed.ptr == last) {
        return static_cast<std::uint64_t>(value);
    }
    // 越界的数字串（"99999999999999999999"）走的也是这条：Java 那里同样是
    // NumberFormatException，于是也取哈希
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(javaStringHash(text)));
}

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
    // 种子输入框。与名称框共用 kWorldNameFieldRules：26.1 的两个框都是默认的
    // EditBox（`EditBox.maxLength = 32`），种子框没有调用 setMaxLength
    TextFieldState createWorldSeed;
    // 创建页上有两个输入框，键盘归谁——名称框还是种子框
    // 焦点是**屏幕状态**（同 focusedWidget 那条注释），页面重建不该把它冲掉
    bool createWorldSeedFocused = false;
    gameplay::GameMode createWorldGameMode = gameplay::GameMode::Survival;
    // 新世界的难度。存档字段一直都在，加载时也一直会 setDifficulty，
    // 从前只是没有任何一处前端能选它，于是恒为 Normal
    gameplay::Difficulty createWorldDifficulty = gameplay::Difficulty::Normal;
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
    // 按键设置列表的滚动偏移与滚动条拖拽状态
    std::size_t controlsListFirstIndex = 0U;
    // 正在拖某一条滚动条。**一屏最多一张滚动列表**，所以拖的是哪一条由当前页面决定——
    // 不必每张列表一个 bool。
    //
    // ★ 从前是 `languageScrollbarDragging` + `controlsScrollbarDragging` 两个，
    //   而后者是个**只被写成 false、从没被读过**的死字段：按键绑定那张列表的滚动条
    //   根本拖不动（偏差 D18），设置列表更是连字段都没有。加一张列表要再加一个 bool、
    //   再抄一遍按下/拖动/松开三处——这与滑块那次"三个 xxxSliderDragging"是同一族。
    bool scrollbarDragging = false;
    // UI-6d：三段式设置页（视频设置 / 控制 / 高级图形）那张 OptionsList 的滚动偏移。
    //
    // ★ 三页共用一个字段，与 26.1 一致：那三屏是**三个屏幕对象**，进哪一屏都是新建
    //   一个 OptionsList、滚动位置从 0 起。共用一个字段而在换页时归零（`resetPageState`）
    //   就是这个语义；每页各存一个反而会"退出去再进来还停在半截"。
    std::size_t optionsListFirstIndex = 0U;
    // UI-6e ③：资源包右栏当前选中的行（调序按钮作用于它）。npos = 没选中。
    std::size_t selectedPackRow = static_cast<std::size_t>(-1);
    // 提交过一次选择之后置位：界面据此显示"重启后生效"。
    // ★ 换包**不做热重载**（依据见 known-debt），所以这句提示是必需的——
    //   没有它，玩家会以为开关没生效。
    bool packRestartRequired = false;
    bool optionsOpen = false;
    // 正在拖的那个滑块，None 表示没有在拖。
    //
    // ★ 从前这里是**三个 bool**，一个滑块一个（渲染距离 / 模拟距离 / 主音量），
    //   而拖拽分派、松开清理、绘制高亮各写一遍那三个名字。加第四个滑块要动那三处，
    //   漏一处的症状是"滑块画得出来但拖不动"——UI-6d 的模糊强度滑块正是这么坏的。
    //   三个 bool 是同一个事实（"现在在拖谁"）的三份表述，收成一个 id 之后，
    //   加滑块不再需要碰这里的任何东西。
    WidgetId draggingSlider = WidgetId::None;
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

// 创建世界表单 -> GameRuntime::createWorld 的那五个实参。
//
// 为什么单独立一个纯函数而不是在渲染器里直接展开：那一行是**唯一**把表单读进后端的
// 地方，而它从前正是出错的地方——种子参数早就在，前端却在调用点就地造了一个时钟种子
// 把它盖掉，难度则被 GameRuntime 写死成常量。这两处失误都长成"调用点少传/传错了一个
// 参数"，而渲染器里的代码无头测试碰不到。收成这个函数之后它们就都在断言覆盖之下了。
//
// randomSeed 由调用方掷：随机源属于渲染器，解析层保持可测的纯函数。
struct NewWorldRequest final {
    std::string name;
    std::uint64_t seed = 0U;
    gameplay::GameMode mode = gameplay::GameMode::Survival;
    bool allowCommands = false;
    gameplay::Difficulty difficulty = gameplay::Difficulty::Normal;
};

[[nodiscard]] inline NewWorldRequest newWorldRequest(const MenuSystem& menu,
                                                     std::uint64_t randomSeed) {
    return NewWorldRequest{
        menu.createWorldName.value,
        parseWorldSeed(menu.createWorldSeed.value).value_or(randomSeed),
        menu.createWorldGameMode,
        menu.createWorldAllowCommands,
        menu.createWorldDifficulty,
    };
}

} // namespace mc::ui
