#pragma once

// 菜单页面唯一的装配处
// buildPage(PageId, ctx, cb, rect) 返回该页面的 ui::Page，也就是一个 vector<Widget>
// 其中每个控件的 kind、标签、是否可用、debugId 与回调都已接好
// 它同时取代了每页一份的 constexpr MenuButton 数组与那个 switch 派发
// 控件的顺序就是布局的顺序，而每个控件自己拥有从前由 switch 执行的那个动作
//
// 不碰 Vulkan 且可测试，渲染器提供三样东西：
//   - MenuCallbacks：每个动作都是一个 std::function，可捕获 Vulkan、存档与音频
//   - RectProvider：由下标取 UiRect，来自渲染器的 HudLayout
//   - MenuBuildContext：页面形状所依赖的那些只读标志
// 无头测试因此能用桩回调加一套平凡的行布局搭出页面，再断言点击第 N 个控件触发第 N 个回调
// ui 命名空间从不接触 Vulkan

#include "ui/OptionSlider.hpp"
#include "ui/OptionsList.hpp"
#include "ui/PageStack.hpp"
#include "ui/WidgetId.hpp"
#include "input/InputAction.hpp"
#include "input/InputNaming.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace mc::ui {

// 由下标取控件的屏幕矩形，来自调用方的布局
// 装配器按控件在页面上的序号来要矩形，这与 frontendButtonRect 既有的下标约定一致
using RectProvider = std::function<UiRect(std::size_t index)>;

// 页面形状所依赖的那些只读事实
// 保持很小且只含值，测试因此能直接把它们设好
struct MenuBuildContext final {
    bool worldOpen = false;  // a save is loaded: Options gains Difficulty, etc.
    // 至少存在一个存档，世界列表的进入与编辑按钮只有这时才可用
    // 存档列表为空时它们变灰，且点击不得触发
    // 绘制与派发读的是同一个标志，被禁用的按钮因此既不会画成可用的样子，也不会被激活
    bool worldSelectable = false;
    // 装配器盖在选项按钮上的标签文本，渲染器已经完成本地化与取值格式化
    // 测试里给空串也没问题
    std::function<std::string(std::uint16_t debugId)> labelFor{};
    // 世界列表与语言列表这两个滚动列表的行数
    // 渲染器在装配之前已经把滚动偏移与可见窗口折算进这两个值
    std::size_t worldRowCount = 0;
    std::size_t languageRowCount = 0;
    // 某个动作那一行的**两段**文字：左边的动作名与右边按钮上的键名。
    //
    // ★ 一个回调返回两个字段，而不是两个回调各返回一段。
    //   UI-6b 把这一行拆成两个控件之后曾经是两个回调——而填这份上下文的地方有两处
    //   （渲染器的 `buildCurrentPage` 与 HudRenderer 常驻的 `drawContext_`），
    //   只填了一处的那个字段就静默回落到英文兜底：界面切成中文以后按键设置里
    //   满屏还是 "Forward / Back / Jump"，而标题和底部按钮都已经是中文。
    //   做成一个返回值的两个字段，"漏填一个"这件事在类型上就不成立。
    //
    // 渲染器从 InputSystem 这个唯一来源构造它，测试可以打桩。
    // 设置了它，按键设置页就渲染绑定表而不是早先那套开关脚手架。
    struct KeyBindRowLabels final {
        std::string action;  // 左边：已本地化的动作名（`key.forward` 等）
        std::string key;     // 右边：按钮上的键名，含冲突/捕获中的装饰
        // 这一行的重置按钮能不能按：`resetButton.active = !this.key.isDefault()`
        // （`KeyBindsList.java:158`）。已经是默认绑定时它是灰的。
        // 放进同一个返回值而不是再开一个回调——理由同上面那段。
        bool resettable = false;
    };
    std::function<KeyBindRowLabels(input::InputAction action)> keyBindLabelsFor{};
    // UI-6d：设置项列表的滚动窗口（以**行**为单位）。rowCount 为 0 表示"不滚，全装配"
    // ——项数装得下的页面（Controls / 高级图形）走这一档。
    OptionsWindow optionsWindow{};
    // 按键设置页的绑定列表是滚动的，只装配可见窗口，与世界列表和语言列表一样
    // 无论有多少个动作，控件数量因此都有界
    // keyBindFirstIndex 是在 input::keyBindRows() 中的滚动偏移，keyBindRowCount 是可见窗口的大小
    std::size_t keyBindFirstIndex = 0;
    std::size_t keyBindRowCount = 0;
    // UI-4：这一次点击是否按着 Shift。循环选项按钮据此反向步进（spec §2.3）。
    // 放在上下文里而不是回调签名里，是因为"按着 Shift 吗"是**装配这一页时的事实**，
    // 与 worldOpen / worldSelectable 同类；改签名会波及每一个 cycleOption 的调用点。
    bool reverseCycle = false;
};

