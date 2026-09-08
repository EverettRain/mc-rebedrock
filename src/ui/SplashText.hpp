#pragma once

// UI-4：主菜单那行斜着的黄字（GUI spec §6.3 的 splash）。
//
// 这里只有**纯逻辑**：从 `texts/splashes.txt` 的内容里选一行、算它的姿态。
// 不读文件、不碰字体、不碰 Vulkan，因此每一条都能被无头断言。
//
// ★ 姿态公式取自 26.1 `SplashRenderer.extractRenderState`，**不是 spec §6.3 的正文**。
//   spec 那里写的锚点 `W/2 + 90` 与 `scale = 1.8 - abs(sin(t/100)*0.1) * 0.5` 都是旧值，
//   已在 UI-2 的落地记录里作为更正 D 登记。真实的是：
//     锚点 (W/2 + 123, 69)，旋转 -PI/9，
//     scale = (1.8 - |sin(2*PI * (ms%1000)/1000)| * 0.1) * 100 / (textWidth + 32)
//     文本在变换空间内**左对齐**于 (-textWidth/2, -8)

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::ui {

// SplashRenderer 的常量，逐条对应源码里的字段。
inline constexpr float kSplashAnchorX = 123.0F;   // WIDTH_OFFSET
inline constexpr float kSplashAnchorY = 69.0F;    // HEIGH_OFFSET（原版就是这个拼写）
inline constexpr float kSplashRotation = -0.34906585F; // TEXT_ANGLE = -PI/9 弧度（-20°）
inline constexpr float kSplashPhaseAmplitude = 0.1F;
inline constexpr float kSplashPhaseBase = 1.8F;
inline constexpr float kSplashWidthPadding = 32.0F;
inline constexpr float kSplashWidthReference = 100.0F;
// 文本在变换空间里的纵向偏移：`textRenderer.accept(LEFT, -textWidth/2, -8, …)`
inline constexpr float kSplashTextOffsetY = -8.0F;

// 把 splashes.txt 的内容切成候选行。
//
// vanilla 的 SplashManager 逐行读、去掉首尾空白、丢掉空行；本作照做，另外丢掉以 `#`
// 开头的行——原版文件里没有注释，但资源包作者可能会写，而一行注释被抽中当 splash
// 显示出来是那种"看着像 bug 但没人知道来源"的问题。
[[nodiscard]] inline std::vector<std::string> parseSplashes(std::string_view contents) {
    std::vector<std::string> lines;
    std::size_t start = 0U;
    while (start <= contents.size()) {
        const std::size_t newline = contents.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? contents.size() : newline;
        std::string_view line = contents.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1U);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1U);
        }
        if (!line.empty() && line.front() != '#') {
            lines.emplace_back(line);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1U;
    }
    return lines;
}

// 选一行。
//
// vanilla 每次进标题屏随机选一次。本作把"选哪一行"做成一个**纯函数**，种子由调用方给：
// 正常运行喂时钟，截图通道喂一个固定值——否则同一条命令行两次运行会拍到不同的字，
// 而"两遍逐字节相同"是那条通道的验收条件（见 [[界面截图通道]]）。
[[nodiscard]] inline std::string_view chooseSplash(const std::vector<std::string>& lines,
                                                   std::uint64_t seed) {
    if (lines.empty()) {
        return {};
    }
    // 一次 splitmix64，够散且不需要拉进一个 RNG 头文件
    std::uint64_t x = seed + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return lines[static_cast<std::size_t>(x % lines.size())];
}

// 脉动缩放。`timeMilliseconds` 是墙钟毫秒，只有它的千分之一周期有意义。
[[nodiscard]] inline float splashScale(float textWidth, std::uint64_t timeMilliseconds) {
    constexpr float kTwoPi = 6.28318530718F;
    const float phase = static_cast<float>(timeMilliseconds % 1000ULL) / 1000.0F;
    const float pulse =
        kSplashPhaseBase - std::fabs(std::sin(phase * kTwoPi)) * kSplashPhaseAmplitude;
    return pulse * kSplashWidthReference / (textWidth + kSplashWidthPadding);
}

} // namespace mc::ui
