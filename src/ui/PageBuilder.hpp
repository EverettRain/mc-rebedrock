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

#include "ui/PageStack.hpp"
#include "ui/WidgetId.hpp"
#include "input/InputAction.hpp"
#include "input/InputNaming.hpp"
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
    // 某个动作那一行的标签，比如"前进: W"，正在捕获按键时则是"前进: > ? <"
    // 渲染器从 InputSystem 这个唯一来源构造它，测试可以打桩
    // 设置了它，按键设置页就渲染绑定表而不是早先那套开关脚手架
    std::function<std::string(input::InputAction action)> keyBindLabelFor{};
    // 按键设置页的绑定列表是滚动的，只装配可见窗口，与世界列表和语言列表一样
    // 无论有多少个动作，控件数量因此都有界
    // keyBindFirstIndex 是在 input::keyBindRows() 中的滚动偏移，keyBindRowCount 是可见窗口的大小
    std::size_t keyBindFirstIndex = 0;
    std::size_t keyBindRowCount = 0;
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
};

namespace detail {

[[nodiscard]] inline std::string label(const MenuBuildContext& ctx, WidgetId id) {
    return ctx.labelFor ? ctx.labelFor(static_cast<std::uint16_t>(id)) : std::string{};
}

// 追加一个普通按钮控件，它的矩形取自提供器给出的下一个序号
inline void addButton(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                      WidgetId id, std::function<void()> onActivate, bool enabled = true) {
    Widget w;
    w.kind = WidgetKind::Button;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = label(ctx, id);
    w.enabled = enabled;
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

// 一个循环选项的按钮
// 它的动作永远是那同一个通用步进，以 id 为键
// 取值、字段与标签都归表管，见 ui/OptionCycle.hpp，绝不归这个调用点管
inline void addOptionButton(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                            WidgetId id, const MenuCallbacks& cb) {
    // 回调被拷贝进控件，与其它每个动作一样
    // 因为 Page 的存活期长于 buildPage 收到的那个 MenuCallbacks 引用
    addButton(page, rectFor, ctx, id, [cycle = cb.cycleOption, id] {
        if (cycle) {
            cycle(id, /*direction=*/1);
        }
    });
}

inline void addSlider(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                      WidgetId id, SliderBind bind) {
    Widget w;
    w.kind = WidgetKind::Slider;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = label(ctx, id);
    w.slider = std::move(bind);
    page.push_back(std::move(w));
}

inline void addListRow(Page& page, const RectProvider& rectFor, WidgetId id, std::size_t rowIndex,
                       std::function<void()> onActivate) {
    Widget w;
    w.kind = WidgetKind::ListRow;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.onActivate = std::move(onActivate);
    static_cast<void>(rowIndex);
    page.push_back(std::move(w));
}

// 一个按键绑定行：形如"动作: 按键"的 ListRow，点击它就开始为该动作捕获新按键
// 标签经上下文的 keyBindLabelFor 来自 InputSystem 这个唯一来源
// enabled 恒为真，任何一行都可以重绑
inline void addKeyBindRow(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                          input::InputAction action, std::function<void()> onActivate) {
    Widget w;
    w.kind = WidgetKind::ListRow;
    w.debugId = static_cast<std::uint16_t>(WidgetId::KeyBindRow);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = ctx.keyBindLabelFor ? ctx.keyBindLabelFor(action) : std::string{};
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

}  // namespace detail

// 把 id 对应的页面装配进调用方给的缓冲
// 控件顺序与从前每页一份的数组一致，每个回调执行的正是旧 switch 分支所做的事
// page 会先被清空，容量因此跨次复用
// 绘制侧每帧都要一份当前页，走这个重载就不必每帧向堆要一个新的 vector
inline void buildPageInto(Page& page, PageId id, const MenuBuildContext& ctx,
                          const MenuCallbacks& cb, const RectProvider& rectFor) {
    using detail::addButton;
    using detail::addOptionButton;
    using detail::addListRow;
    using detail::addSlider;
    page.clear();

    switch (id) {
        case PageId::Title:
            addButton(page, rectFor, ctx, WidgetId::Singleplayer, cb.openSingleplayer);
            addButton(page, rectFor, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, rectFor, ctx, WidgetId::Exit, cb.exitGame);
            break;

        case PageId::Pause:
            addButton(page, rectFor, ctx, WidgetId::Resume, cb.resume);
            addButton(page, rectFor, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, rectFor, ctx, WidgetId::SaveQuit, cb.saveAndQuit);
            break;

        case PageId::Death:
            addButton(page, rectFor, ctx, WidgetId::Respawn, cb.respawn);
            addButton(page, rectFor, ctx, WidgetId::TitleScreen, cb.returnToTitle);
            break;

        case PageId::WorldList:
            // 先是滚动的存档行，也就是列表主体，然后是四个动作按钮
            // 按钮顺序沿用惯例：进入、创建、编辑、返回
            for (std::size_t row = 0; row < ctx.worldRowCount; ++row) {
                addListRow(page, rectFor, WidgetId::WorldRow, row,
                           [cb, row]() { if (cb.selectWorldRow) cb.selectWorldRow(row); });
            }
            addButton(page, rectFor, ctx, WidgetId::PlaySelected, cb.playSelectedWorld,
                      ctx.worldSelectable);
            addButton(page, rectFor, ctx, WidgetId::CreateWorld, cb.createWorld);
            addButton(page, rectFor, ctx, WidgetId::Edit, cb.editWorld, ctx.worldSelectable);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::CreateWorld:
            addButton(page, rectFor, ctx, WidgetId::CreateGameMode, cb.toggleCreateGameMode);
            addButton(page, rectFor, ctx, WidgetId::CreateAllowCommands,
                      cb.toggleCreateAllowCommands);
            addButton(page, rectFor, ctx, WidgetId::CreateConfirm, cb.confirmCreate);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::EditWorld:
            addButton(page, rectFor, ctx, WidgetId::SaveRename, cb.renameWorld);
            addButton(page, rectFor, ctx, WidgetId::DeleteWorld, cb.deleteWorld);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::ConfirmDelete:
            addButton(page, rectFor, ctx, WidgetId::DeleteConfirm, cb.confirmDelete);
            addButton(page, rectFor, ctx, WidgetId::DeleteCancel, cb.cancelDelete);
            break;

        case PageId::Options:
            addSlider(page, rectFor, ctx, WidgetId::MasterVolume, cb.masterVolume);
            if (ctx.worldOpen) {
                addButton(page, rectFor, ctx, WidgetId::Difficulty, cb.cycleDifficulty);
            }
            addButton(page, rectFor, ctx, WidgetId::Controls, cb.openControls);
            addButton(page, rectFor, ctx, WidgetId::VideoSettings, cb.openVideoSettings);
            addOptionButton(page, rectFor, ctx, WidgetId::Subtitles, cb);
            addButton(page, rectFor, ctx, WidgetId::Language, cb.openLanguage);
            addButton(page, rectFor, ctx, WidgetId::Experimental, cb.openExperimental);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::VideoSettings:
            addButton(page, rectFor, ctx, WidgetId::Resolution, cb.cycleResolution);
            addButton(page, rectFor, ctx, WidgetId::GuiScale, cb.cycleGuiScale);
            addSlider(page, rectFor, ctx, WidgetId::ViewDistance, cb.viewDistance);
            addSlider(page, rectFor, ctx, WidgetId::SimulationDistance, cb.simulationDistance);
            addOptionButton(page, rectFor, ctx, WidgetId::FrameRateLimit, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::AntiAliasing, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::Anisotropy, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::SmoothLighting, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::DynamicLight, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::Vsync, cb);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::Controls: {
            // 按键绑定行是一个滚动列表，只装配可见窗口 [keyBindFirstIndex, +keyBindRowCount)
            // 页面因此绝不会超出布局容量，24 个固定按钮会直接抛出
            // 每行的矩形来自 rectFor，渲染器把这些列表下标映射到 controlsRow 的矩形上
            // 末尾四个是底部按钮
            // 每一行点击时开始它的重绑捕获，标签来自 InputSystem 这个唯一来源
            constexpr auto rows = input::keyBindRows();
            const std::size_t first = std::min(ctx.keyBindFirstIndex, rows.size());
            const std::size_t last = std::min(first + ctx.keyBindRowCount, rows.size());
            for (std::size_t i = first; i < last; ++i) {
                const input::InputAction action = rows[i];
                detail::addKeyBindRow(page, rectFor, ctx, action, [cb, action]() {
                    if (cb.beginKeyCapture) cb.beginKeyCapture(action);
                });
            }
            addOptionButton(page, rectFor, ctx, WidgetId::ViewBobbing, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::AutoJump, cb);
            addButton(page, rectFor, ctx, WidgetId::ResetKeyBinds, cb.resetKeyBinds);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        case PageId::Language:
            for (std::size_t row = 0; row < ctx.languageRowCount; ++row) {
                addListRow(page, rectFor, WidgetId::LanguageRow, row,
                           [cb, row]() { if (cb.selectLanguageRow) cb.selectLanguageRow(row); });
            }
            addOptionButton(page, rectFor, ctx, WidgetId::ForceUnicodeFont, cb);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::Experimental:
            addOptionButton(page, rectFor, ctx, WidgetId::RainMode, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::ParticleLevel, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::SunShadows, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::RainCollisionCache, cb);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::Loading:
        case PageId::Game:
            break;  // no menu widgets (in-world HUD / loading are not menu pages)
    }
}

// 返回一份新页面
// 派发路径用它，那里点击时才构建一次；每帧绘制则走上面那个写入缓冲的重载
[[nodiscard]] inline Page buildPage(PageId id, const MenuBuildContext& ctx,
                                    const MenuCallbacks& cb, const RectProvider& rectFor) {
    Page page;
    buildPageInto(page, id, ctx, cb, rectFor);
    return page;
}

}  // namespace mc::ui