// 页面能触发的每一个菜单动作，以可注入的回调形式给出
// 渲染器用它自己的成员函数填这些字段，比如 setPaused、startWorld、cycleResolution
// 留空的那些在其控件被点击时就是空操作，测试可以只填自己要用的几个
struct MenuCallbacks final {
    // 标题页与世界流程
    std::function<void()> openSingleplayer{};
    std::function<void()> exitGame{};
    std::function<void()> playSelectedWorld{};
    std::function<void()> createWorld{};
    std::function<void()> editWorld{};
    std::function<void()> confirmCreate{};
    std::function<void()> toggleCreateGameMode{};
    // 创建页那个难度循环按钮。与世界内选项页的 cycleDifficulty 是**两个**回调：
    // 它们步进的是两个不同的东西（创建表单的暂存值 / 已打开存档的字段），
    // 共用一个回调就得在里面按当前页面分叉，那是把页面的事塞进动作里
    std::function<void()> cycleCreateDifficulty{};
    std::function<void()> toggleCreateAllowCommands{};
    std::function<void()> renameWorld{};
    std::function<void()> deleteWorld{};
    std::function<void()> confirmDelete{};
    std::function<void()> cancelDelete{};
    std::function<void(std::size_t rowIndex)> selectWorldRow{};

    // 暂停与死亡界面
    std::function<void()> resume{};
    std::function<void()> saveAndQuit{};
    std::function<void()> respawn{};
    std::function<void()> returnToTitle{};

    // 选项页导航
    std::function<void()> openOptions{};
    std::function<void()> openVideoSettings{};
    std::function<void()> openControls{};
    std::function<void()> openLanguage{};
    std::function<void()> openExperimental{};
    // UI-6c：Controls 枢纽上跳到绑定列表的那个按钮（26.1 §7.6 → §7.8），
    // 以及 Options 上跳到辅助功能设置的那个（§7.11）。
    // UI-6c：只重置一个动作的绑定（每行那个按钮）。整表重置仍是 resetKeyBinds。
    std::function<void(input::InputAction)> resetKeyBind{};
    std::function<void()> openKeyBinds{};
    std::function<void()> openAccessibility{};
    std::function<void()> openAdvancedGraphics{};
    std::function<void()> doneOptions{};   // pop the current options sub-page
    std::function<void()> back{};          // generic page pop

    // 视频与玩法的开关和循环选项
    std::function<void()> cycleResolution{};
    std::function<void()> cycleGuiScale{};
    // 每个在固定取值列表上步进的选项都走这一个回调，以控件 id 为键
    // 取值、字段与标签全都来自 ui::OptionCycle 的表
    // 新增一个选项因此只是一行表数据加一行放置它的 addOptionButton，绝不需要在这里再加一个回调
    // direction 取 +1 表示下一个值、-1 表示上一个值，双向选择控件因此不需要任何新的接线
    std::function<void(WidgetId id, int direction)> cycleOption{};
    std::function<void()> cycleDifficulty{};

    // 实验性内容页

    // 语言列表的行选中，这只是暂选，按下完成才提交
    std::function<void(std::size_t rowIndex)> selectLanguageRow{};

    // 点击某个动作所在的行会开始捕获它的下一个按键，走 KeyBindingScreen::beginCapture
    // 重置则恢复 vanilla 默认值
    // 两者都经渲染器的闭包作用在 InputSystem 这个唯一来源上
    std::function<void(input::InputAction action)> beginKeyCapture{};
    std::function<void()> resetKeyBinds{};

