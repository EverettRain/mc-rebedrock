#pragma once

// 循环选项的唯一来源：它步进的取值、它所在的 GameOptions 字段，以及标签所用的翻译键
// 一个选项一行表数据
//
// 从前每个选项要在三处手工对齐：渲染器里一个自己写步进的 cb.cycleXxx lambda
// 那些步进有的是 (x + 1) % 3，有的是一个 switch (quality) 的 next 函数，还有的是在局部数组上 find
// 加上 widgetLabel() 里一个负责格式化取值的 case，再加上字段本身
// 新增一个选项要改三处，漏掉一处的症状就是按钮的标签与它实际做的事对不上
//
// 现在它们是一行，步进与标签都由这一行导出：
//   cycleOptionValue(desc, options, +1) 向前步进，传 -1 向后
//   之所以存在向后这个方向，是因为取值列表是有序数据而不是写死的 next() 链
//   正是这一点让双向选择控件成为一次界面改动，而不是把每个选项重写一遍
//   optionValueLabel(desc, value, translate) 渲染标签里表示取值的那一半
//
// 这张表是 constexpr 数据：不分配、没有初始化顺序问题，无头测试可以遍历它
// 它刻意不携带的一样东西是改动对渲染器的副作用，比如重建交换链、重新网格化世界、重新调整粒子系统
// 那些要碰 Vulkan，住在渲染器自己的 applyOptionChanged() 里，那是响应一次改动的唯一位置

#include "config/GameOptions.hpp"
#include "ui/Language.hpp"
#include "ui/WidgetId.hpp"
#include "world/WorldConstants.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace mc::ui {

// 选项取值列表里的一个位置
// value 是存储值，bool 字段存 0 或 1，枚举字段存它的底层值
// key 与 fallback 指明它的翻译，两者留空则改为按该行的 numberSuffix 或 numberTemplate 渲染数字
struct OptionValue final {
    int value = 0;
    std::string_view key{};
    std::string_view fallback{};
};

// 一个选项读写的 GameOptions 字段
// 三种形态覆盖全部循环选项：开关标志、整数，以及那个唯一的三态枚举
using OptionField = std::variant<bool config::GameOptions::*, int config::GameOptions::*,
                                 world::SmoothLightingQuality config::GameOptions::*>;

struct OptionDesc final {
    WidgetId id = WidgetId::None;
    // 选项的名字，由 vanilla 的翻译键加上它的英文文本构成
    // 每个标签都带兜底文本，缺这个键的语言因此仍然读得通
    std::string_view nameKey{};
    std::string_view nameFallback{};
    OptionField field{};
    // 这个选项步进经过的取值，按循环顺序排列
    std::span<const OptionValue> values{};
    // 没有自己翻译键的取值怎么渲染：直接给数字、给数字加一个字面后缀（"16" 加 "x"）
    // 或者走 vanilla 那种单参数模板（"%s fps"）
    std::string_view numberSuffix{};
    std::string_view numberTemplateKey{};
    std::string_view numberTemplateFallback{};
};

// ---- 共用的取值列表 -------------------------------------------------------

inline constexpr std::array<OptionValue, 2> kOnOffValues{{
    {0, "options.off", "OFF"},
    {1, "options.on", "ON"},
}};

// vanilla 的最大帧率循环，最后一档是不限制，存储值为 0
inline constexpr std::array<OptionValue, 6> kFrameRateValues{{
    {30}, {60}, {120}, {144}, {240},
    {0, "options.framerateLimit.max", "Unlimited"},
}};

// 各向异性过滤逐级翻倍到 16x 再回绕
// 与从前那句 anisotropy >= 16 ? 1 : anisotropy * 2 产生的序列相同，现在直接把取值写出来
inline constexpr std::array<OptionValue, 5> kAnisotropyValues{{{1}, {2}, {4}, {8}, {16}}};

// 平滑光照是一个三态的质量档，循环顺序是关、标准、高
inline constexpr std::array<OptionValue, 3> kSmoothLightingValues{{
    {static_cast<int>(world::SmoothLightingQuality::Off), "options.ao.off", "OFF"},
    {static_cast<int>(world::SmoothLightingQuality::Standard), "options.ao.min", "Minimum"},
    {static_cast<int>(world::SmoothLightingQuality::High), "options.ao.max", "Maximum"},
}};

// 降雨的绘制路径，属实验性内容：贴图雨幕，以及实例化 SSBO 粒子
inline constexpr std::array<OptionValue, 2> kRainModeValues{{
    {0, "options.rebedrock.rainMode.texture", "Texture Rain"},
    {1, "options.rebedrock.rainMode.async", "Asynchronous Particle Rain"},
}};

// 粒子密度，也就是粒子效果档位，它同时缩放雨的预算与粒子系统的存活上限和生成数量
inline constexpr std::array<OptionValue, 4> kParticleLevelValues{{
    {0, "options.rebedrock.particleLevel.low", "Low (0.5x)"},
    {1, "options.rebedrock.particleLevel.medium", "Medium (1x)"},
    {2, "options.rebedrock.particleLevel.high", "High (2x)"},
    {3, "options.rebedrock.particleLevel.crazy", "Crazy (3x)"},
}};

// ---- 表本身 ---------------------------------------------------------------
//
// 按钮在固定取值列表上步进的每一个选项
// 滑块是另一种控件，不在这里，指渲染距离、模拟距离与主音量
// 另有三项设置根本不是 GameOptions 字段，也不在这里
// 分辨率读实时窗口尺寸，GUI 缩放读菜单状态，难度读当前打开的存档
// 这三项仍由渲染器直接处理

