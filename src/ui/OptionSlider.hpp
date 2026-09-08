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

#include "audio/SoundCategory.hpp"
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

inline constexpr std::array<IntSliderDesc, 2> kIntSliders{{
    // 26.1 `Options.menuBackgroundBlurriness`：IntRange(0, 10)，默认 5，0 显示 OFF。
    {WidgetId::MenuBackgroundBlurriness, "options.accessibility.menu_background_blurriness",
     "Menu Background Blurriness", &config::GameOptions::menuBackgroundBlurriness, 0, 10,
     /*offAtMinimum=*/true},
    // UI-6e：视场角。26.1 `Options.fov` 的 IntRange(30, 110)。
    // ★ 它的两个特殊标签（70 → Normal、110 → Quake Pro）**不进这张表**：
    //   那两个判据是"取值等于某个具体数"，而 offAtMinimum 是"到最小档"——
    //   70 是默认值不是最小值（最小是 30）。为两个特例给表加两组字段，会让其余
    //   每一项都背上用不到的列。特例留在 widgetLabel 里，表只管取值范围。
    {WidgetId::FieldOfView, "options.fov", "FOV", &config::GameOptions::fieldOfView, 30, 110,
     /*offAtMinimum=*/false},
}};

// UI-6e：取值是 **float** 的滑块。
//
// 与 IntSliderDesc 的区别不只是类型：整数滑块有"档"（0..10 共 11 档，拖动会吸附），
// float 滑块是连续的（26.1 的 `OptionInstance.UnitDouble`，取值域就是 [0,1]）。
// 把两者塞进一个模板只会让 `intSliderValue` 那条"四舍五入不是截断"的注释失去意义——
// 它对连续量根本不适用。
//
// ★ **取值位置有两种**，这是这张表最别扭也最必要的一处。26.1 的十个音量在
//   `Options.soundSourceVolumes` 这张 map 里，本作对应的是
//   `GameOptions::soundCategoryVolumes`——一个 `std::array<float, 10>`，
//   **不是十个独立字段**。而 `float GameOptions::*` 这种成员指针**指不到数组的某一格**。
//   所以表项要么给字段指针，要么给一个类别下标；两者只能有一个。
//   （试过让它们统一成"读写函数指针对"，那样每一项要写两个 lambda，
//   表就不再是"一行一项"，加一个滑块反而更贵。）
struct FloatSliderDesc final {
    WidgetId id = WidgetId::None;
    std::string_view nameKey{};
    std::string_view nameFallback{};
    // 二选一：`field` 指向 GameOptions 的一个 float 字段；
    // 若 `categoryVolume != Count`，改为指向 soundCategoryVolumes 的那一格。
    float config::GameOptions::* field = nullptr;
    audio::SoundCategory categoryVolume = audio::SoundCategory::Count;
    // 0 显示为 OFF 而不是 "0%"（26.1 `Options.percentValueOrOffLabel`）。
    bool offAtZero = true;
};

// 十类音量。顺序照 26.1 `SoundSource` 枚举，也就是本作 `audio::SoundCategory`。
//
// ★ 主音量走**字段**（`GameOptions::masterVolume` 是权威值），其余九类走**数组格**
//   （`soundCategoryVolumes[类别]`）。两条取值路径的分歧只在 floatSliderValue /
//   setFloatSliderValue 两处——表里看不出来，所以那两条路径各有断言。
//
// ★ 26.1 有**十一**类（多一个 `SoundSource.UI`）。本作没有 UI 那一档（偏差 D6，
//   按钮音走 Master），所以这张表比 vanilla 少一行——差额是可数的，不是漏了。
inline constexpr std::array<FloatSliderDesc, 10> kFloatSliders{{
    // 26.1 `SoundOptionsScreen` 用 addBig 把 MASTER 单独放一行。
    {WidgetId::MasterVolume, "soundCategory.master", "Master Volume",
     &config::GameOptions::masterVolume, audio::SoundCategory::Count, /*offAtZero=*/true},
    {WidgetId::MusicVolume, "soundCategory.music", "Music",
     nullptr, audio::SoundCategory::Music, true},
    {WidgetId::RecordVolume, "soundCategory.record", "Jukebox/Note Blocks",
     nullptr, audio::SoundCategory::Record, true},
    {WidgetId::WeatherVolume, "soundCategory.weather", "Weather",
     nullptr, audio::SoundCategory::Weather, true},
    {WidgetId::BlockVolume, "soundCategory.block", "Blocks",
     nullptr, audio::SoundCategory::Block, true},
    {WidgetId::HostileVolume, "soundCategory.hostile", "Hostile Creatures",
     nullptr, audio::SoundCategory::Hostile, true},
    {WidgetId::NeutralVolume, "soundCategory.neutral", "Friendly Creatures",
     nullptr, audio::SoundCategory::Neutral, true},
    {WidgetId::PlayerVolume, "soundCategory.player", "Players",
     nullptr, audio::SoundCategory::Player, true},
    {WidgetId::AmbientVolume, "soundCategory.ambient", "Ambient/Environment",
     nullptr, audio::SoundCategory::Ambient, true},
    {WidgetId::VoiceVolume, "soundCategory.voice", "Voice/Speech",
     nullptr, audio::SoundCategory::Voice, true},
}};

