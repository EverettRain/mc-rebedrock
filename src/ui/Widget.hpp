#pragma once

// 数据驱动的菜单对象模型
// 一个菜单页面是一列扁平的 Widget 值，形状上允许嵌套，但既不是常驻的面向对象树，也不是虚表继承体系
// 每个控件自带几何、标签、是否可用，以及一个激活时触发的 std::function 回调
// 它取代了从前那三者的耦合：一个 MenuButton 枚举、每页一份 constexpr 数组
// 外加约 300 行的 switch 派发
// 现在页面在 PageBuilder 一处装配，在 MenuInteraction 通用地命中与派发
// 再由渲染器的绘制后端按 kind 通用地画出
//
// 这里不碰 Vulkan 也不碰 GLFW，它住在 mc_rebedrock_runtime 里
// 模型、命中测试与派发因此能被无头单测覆盖：搭一个页面，点一个坐标，断言对应的回调被触发
// 回调可以捕获渲染器需要的任何 Vulkan、存档或音频状态，ui 命名空间从不接触这些
//
// A0：容器界面（背包/箱子/工作台/熔炉/附魔台/铁砧/创造背包）也并进了这个模型
// 它此前自成一套：SlotView + HudLayout 的 28 个具名槽位函数 + 绘制侧的 if/else 链
// 于是菜单侧攒下的每一条护栏——控件不越界、命中、焦点遍历——对容器屏一条都不生效
//
// ★ 页面一律是**扁平**的。`Widget::children` 曾作为"将来容器界面用得上"的形状留着，
//   而它 0 消费者、从没搭成框架；A0 真做容器界面时用的是**扁平相邻 Widget**
//   （一行多个控件靠相邻，容器槽位同理），与 26.1 `children()` 的线性 Tab 序一致
//   且不需要递归。那个空壳字段已删——留着一个没有消费者的嵌套形状，只会让下一个人
//   以为该往里放东西。

#include "gameplay/ScreenTypes.hpp"
#include "ui/HudLayout.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc::ui {

enum class WidgetKind : std::uint8_t {
    Button,     // a clickable button (GuiNineSlice + label)
    // UI-4：只有图标没有文字的方钮（26.1 的 SpriteIconButton，iconOnly=true）。
    // 与 Button 完全同形——同一张九宫格底、同一套命中与派发——区别只在于绘制侧画的是
    // 一张 15x15 的图标而不是一行标签。所以它是一个 kind，不是一个新的控件家族。
    IconButton,
    Slider,     // a horizontal slider with a draggable handle
    ListRow,    // one selectable row in a scrolling list (worlds/languages)
    Label,      // static text, never interactive
    Panel,      // 不可交互的底板（容器界面那张 176x166 / 195x136 的面板就是它）
    Toggle,     // a button whose label reflects an on/off (cycled) option
    TextField,  // an editable text line (create/edit world name)
    // UI-9：标签页导航栏里的一个页签（26.1 `TabButton`）。
    //
    // 它是一个 kind 而不是"用 debugId 特判的 Button"，理由与 IconButton 同：同一套
    // 命中、同一套派发，区别只在绘制侧画的是四态页签精灵 + 选中下划线。
    // ★ **绝不能靠 debugId 分派绘制**——那个字段的约定是"只给测试与日志，绝不用来
    //   分派行为"（WidgetId.hpp 的开篇），破一次这条，下一个人就会再破一次。
    Tab,
    // UI-10 / D24：**纯命中区**——参与命中与派发，但自己什么都不画。
    //
    // ★ 它是为可转移列表的行内箭头而立的：26.1 把三张 32x32 的箭头精灵画在**同一个**
    //   图标位上（`TransferableSelectionList:164-195` 全是
    //   `blit(..., getContentX(), getContentY(), 32, 32)`），光标落在左半 / 右上 1/4 /
    //   右下 1/4 只决定**用哪一张、要不要高亮**。也就是说"热区"与"画在哪"不是一回事：
    //   画归那一行，热区只管点。
    IconZone,
    // A0：容器界面的一个槽位。身份是 `slotKind + slotIndex` 两个字段，**不是**
    // 指针——跨帧身份一直就是纯值 `gameplay::SlotRef`（`buildSlotLayout` 刻意把每个
    // storage 置空，好让渲染线程够不着模拟线程拥有的背包内存）。
    //
    // 它是一个 kind 而不是另一个控件家族，理由与 IconButton 同：同一套命中、同一套
    // 派发，区别只在绘制侧画的是一格物品。
    Slot,
    // UI-11 / A5：一个复选框（26.1 `Checkbox`）。左边一个 17x17 的方盒（四态精灵），
    // 右边一行文字，整块都是热区——它在 26.1 里就是个 `AbstractButton`。
    //
    // 它是一个 kind 而不是"标签写成 [x] 的 Button"：勾没勾上是**状态**，
    // 而按钮的外观只由 enabled / hover / pressed 决定。把它塞进 Button 就得靠
    // debugId 分派绘制，那条路 Tab 那里已经明令禁止过一次。
    Checkbox,
    // UI-11 / A6：一张图（26.1 `GuiGraphics.blit` 出来的那种，不是九宫格控件）。
    //
    // ★ 第一个消费者是世界列表每一行左边那张 32x32 的存档缩略图
    //   （`WorldSelectionList.WorldListEntry`，spec §7 的 `<Image id="icon">`）。
    //   它**不可交互**：26.1 里图标位上那块"点一下直接进世界"的热区是行自己的
    //   `mouseOverIcon`，不是图片控件——热区与画在哪不是一回事（IconZone 那条注释）。
    //   做成控件的收益是那张图从此吃到"控件不越界"这条通用护栏，而它此前是绘制
    //   循环里就地算出来的一个矩形，任何护栏都够不着。
    Image,
};