inline constexpr std::array<OptionDesc, 14> kCyclingOptions{{
    {WidgetId::AutoJump, "options.autoJump", "Auto-Jump", &config::GameOptions::autoJump,
     kOnOffValues},
    {WidgetId::FrameRateLimit, "options.framerateLimit", "Max Framerate",
     &config::GameOptions::frameRateLimit, kFrameRateValues, /*numberSuffix=*/{},
     "options.framerate", "%s fps"},
    {WidgetId::AntiAliasing, "options.rebedrock.antiAliasing", "Anti-Aliasing",
     &config::GameOptions::antiAliasing, kOnOffValues},
    {WidgetId::Anisotropy, "options.maxAnisotropy", "Anisotropic Filtering",
     &config::GameOptions::anisotropy, kAnisotropyValues, /*numberSuffix=*/"x"},
    {WidgetId::SmoothLighting, "options.ao", "Smooth Lighting",
     &config::GameOptions::smoothLightingQuality, kSmoothLightingValues},
    {WidgetId::DynamicLight, "options.rebedrock.dynamicLights", "Dynamic Lighting",
     &config::GameOptions::dynamicLight, kOnOffValues},
    {WidgetId::Vsync, "options.vsync", "VSync", &config::GameOptions::vsync, kOnOffValues},
    {WidgetId::ViewBobbing, "options.viewBobbing", "View Bobbing",
     &config::GameOptions::viewBobbing, kOnOffValues},
    {WidgetId::ForceUnicodeFont, "options.forceUnicodeFont", "Force Unicode Font",
     &config::GameOptions::forceUnicodeFont, kOnOffValues},
    {WidgetId::Subtitles, "options.showSubtitles", "Show Subtitles",
     &config::GameOptions::showSubtitles, kOnOffValues},
    {WidgetId::RainMode, "options.rebedrock.rainMode", "Rain Mode",
     &config::GameOptions::rainMode, kRainModeValues},
    {WidgetId::ParticleLevel, "options.particles", "Particles",
     &config::GameOptions::particleLevel, kParticleLevelValues},
    {WidgetId::SunShadows, "options.rebedrock.sunShadows", "Sun Shadows",
     &config::GameOptions::sunShadows, kOnOffValues},
    {WidgetId::RainCollisionCache, "options.rebedrock.rainCollisionCache",
     "Rain Collision Cache", &config::GameOptions::rainCollisionCache, kOnOffValues},
}};

// id 所指的那个选项，它不是循环选项时返回空
// 不是循环选项的有滑块、页面按钮，以及那三项在 GameOptions 之外的设置
// 之所以是 constexpr，因为 ui/WidgetLabels.hpp 的覆盖性断言要在编译期问这个 id 是不是循环选项
// 这个查找因此必须能在常量求值里跑
[[nodiscard]] constexpr const OptionDesc* findCyclingOption(WidgetId id) {
    for (const OptionDesc& desc : kCyclingOptions) {
        if (desc.id == id) {
            return &desc;
        }
    }
    return nullptr;
}

// ---- 读取、写入与步进 -----------------------------------------------------

[[nodiscard]] inline int readOption(const OptionDesc& desc, const config::GameOptions& options) {
    return std::visit(
        [&options](auto member) { return static_cast<int>(options.*member); }, desc.field);
}

inline void writeOption(const OptionDesc& desc, config::GameOptions& options, int value) {
    std::visit(
        [&options, value](auto member) {
            using Field = std::remove_reference_t<decltype(options.*member)>;
            options.*member = static_cast<Field>(value);
        },
        desc.field);
}

// value 在该选项取值列表中的下标，存储值不在列表里时返回 0
// 那种情况来自手工改过的选项文件，或者某个取值已从列表退役
// 吸附到第一项正是从前那句手写的 find 失败取零所做的事
[[nodiscard]] inline std::size_t optionValueIndex(const OptionDesc& desc, int value) {
    for (std::size_t index = 0; index < desc.values.size(); ++index) {
        if (desc.values[index].value == value) {
            return index;
        }
    }
    return 0;
}

// 把选项步进一格，两端回绕
// direction 取 +1 表示下一个取值，即左键点击，与 vanilla 一致；取 -1 表示上一个
inline void cycleOptionValue(const OptionDesc& desc, config::GameOptions& options,
                             int direction = 1) {
    if (desc.values.empty()) {
        return;
    }
    const std::size_t count = desc.values.size();
    const std::size_t current = optionValueIndex(desc, readOption(desc, options));
    const std::size_t step = direction < 0 ? count - 1U : 1U;
    writeOption(desc, options, desc.values[(current + step) % count].value);
}

// 选项标签里表示取值的那一半，比如 ON、120 fps、16x、最低
// translate(key, fallback) 返回某个键的本地化文本，由调用方提供，这里因此不依赖渲染器的语言表
template <typename Translate>
[[nodiscard]] std::string optionValueLabel(const OptionDesc& desc, int value,
                                           Translate&& translate) {
    if (desc.values.empty()) {
        return {};
    }
    const OptionValue& entry = desc.values[optionValueIndex(desc, value)];
    if (!entry.key.empty()) {
        return translate(entry.key, entry.fallback);
    }
    const std::string number = std::to_string(entry.value);
    if (!desc.numberTemplateKey.empty()) {
        return formatTranslation(translate(desc.numberTemplateKey, desc.numberTemplateFallback),
                                 std::array<std::string_view, 1>{number});
    }
    return number + std::string{desc.numberSuffix};
}

}  // namespace mc::ui