// ★ 每一类音量都必须恰好出现一次。漏一类 = 那一档在界面上根本调不了（而它仍然
//   在存档里、仍然在乘），多一类 = 两个滑块改同一个值。
[[nodiscard]] constexpr bool floatSlidersCoverEverySoundCategory() {
    for (std::size_t index = 0; index < audio::kSoundCategoryCount; ++index) {
        const auto category = static_cast<audio::SoundCategory>(index);
        std::size_t seen = 0;
        for (const FloatSliderDesc& desc : kFloatSliders) {
            // 主音量那一项走字段而不是数组格，它代表的正是 Master。
            const bool isMaster = desc.categoryVolume == audio::SoundCategory::Count &&
                                  desc.id == WidgetId::MasterVolume;
            if (desc.categoryVolume == category ||
                (isMaster && category == audio::SoundCategory::Master)) {
                ++seen;
            }
        }
        if (seen != 1U) {
            return false;
        }
    }
    return true;
}

static_assert(floatSlidersCoverEverySoundCategory(),
              "每一类音量都要恰好一个滑块——漏一类那一档就再也调不了了");
static_assert(kFloatSliders.size() == audio::kSoundCategoryCount,
              "十类音量，十个滑块");

[[nodiscard]] constexpr const FloatSliderDesc* findFloatSlider(WidgetId id) {
    for (const FloatSliderDesc& desc : kFloatSliders) {
        if (desc.id == id) {
            return &desc;
        }
    }
    return nullptr;
}

// 读一个 float 滑块的当前值。两种取值位置的分歧**只在这里和下面那个写函数里**，
// 调用方不必知道某一项是字段还是数组格。
[[nodiscard]] inline float floatSliderValue(const FloatSliderDesc& desc,
                                            const config::GameOptions& options) {
    if (desc.categoryVolume != audio::SoundCategory::Count) {
        return options.soundCategoryVolumes[static_cast<std::size_t>(desc.categoryVolume)];
    }
    return desc.field != nullptr ? options.*(desc.field) : 0.0F;
}

inline void setFloatSliderValue(const FloatSliderDesc& desc, config::GameOptions& options,
                                float value) {
    const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    if (desc.categoryVolume != audio::SoundCategory::Count) {
        options.soundCategoryVolumes[static_cast<std::size_t>(desc.categoryVolume)] = clamped;
        return;
    }
    if (desc.field != nullptr) {
        options.*(desc.field) = clamped;
    }
}

// 百分比档位（26.1 `Options.percentValueLabel`）。
//
// ★ **截断，不是四舍五入**：`(int)(value * 100.0)`（`Options.java:1911`）。
//   这与整数滑块那条"必须四舍五入"的规则**方向相反**，两者都是照抄 vanilla——
//   写成一样的只会让其中一个不对。0.999 在 vanilla 里显示 99%，不是 100%。
[[nodiscard]] constexpr int floatSliderPercent(float fraction) {
    const float clamped = fraction < 0.0F ? 0.0F : (fraction > 1.0F ? 1.0F : fraction);
    return static_cast<int>(clamped * 100.0F);
}

// 这一项该显示 OFF 吗（`percentValueOrOffLabel`：**值为 0** 时显示 OFF）。
// 注意判据是**原始值 == 0**，不是"百分比取整后为 0"——0.004 的百分比是 0，
// 但 vanilla 显示的是 "0%" 而不是 OFF。
[[nodiscard]] constexpr bool floatSliderShowsOff(const FloatSliderDesc& desc, float value) {
    return desc.offAtZero && value == 0.0F;
}

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