// UI-4：图标钮里那张图标的边长与按钮边长（26.1 `CommonButtons`：20x20 的钮里一张 15x15 的图）。
inline constexpr float kIconButtonSize = 20.0F;
inline constexpr float kIconButtonIconSize = 15.0F;

// 图标在钮内的矩形。
//
// ★ **整数除法的顺序也要照抄 vanilla，不能代数化简。**
//   26.1 `SpriteIconButton.CenteredIcon.extractContents`（:132-133）：
//       x = getX() + getWidth()/2  - spriteWidth/2      // 20/2 - 15/2 = 10 - 7 = 3
//   本作从前写的是 `(20 - 15) / 2` = 5/2 = **2** —— 两个式子在实数上相等，
//   在整数除法下差 1。症状就是现场报告的那句"图标偏左上角"：前缘 2、后缘 3，
//   而 vanilla 是前缘 3、后缘 2。
//   旧注释还振振有词地写着"整数居中就是这样，不是对称的"——**方向反了**，
//   vanilla 那条式子偏的是右下，不是左上。
//
// 它是一个纯函数而不是绘制侧的三行算术，因为"图标没居中"改不动任何返回值——
// 那正是 UI-4 第一轮 sabotage 没抓住的形状（REGULAR §5：没抓住就补测试，不换 sabotage）。
// 而这一次说明：抽成纯函数**还不够**，断言必须钉住那个具体的数，不能只说"它居中"。
[[nodiscard]] inline UiRect iconButtonIconRect(const UiRect& button, float scale) {
    const int inset =
        static_cast<int>(kIconButtonSize) / 2 - static_cast<int>(kIconButtonIconSize) / 2;
    return {
        button.x + static_cast<float>(inset) * scale,
        button.y + static_cast<float>(inset) * scale,
        kIconButtonIconSize * scale,
        kIconButtonIconSize * scale,
    };
}

