#pragma once

// UI-6d：由**表**驱动的整数滑块。
//
// 本作在这之前只有三个滑块，各自硬编码：渲染距离、模拟距离、主音量。它们在
// `MenuCallbacks` 里各占一个成员、在渲染器里各填三个 lambda、在 `widgetLabel` 里各写
// 一个 case——加第四个滑块要动三处，而漏一处的症状是滑块画得出来却拖不动，
// 或者拖得动但标签不跟着变。
//
// 26.1 的 `menuBackgroundBlurriness` 是滑块（`OptionInstance.IntRange(0, 10)`，
// `Options.java:273-278`），UI-5 已经把它的语义与存储做好、把 UI 留给了这一轮。
// 它是这张表的第一个消费者；高级图形页将来的光影参数会是第二批。
//
// 表里只放**取值是 `GameOptions` 的一个 int 字段**的滑块。那三个既有滑块不进来：
// 渲染距离与模拟距离读的是渲染器的运行期值（不是 options 字段），主音量是 float，
// 而且它们工作正常——把它们搬进来要连带碰音频与区块流送，不是这一轮的事。

#include "config/GameOptions.hpp"
#include "ui/WidgetId.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace mc::ui {

struct IntSliderDesc final {
    WidgetId id = WidgetId::None;
    std::string_view nameKey{};
    std::string_view nameFallback{};
    int config::GameOptions::* field = nullptr;
    int minimum = 0;
    int maximum = 1;
    // 最小档显示为 OFF 而不是数字（26.1 的 `genericValueOrOffLabel`）。
    bool offAtMinimum = false;
};

inline constexpr std::array<IntSliderDesc, 1> kIntSliders{{
    // 26.1 `Options.menuBackgroundBlurriness`：IntRange(0, 10)，默认 5，0 显示 OFF。
    {WidgetId::MenuBackgroundBlurriness, "options.accessibility.menu_background_blurriness",
     "Menu Background Blurriness", &config::GameOptions::menuBackgroundBlurriness, 0, 10,
     /*offAtMinimum=*/true},
}};

[[nodiscard]] constexpr const IntSliderDesc* findIntSlider(WidgetId id) {
    for (const IntSliderDesc& desc : kIntSliders) {
        if (desc.id == id) {
            return &desc;
        }
    }
    return nullptr;
}

// 当前值 → 滑块比例 [0,1]。范围退化（min == max）时给 0 而不是除零。
[[nodiscard]] constexpr float intSliderFraction(const IntSliderDesc& desc, int value) {
    const int span = desc.maximum - desc.minimum;
    if (span <= 0) {
        return 0.0F;
    }
    const int clamped = std::clamp(value, desc.minimum, desc.maximum);
    return static_cast<float>(clamped - desc.minimum) / static_cast<float>(span);
}

// 滑块比例 → 值。
//
// ★ 四舍五入，不是截断。截断会让滑块**永远到不了最大档**：拖到最右端时比例是 1.0，
//   截断后仍是 max，但拖到 0.99 就掉回 max-1，而滑块看起来已经贴到头了。
//   更常见的症状是每一档的"吸附点"整体偏左半格。
[[nodiscard]] constexpr int intSliderValue(const IntSliderDesc& desc, float fraction) {
    const int span = desc.maximum - desc.minimum;
    if (span <= 0) {
        return desc.minimum;
    }
    const float clamped = fraction < 0.0F ? 0.0F : (fraction > 1.0F ? 1.0F : fraction);
    const int steps = static_cast<int>(clamped * static_cast<float>(span) + 0.5F);
    return desc.minimum + std::clamp(steps, 0, span);
}

} // namespace mc::ui