    // 滑块：取值函数，以及拖拽与提交时施加取值的函数，比例取 [0,1]
    SliderBind viewDistance{};
    SliderBind simulationDistance{};
    SliderBind masterVolume{};
    // UI-6d：按 id 造一个整数滑块的绑定（表在 ui/OptionSlider.hpp）。
    // 一个回调服务表里所有的滑块——加一个滑块不该再多一个成员。
    std::function<SliderBind(WidgetId)> intSliderFor{};
};

namespace detail {

[[nodiscard]] inline std::string label(const MenuBuildContext& ctx, WidgetId id) {
    return ctx.labelFor ? ctx.labelFor(static_cast<std::uint16_t>(id)) : std::string{};
}

// 追加一个普通按钮控件，它的矩形取自提供器给出的下一个序号
inline void addButton(Page& page, const MenuBuildContext& ctx,
                      WidgetId id, std::function<void()> onActivate, bool enabled = true) {
    Widget w;
    w.kind = WidgetKind::Button;
    w.debugId = static_cast<std::uint16_t>(id);
    w.label = label(ctx, id);
    w.enabled = enabled;
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

// UI-4：只有图标没有文字的方钮。除了 kind 之外与 addButton 完全一样——
// 图标本身由绘制侧按 id 查表取，因为图标是**资源**，而这一层从不接触资源。
inline void addIconButton(Page& page, WidgetId id,
                          std::function<void()> onActivate, bool enabled = true) {
    Widget w;
    w.kind = WidgetKind::IconButton;
    w.debugId = static_cast<std::uint16_t>(id);
    w.enabled = enabled;
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

// 一个循环选项的按钮
// 它的动作永远是那同一个通用步进，以 id 为键
// 取值、字段与标签都归表管，见 ui/OptionCycle.hpp，绝不归这个调用点管
inline void addOptionButton(Page& page, const MenuBuildContext& ctx,
                            WidgetId id, const MenuCallbacks& cb) {
    // 回调被拷贝进控件，与其它每个动作一样
    // 因为 Page 的存活期长于 buildPage 收到的那个 MenuCallbacks 引用
    //
    // UI-4 / GUI spec §2.3：**Shift+点击反向循环**（1.17 起）。方向从上下文的
    // `reverseCycle` 读——那是"这一次点击按着 Shift 吗"，由渲染器在装配前填好。
    // 方向早就是 cycleOption 的参数，此前只是从没传过 -1。
    addButton(page, ctx, id, [cycle = cb.cycleOption, id, reverse = ctx.reverseCycle] {
        if (cycle) {
            cycle(id, reverse ? -1 : 1);
        }
    });
}

inline void addSlider(Page& page, const MenuBuildContext& ctx,
                      WidgetId id, SliderBind bind) {
    Widget w;
    w.kind = WidgetKind::Slider;
    w.debugId = static_cast<std::uint16_t>(id);
    w.label = label(ctx, id);
    w.slider = std::move(bind);
    page.push_back(std::move(w));
}

// UI-6d：一个由 ui/OptionSlider.hpp 那张表驱动的整数滑块。
//
// 与 addSlider 的区别是**取值从哪来**：那个收一个调用方现造的 SliderBind（三个既有滑块
// 各自硬编码），这个按 id 查表、由回调统一造。加一个整数滑块因此只改表一行，
// 而不是"回调加一个成员 + 渲染器填三个 lambda + widgetLabel 加一个 case"。
// 这一页的第 `optionIndex` 个设置项在不在可见窗口里。
//
// rowCount == 0 是"不滚"：装得下的页面不必给窗口，也就不必在每个装配点写条件。
[[nodiscard]] inline bool optionVisible(const MenuBuildContext& ctx, PageId page,
                                        std::size_t optionIndex) {
    if (ctx.optionsWindow.rowCount == 0U) {
        return true;
    }
    return ctx.optionsWindow.contains(
        optionsGroupedSlot(optionsGroupsOf(page), optionIndex).row);
}

// 三段式设置页的装配游标：按**设置项**序号推进，只发射落在滚动窗口里的那些。
//
// ★ 三个三段式页面都必须用它，包括当前装得下的那两个。`optionsWindowFor` 给所有
//   HeaderFooterList 页面同一种窗口，而布局侧（`optionsScrolledSlot`）按 firstRow
//   跳过滚上去的项来解释控件序号。哪一页装配了窗口外的项，两侧对序号的含义就不一致，
//   那一页的控件会**整体错行**——不是"多画了几行"，是全都画在别的行上。
//   窗口大到装得下时它是恒真的，代价为零。
class OptionCursor final {
public:
    OptionCursor(const MenuBuildContext& ctx, PageId page) : ctx_(&ctx), page_(page) {}

    template <typename EmitFn>
    void operator()(EmitFn&& emit) {
        if (optionVisible(*ctx_, page_, index_)) {
            emit();
        }
        ++index_;
    }

private:
    const MenuBuildContext* ctx_;
    PageId page_;
    std::size_t index_ = 0;
};

inline void addIntSlider(Page& page, const MenuBuildContext& ctx, const MenuCallbacks& cb,
                         WidgetId id) {
    Widget w;
    w.kind = WidgetKind::Slider;
    w.debugId = static_cast<std::uint16_t>(id);
    w.label = label(ctx, id);
    if (cb.intSliderFor) {
        w.slider = cb.intSliderFor(id);
    }
    page.push_back(std::move(w));
}

inline void addListRow(Page& page, WidgetId id, std::size_t rowIndex,
                       std::function<void()> onActivate) {
    Widget w;
    w.kind = WidgetKind::ListRow;
    w.debugId = static_cast<std::uint16_t>(id);
    w.onActivate = std::move(onActivate);
    static_cast<void>(rowIndex);
    page.push_back(std::move(w));
}

// UI-6b：一个按键绑定行 = **两个**控件，不是一个。
//
// 26.1 的 `KeyBindsList.KeyEntry` 是一段动作名加两个按钮（改键 / 重置），
// `children()` 把它们一起交出去，键盘焦点在行内走。本作从前把整行做成**一个**
// `ListRow`：一块底衬加一行 `"动作: 按键"` 文本、整行一次点击。
//
// 这里先补前两样——名称 Label 与改键 Button。**重置按钮留给 UI-6c**：它需要
// 「这个动作的默认绑定是什么」，而 input 层今天只有整表重置（`resetToDefaults`），
// 没有 `defaultBinding(action)`。先摆一个按不动的按钮不如不摆。
//
// 三个 Widget 的矩形由**布局那一趟**填（ui::layoutPageInto）：装配只管"有哪些控件"。
inline void addKeyBindRow(Page& page, const MenuBuildContext& ctx,
                          input::InputAction action, std::function<void()> onActivate,
                          std::function<void()> onReset) {
    // 两段文字一次取出：漏填其中一段在类型上就不成立（见 KeyBindRowLabels）。
    MenuBuildContext::KeyBindRowLabels labels;
    if (ctx.keyBindLabelsFor) {
        labels = ctx.keyBindLabelsFor(action);
    } else {
        labels.action = std::string{input::actionDisplayName(action)};
    }

    Widget name;
    name.kind = WidgetKind::Label;
    name.debugId = static_cast<std::uint16_t>(WidgetId::KeyBindRow);
    name.label = std::move(labels.action);
    // Label 不可交互：焦点遍历跳过它，点它也不会开始捕获。
    name.enabled = false;
    page.push_back(std::move(name));

    Widget change;
    change.kind = WidgetKind::Button;
    change.debugId = static_cast<std::uint16_t>(WidgetId::KeyBindRow);
    // 按钮上写的是**键名**，不是"动作: 按键"。装饰（冲突的 `[ … ]`、捕获中的 `> … <`）
    // 与动作名一起由 keyBindLabelsFor 给出——它读的是 InputSystem 这个唯一来源。
    change.label = std::move(labels.key);
    change.onActivate = std::move(onActivate);
    page.push_back(std::move(change));

    // UI-6c：这一行自己的重置按钮（`controls.reset`），只重置**这一个**绑定。
    // 已经是默认绑定时它是灰的——26.1 的 `resetButton.active = !key.isDefault()`。
    Widget reset;
    reset.kind = WidgetKind::Button;
    reset.debugId = static_cast<std::uint16_t>(WidgetId::ResetKeyBind);
    reset.label = ctx.labelFor ? ctx.labelFor(static_cast<std::uint16_t>(WidgetId::ResetKeyBind))
                               : std::string{};
    reset.enabled = labels.resettable;
    reset.onActivate = std::move(onReset);
    page.push_back(std::move(reset));
}

}  // namespace detail

// 把 id 对应的页面装配进调用方给的缓冲
// 控件顺序与从前每页一份的数组一致，每个回调执行的正是旧 switch 分支所做的事
// page 会先被清空，容量因此跨次复用
// 绘制侧每帧都要一份当前页，走这个重载就不必每帧向堆要一个新的 vector
inline void buildPageInto(Page& page, PageId id, const MenuBuildContext& ctx,
                          const MenuCallbacks& cb) {
    using detail::addButton;
    using detail::addIconButton;
    using detail::addOptionButton;
    using detail::addListRow;
    using detail::addIntSlider;
    using detail::addSlider;
    page.clear();

    switch (id) {
        case PageId::Title:
            // UI-2：顺序即版面（TitleScreenLayout 的 titleWidgetRect 用同一个序号约定）。
            // 26.1 的 TitleScreen.init 按 单人 / 多人 / Realms / 语言 / 选项 / 退出 / 无障碍
            // 这个次序装配，本页照抄。
            //
            // 多人、Realms、无障碍三个的目标屏幕本作还没有（ServerList / RealmsMain /
            // AccessibilitySettings），所以它们**在位、灰着、点不动**——vanilla 在
            // allowsMultiplayer() 为假时正是这么灰掉多人与 Realms 的，不是自造形态。
            // 目标屏幕登记在 UI-7。
            addButton(page, ctx, WidgetId::Singleplayer, cb.openSingleplayer);
            addButton(page, ctx, WidgetId::Multiplayer, nullptr, /*enabled=*/false);
            addButton(page, ctx, WidgetId::Realms, nullptr, /*enabled=*/false);
            addIconButton(page, WidgetId::TitleLanguage, cb.openLanguage);
            addButton(page, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, ctx, WidgetId::Exit, cb.exitGame);
            addIconButton(page, WidgetId::TitleAccessibility, nullptr,
                          /*enabled=*/false);
            break;

        case PageId::Pause:
            addButton(page, ctx, WidgetId::Resume, cb.resume);
            addButton(page, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, ctx, WidgetId::SaveQuit, cb.saveAndQuit);
            break;

        case PageId::Death:
            addButton(page, ctx, WidgetId::Respawn, cb.respawn);
            addButton(page, ctx, WidgetId::TitleScreen, cb.returnToTitle);
            break;

        case PageId::WorldList:
            // 先是滚动的存档行，也就是列表主体，然后是四个动作按钮
            // 按钮顺序沿用惯例：进入、创建、编辑、返回
            for (std::size_t row = 0; row < ctx.worldRowCount; ++row) {
                addListRow(page, WidgetId::WorldRow, row,
                           [cb, row]() { if (cb.selectWorldRow) cb.selectWorldRow(row); });
            }
            addButton(page, ctx, WidgetId::PlaySelected, cb.playSelectedWorld,
                      ctx.worldSelectable);
            addButton(page, ctx, WidgetId::CreateWorld, cb.createWorld);
            addButton(page, ctx, WidgetId::Edit, cb.editWorld, ctx.worldSelectable);
            addButton(page, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::CreateWorld:
            // 顺序照 26.1 的 CreateWorldScreen.GameTab：名称框、游戏模式、难度、允许作弊
            addButton(page, ctx, WidgetId::CreateGameMode, cb.toggleCreateGameMode);
            // ★ 复用 WidgetId::Difficulty，不新开一个 id：标签"难度: 普通"那段算法
            //   世界内选项页已经有了，另起一个 id 就得再抄一份，两份迟早分岔
            addButton(page, ctx, WidgetId::Difficulty, cb.cycleCreateDifficulty);
            addButton(page, ctx, WidgetId::CreateAllowCommands,
                      cb.toggleCreateAllowCommands);
            addButton(page, ctx, WidgetId::CreateConfirm, cb.confirmCreate);
            addButton(page, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::EditWorld:
            addButton(page, ctx, WidgetId::SaveRename, cb.renameWorld);
            addButton(page, ctx, WidgetId::DeleteWorld, cb.deleteWorld);
            addButton(page, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::ConfirmDelete:
            addButton(page, ctx, WidgetId::DeleteConfirm, cb.confirmDelete);
            addButton(page, ctx, WidgetId::DeleteCancel, cb.cancelDelete);
            break;

        case PageId::Options:
            addSlider(page, ctx, WidgetId::MasterVolume, cb.masterVolume);
            if (ctx.worldOpen) {
                addButton(page, ctx, WidgetId::Difficulty, cb.cycleDifficulty);
            }
            addButton(page, ctx, WidgetId::Controls, cb.openControls);
            addButton(page, ctx, WidgetId::VideoSettings, cb.openVideoSettings);
            addButton(page, ctx, WidgetId::Language, cb.openLanguage);
            // UI-6c：26.1 的 Options 上有 Accessibility Settings…（§7.11）。
            // 字幕开关跟着搬过去了——它在 26.1 里本来就属于那一屏
            // （`AccessibilityOptionsScreen.java:25` 的 `options.showSubtitles()`）。
            addButton(page, ctx, WidgetId::Accessibility, cb.openAccessibility);
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;

        // UI-6c：26.1 §7.11 辅助功能设置。这一轮只放两项——View Bobbing（从 Controls
        // 挪来，偏差 D2）与字幕开关（从 Options 挪来）。26.1 那一屏还有十几项，
        // 其中大多数本作没有对应的玩法或表现（旁白、高对比度、聊天透明度…）；
        // **菜单背景模糊强度**是有的（UI-5 落地了它的语义与存储），但 26.1 用的是**滑块**，
        // 而本作的滑块今天只服务三个硬编码项，做通用滑块是另一块工作。已登记为余项。
        case PageId::Accessibility:
            addOptionButton(page, ctx, WidgetId::ViewBobbing, cb);
            addOptionButton(page, ctx, WidgetId::Subtitles, cb);
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;

        // UI-6d：§7.3 视频设置。26.1 的形状是「一个 preset 大按钮 + 三次 addSmall」，
        // 每次 addSmall 从新行起（那道边界是**语义分组**，见 optionsGroupedSlot）。
        //
        // 26.1 有 28 个设置项，本作只有约 12 项有真实后端。按用户裁决：**只补有后端的**，
        // 其余（伽马、暴击指示器、自动保存指示器、实体渲染距离缩放、树叶剔除、纹理过滤、
        // 天气半径…）登记在偏差表里，各自等它的渲染/玩法特性做出来再上。
        //
        // 「实验性内容」那一页没有了：雨、粒子、雨碰撞缓存是**渲染表现**项，26.1 里它们
        // 的同类（`particles`）就在这一屏；太阳阴影与动态光源归新的高级图形页。
        case PageId::VideoSettings: {
            // 这一屏装不下：只装配落在滚动窗口里的项（`optionVisible`）。
            // 页脚的 Done 不在窗口里——它是三段式版面的页脚，永远在。
            detail::OptionCursor add{ctx, id};
            // preset 大按钮：本作没有预设机制，置灰。少了它版面比 26.1 短一行。
            add([&] { addButton(page, ctx, WidgetId::GraphicsPreset, nullptr, /*enabled=*/false); });
            // 第一组：画质
            add([&] { addSlider(page, ctx, WidgetId::ViewDistance, cb.viewDistance); });
            add([&] { addSlider(page, ctx, WidgetId::SimulationDistance, cb.simulationDistance); });
            add([&] { addOptionButton(page, ctx, WidgetId::SmoothLighting, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::ParticleLevel, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::EntityShadows, cb); });
            add([&] { addIntSlider(page, ctx, cb, WidgetId::MenuBackgroundBlurriness); });
            add([&] { addOptionButton(page, ctx, WidgetId::AntiAliasing, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::Anisotropy, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::RainMode, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::RainCollisionCache, cb); });
            add([&] { addButton(page, ctx, WidgetId::AdvancedGraphics, cb.openAdvancedGraphics); });
            // 第二组：窗口
            add([&] { addOptionButton(page, ctx, WidgetId::FrameRateLimit, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::Vsync, cb); });
            add([&] { addButton(page, ctx, WidgetId::GuiScale, cb.cycleGuiScale); });
            add([&] { addButton(page, ctx, WidgetId::Resolution, cb.cycleResolution); });
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        case PageId::AdvancedGraphics: {
            detail::OptionCursor add{ctx, id};
            add([&] { addOptionButton(page, ctx, WidgetId::SunShadows, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::CascadedShadows, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::DynamicLight, cb); });
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        case PageId::Controls:
            // ★ 26.1 的第一行是**两个**跳转：`addSmall(mouse_settings, keybinds)`。
            //   鼠标设置那一屏本作没有，所以它是一个**置灰**按钮——版面与 vanilla 对上，
            //   而"这个功能还没有"看得出来。点它不会把人送进一张空页。
            //   （主菜单的 Multiplayer / Realms 是同一种做法。）
        {
            detail::OptionCursor add{ctx, id};
            add([&] {
                addButton(page, ctx, WidgetId::MouseSettings, nullptr, /*enabled=*/false);
            });
            add([&] { addButton(page, ctx, WidgetId::OpenKeyBinds, cb.openKeyBinds); });
            add([&] { addOptionButton(page, ctx, WidgetId::ToggleCrouch, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::ToggleSprint, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::ToggleAttack, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::ToggleUse, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::AutoJump, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::SprintWindow, cb); });
            add([&] { addOptionButton(page, ctx, WidgetId::OperatorItemsTab, cb); });
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        // UI-6c：26.1 的 §7.8 `KeyBindsScreen`——页眉标题、绑定列表、页脚两个按钮
        // （`controls.resetAll` 与 Done，横排）。
        case PageId::KeyBinds: {
            // 绑定行是一个滚动列表，只装配可见窗口 [keyBindFirstIndex, +keyBindRowCount)，
            // 页面因此绝不会超出布局容量。每行**三个**控件（名称 Label + 改键 Button +
            // 重置 Button），矩形由布局那一趟填。
            //
            // ★ UI-6c：窗口数的是**行**，不是动作。展开后的行表里夹着分类标题行
            //   （`ui/KeyBindList.hpp`），标题行占一行但**不产生控件**——它由绘制侧
            //   直接画（纯文本、不可交互，做成 Widget 只会让焦点遍历多停一站）。
            const std::size_t first = std::min(ctx.keyBindFirstIndex, kKeyBindListRowCount);
            const std::size_t last = std::min(first + ctx.keyBindRowCount, kKeyBindListRowCount);
            for (std::size_t row = first; row < last; ++row) {
                const auto entry = keyBindListRow(row);
                if (entry.isCategory) {
                    continue;
                }
                const input::InputAction action = entry.action;
                detail::addKeyBindRow(
                    page, ctx, action,
                    [cb, action]() { if (cb.beginKeyCapture) cb.beginKeyCapture(action); },
                    [cb, action]() { if (cb.resetKeyBind) cb.resetKeyBind(action); });
            }
            addButton(page, ctx, WidgetId::ResetKeyBinds, cb.resetKeyBinds);
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        case PageId::Language:
            for (std::size_t row = 0; row < ctx.languageRowCount; ++row) {
                addListRow(page, WidgetId::LanguageRow, row,
                           [cb, row]() { if (cb.selectLanguageRow) cb.selectLanguageRow(row); });
            }
            addOptionButton(page, ctx, WidgetId::ForceUnicodeFont, cb);
            addButton(page, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::Loading:
        case PageId::Game:
            break;  // no menu widgets (in-world HUD / loading are not menu pages)
    }
}

// 用任意一个"按序号给矩形"的函数填一页的矩形。
//
// 生产路径**不走这条**——它走 `ui::layoutPageInto`，那里按钮数是从装配结果数出来的。
// 这条留给测试：它们常想自造一套平凡的行布局（每个控件一行 20 高）来断言点击派发，
// 而不必把真实的版面求解器拖进来。
inline void applyPageRects(Page& page, const RectProvider& rectFor) {
    if (!rectFor) {
        return;
    }
    for (std::size_t index = 0; index < page.size(); ++index) {
        page[index].rect = rectFor(index);
    }
}

// 返回一份新页面
// 派发路径用它，那里点击时才构建一次；每帧绘制则走上面那个写入缓冲的重载
[[nodiscard]] inline Page buildPage(PageId id, const MenuBuildContext& ctx,
                                    const MenuCallbacks& cb, const RectProvider& rectFor) {
    Page page;
    buildPageInto(page, id, ctx, cb);
    applyPageRects(page, rectFor);
    return page;
}

}  // namespace mc::ui