// 滑块的数据与回调
// value() 给出当前用于显示的值，绘制后端据此画滑块位置，该值可能是归一化的也可能是原始的
// onDrag(fraction) 施加一个新位置，fraction 是轨道上 [0,1] 的比例
// 规矩是把副作用留在回调里而不是留在遍历里
// 遍历中绝不出现 if (kind == Slider) 这样的分支，它只负责调 onDrag
struct SliderBind final {
    std::function<float()> value;             // current fraction in [0,1], for drawing
    std::function<void(float)> onDrag;        // apply a new fraction in [0,1]
    std::function<void()> onCommit;           // release: persist / play feedback
};

// 一个菜单元素，以值的形式存在，可拷贝可移动，一个页面持有它们的 vector
// onActivate 是点击动作，供 Button、Toggle 与 ListRow 使用，Slider 改用 slider 字段
// debugId 是可选的稳定标识，只留给测试与日志，绝不用来分派行为
struct Widget final {
    WidgetKind kind = WidgetKind::Button;
    UiRect rect{};
    std::string label{};
    bool enabled = true;
    std::uint16_t debugId = 0;  // optional test/debug tag; 0 == none

    std::function<void()> onActivate{};  // Button/Toggle/ListRow click
    SliderBind slider{};                 // Slider only

    // UI-10 / D20：悬停时显示的提示框文本。空 = 没有。
    //
    // ★ 26.1 的每个 `AbstractWidget` 都能挂一个 `Tooltip`（`WidgetTooltipHolder`），
    //   显示条件是"悬停，或键盘聚焦且上一次输入来自键盘"，延迟默认是
    //   `Duration.ZERO`（**没有延迟**——只有个别控件调 `setTooltipDelay`）。
    //   本作此前只有容器屏的物品有提示框，菜单控件一个都没有：`drawTooltipBox`
    //   与它的定位（`ui::positionTooltip`）早就是通用的，缺的只是"控件能带一句话"
    //   这一层。
    std::string tooltip{};

    // UI-11 / A5：`kind == Checkbox` 时这一格勾上了没有。其余 kind 下没有意义。
    //
    // ★ 它是**装配时抓的一张快照**，真相在屏幕状态里（`MenuSystem::noticeStopShowing`）
    //   ——页面每帧重建，快照因此每帧都是新的。这与滑块把 `value()` 做成回调是同一件事
    //   的两种写法：滑块每帧要被拖，复选框一帧只读一次。
    bool checked = false;

    // UI-11 / A6：`kind == Image` 时画第几张图。语义由 debugId 决定——
    // `WidgetId::WorldIcon` 时它是**窗口内**的行号（与 `selectWorldRow` 收到的那个
    // 行号同一个口径），绘制侧加上滚动起点才是存档的绝对下标。
    //
    // ★ 用「窗口内行号」而不是绝对下标：装配侧只知道窗口有几行（`ctx.worldRowCount`），
    //   绝对下标要另外把滚动起点也喂给装配——那就是第二份可能与布局不同步的表述。
    std::uint16_t imageIndex = 0U;

    // A0：`kind == Slot` 时这一格是哪个槽。其余 kind 下这两个字段没有意义。
    //
    // ★ 为什么直接用 `gameplay::SlotKind` 而不在 ui 里另建一个镜像枚举：那会是
    //   **同一个事实的两份表述**（README 护栏 18）。加一种槽（比如酿造台）要改两处，
    //   而漏改的症状是"槽位画对了、点击路由到另一个 kind"——没有任何断言会红。
    //   `gameplay/ScreenTypes.hpp` 是个只有两个枚举、不含任何 ui 头的小文件，
    //   包含它不成环（ui/UiFrameData、ui/ItemTooltip、ui/MenuSystem 早就依赖 gameplay）。
    gameplay::SlotKind slotKind = gameplay::SlotKind::PlayerInventory;
    std::uint16_t slotIndex = 0U;

    [[nodiscard]] bool interactive() const noexcept {
        return kind != WidgetKind::Label && kind != WidgetKind::Panel &&
               kind != WidgetKind::Image;
    }
};

// 一个页面就是一列控件值，每次打开页面时重建
// 不做脏标记，菜单是冷路径，重建一次的代价微不足道
using Page = std::vector<Widget>;

}  // namespace mc::ui
