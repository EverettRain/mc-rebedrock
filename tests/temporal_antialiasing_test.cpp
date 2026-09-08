// TAA-1 的判据。
//
// 「判据不能只是看起来不抖」（TAA README 的验收口径）：一趟全屏 resolve 只要跑起来，
// 画面就永远"看着像抗锯齿"。抖动方向错一个符号、重投影少乘一个逆、历史靶忘了翻面，
// 三种缺陷的现场表现都是"有点糊"，而且都能通过任何一次肉眼检查。
//
// 这里钉四样：
//   ① 抖动序列的黄金值（Halton 前 8 项）与它的周期性
//   ② 抖动只进相机投影 —— 数值上（它对裁剪坐标做了什么）与结构上（源码里只出现在
//      updateUniform 里，阴影那一路一次都没有）
//   ③ 重投影的往返一致性：相机没动时它必须是恒等，动了则必须把点送回正确的位置
//   ④ 邻域钳制，以及历史两张靶的读写互斥

#include "render/TemporalAntiAliasing.hpp"
#include "ui/OptionCycle.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif
#ifndef MC_REBEDROCK_SUN_SHADOW_SRC
#error "MC_REBEDROCK_SUN_SHADOW_SRC must point at src/render/SunShadowMap.cpp"
#endif

namespace {

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

void requireNear(float actual, float expected, float tolerance, std::string_view what) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << what << " — 期望 " << expected << "，实际 " << actual << "\n";
        ++failures;
    }
}

[[nodiscard]] std::string readFile(const char* path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAILED: cannot read " << path << "\n";
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + 1U)) {
        ++count;
    }
    return count;
}

// ---- ① 抖动序列 -----------------------------------------------------------

void testJitterSequence() {
    using namespace mc::render;
    // Halton(2) 与 Halton(3) 的前 8 项，手算：
    //   base 2: 1/2 1/4 3/4 1/8 5/8 3/8 7/8 1/16
    //   base 3: 1/3 2/3 1/9 4/9 7/9 2/9 5/9 8/9
    // 这是黄金值，不是"跑一遍抄下来"的回归值——两条序列都可以在纸上算出来
    const float expectedX[]{0.5F,   0.25F,  0.75F,  0.125F,
                            0.625F, 0.375F, 0.875F, 0.0625F};
    const float expectedY[]{1.0F / 3.0F, 2.0F / 3.0F, 1.0F / 9.0F, 4.0F / 9.0F,
                            7.0F / 9.0F, 2.0F / 9.0F, 5.0F / 9.0F, 8.0F / 9.0F};
    for (std::uint32_t index = 0; index < 8U; ++index) {
        requireNear(haltonSequence(index + 1U, 2U), expectedX[index], 1e-6F, "Halton(2) 的黄金值");
        requireNear(haltonSequence(index + 1U, 3U), expectedY[index], 1e-6F, "Halton(3) 的黄金值");
        const glm::vec2 offset = temporalJitterOffset(index);
        // 抖动是**居中**的：序列本身在 [0,1)，减 0.5 之后才是"像素中心附近"。
        // 忘了减，整幅画面会朝右下平移半个像素，而那看起来只是"稍微软了一点"
        requireNear(offset.x, expectedX[index] - 0.5F, 1e-6F, "抖动的 x 是居中过的 Halton(2)");
        requireNear(offset.y, expectedY[index] - 0.5F, 1e-6F, "抖动的 y 是居中过的 Halton(3)");
        require(std::fabs(offset.x) <= 0.5F && std::fabs(offset.y) <= 0.5F,
                "抖动必须落在一个像素之内");
    }
    // 周期性：相机静止时历史要收敛到一个固定的平均值，那要求样本集合是有限且循环的
    for (std::uint64_t frame = 0; frame < 3U * kTemporalJitterPhaseCount; ++frame) {
        const glm::vec2 a = temporalJitterOffset(frame);
        const glm::vec2 b = temporalJitterOffset(frame + kTemporalJitterPhaseCount);
        require(a == b, "抖动序列必须以 kTemporalJitterPhaseCount 为周期");
    }
    // 8 个样本必须真的是 8 个不同的位置。一个退化成常数的序列同样"周期正确"，
    // 而它等于完全没有抖动
    std::size_t distinct = 0;
    for (std::uint64_t frame = 0; frame < kTemporalJitterPhaseCount; ++frame) {
        bool seen = false;
        for (std::uint64_t earlier = 0; earlier < frame; ++earlier) {
            seen = seen || temporalJitterOffset(earlier) == temporalJitterOffset(frame);
        }
        distinct += seen ? 0U : 1U;
    }
    require(distinct == kTemporalJitterPhaseCount, "一个相位周期内的抖动必须两两不同");
}

// ---- ② 抖动怎么进投影 -----------------------------------------------------

[[nodiscard]] glm::mat4 makeProjection() {
    // 与 PerspectiveCamera::projectionMatrix 同一个形状（RH_ZO + y 取反）
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(70.0F), 16.0F / 9.0F, 0.1F, 256.0F);
    projection[1][1] *= -1.0F;
    return projection;
}

void testJitteredProjection() {
    using namespace mc::render;
    constexpr float kWidth = 1920.0F;
    constexpr float kHeight = 1080.0F;
    const glm::mat4 projection = makeProjection();
    const glm::vec2 offset{0.25F, -0.375F};
    const glm::mat4 jittered = jitteredProjection(projection, offset, kWidth, kHeight);

    // 抖动对一个点做的事必须**恰好**是 clip.xy += offset(以 NDC 计) * clip.w，
    // 对任何深度都一样。这是"平移采样位置"与"移动相机"的区别：后者的位移随深度变化
    for (const float depth : {-1.0F, -8.0F, -120.0F}) {
        const glm::vec4 viewSpace{1.5F, -2.25F, depth, 1.0F};
        const glm::vec4 plain = projection * viewSpace;
        const glm::vec4 shifted = jittered * viewSpace;
        requireNear(shifted.x - plain.x, 2.0F * offset.x / kWidth * plain.w, 1e-5F,
                    "抖动对裁剪坐标 x 的作用必须是 offset 像素");
        requireNear(shifted.y - plain.y, 2.0F * offset.y / kHeight * plain.w, 1e-5F,
                    "抖动对裁剪坐标 y 的作用必须是 offset 像素");
        requireNear(shifted.z, plain.z, 1e-5F, "抖动不得动深度");
        requireNear(shifted.w, plain.w, 1e-5F, "抖动不得动 w");
        // 换算到屏幕像素：这才是"半个像素"这句话的意思
        const glm::vec2 plainPixels{plain.x / plain.w * 0.5F * kWidth,
                                    plain.y / plain.w * 0.5F * kHeight};
        const glm::vec2 shiftedPixels{shifted.x / shifted.w * 0.5F * kWidth,
                                      shifted.y / shifted.w * 0.5F * kHeight};
        requireNear(shiftedPixels.x - plainPixels.x, offset.x, 1e-3F, "屏幕位移就是 offset.x 像素");
        requireNear(shiftedPixels.y - plainPixels.y, offset.y, 1e-3F, "屏幕位移就是 offset.y 像素");
    }
    // 零抖动是恒等。相位表里有一项恰好是 0（Halton 的第一项减 0.5），
    // 那一帧的画面必须与完全没有 TAA 时逐位相同
    require(jitteredProjection(projection, {0.0F, 0.0F}, kWidth, kHeight) == projection,
            "零抖动必须原样返回投影矩阵");
}

// 结构护栏：抖动只准出现在世界那一趟的相机投影上。
//
// 抖阴影矩阵会毁掉 RN-24 的角度量化与纹素吸附（影子每帧自己抖一下，而那正是 TAA
// 要压的东西）；抖剔除视锥会让边缘的 section 每帧进出一次。两者都不会崩，
// 也都不会被任何一张截图抓住。
void testJitterIsCameraOnly() {
    const std::string renderer = readFile(MC_REBEDROCK_RENDERER_SRC);
    const std::string sunShadow = readFile(MC_REBEDROCK_SUN_SHADOW_SRC);
    require(countOccurrences(renderer, "jitteredProjection(") == 1U,
            "jitteredProjection 在渲染器里只准出现一次——多一处就是又一条抖动进了别的矩阵");
    require(countOccurrences(sunShadow, "jitteredProjection") == 0U,
            "太阳阴影的矩阵不得被抖动：那会毁掉纹素吸附，而症状正好是 TAA 要压的抖");
    const auto call = renderer.find("jitteredProjection(");
    const auto updateUniform = renderer.find("void updateUniform(");
    require(updateUniform != std::string::npos && call != std::string::npos,
            "必须找得到 updateUniform 与那一次调用");
    if (updateUniform != std::string::npos && call != std::string::npos) {
        // 那一次调用必须落在 updateUniform 的函数体里，而不是别的什么地方
        const auto nextFunction = renderer.find("\n    void ", updateUniform + 1U);
        require(call > updateUniform && (nextFunction == std::string::npos || call < nextFunction),
                "抖动必须加在 updateUniform 里——那是本帧相机投影的唯一产生点");
    }
}

// ---- ③ 重投影 -------------------------------------------------------------

void testReprojection() {
    using namespace mc::render;
    const glm::mat4 projection = makeProjection();
    const glm::mat4 view =
        glm::lookAt(glm::vec3{0.0F}, glm::vec3{0.3F, -0.2F, -1.0F}, glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::mat4 viewProjection = projection * view;

    // 相机没动：重投影必须是恒等。这是最容易被做错又最不容易被看见的一条——
    // 一个整体偏移半个像素的重投影，静止画面看起来只是"稍微软"
    const glm::mat4 still = temporalReprojection(viewProjection, viewProjection, glm::vec3{0.0F});
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            requireNear(still[column][row] / still[3][3], glm::mat4{1.0F}[column][row], 1e-4F,
                        "相机静止时重投影必须是恒等");
        }
    }

    // 相机动了：拿一个已知的世界点，正着算一遍上一帧的屏幕位置，再让重投影算一遍，
    // 两者必须落在同一处。这条同时钉住了"位移方向"——符号反了，画面会朝相反方向拖影
    const glm::vec3 previousEye{4.0F, 65.0F, -12.0F};
    const glm::vec3 currentEye{4.75F, 65.25F, -13.5F};
    const glm::mat4 previousView =
        glm::lookAt(glm::vec3{0.0F}, glm::vec3{0.1F, 0.05F, -1.0F}, glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::mat4 previousViewProjection = projection * previousView;
    const glm::mat4 reprojection = temporalReprojection(previousViewProjection, viewProjection,
                                                        currentEye - previousEye);
    const glm::vec3 worldPoint{9.0F, 63.5F, -30.0F};
    const glm::vec4 currentClip = viewProjection * glm::vec4{worldPoint - currentEye, 1.0F};
    const glm::vec4 expectedPreviousClip =
        previousViewProjection * glm::vec4{worldPoint - previousEye, 1.0F};
    const glm::vec4 actualPreviousClip = reprojection * currentClip;
    const glm::vec2 expectedNdc{expectedPreviousClip.x / expectedPreviousClip.w,
                                expectedPreviousClip.y / expectedPreviousClip.w};
    const glm::vec2 actualNdc{actualPreviousClip.x / actualPreviousClip.w,
                              actualPreviousClip.y / actualPreviousClip.w};
    requireNear(actualNdc.x, expectedNdc.x, 1e-4F, "重投影必须把点送回上一帧的 x");
    requireNear(actualNdc.y, expectedNdc.y, 1e-4F, "重投影必须把点送回上一帧的 y");
    // 相机确实动了，所以这个点在两帧的屏幕上不该是同一处——否则上面那条会被一个
    // 恒等的重投影"通过"
    require(std::fabs(expectedNdc.x - currentClip.x / currentClip.w) > 1e-3F,
            "夹具必须让这个点在两帧之间真的移动过，否则上面那条断言是空的");
}

// ---- ④ 钳制与历史靶 -------------------------------------------------------

void testNeighbourhoodClamp() {
    using namespace mc::render;
    const glm::vec3 low{0.2F, 0.1F, 0.4F};
    const glm::vec3 high{0.6F, 0.5F, 0.9F};
    // 盒内不动：钳制不得改写本来就合理的历史，否则 TAA 攒的亚像素信息每帧被削一次
    const glm::vec3 inside{0.3F, 0.45F, 0.5F};
    require(clampToNeighbourhood(inside, low, high) == inside, "盒内的历史色必须原样保留");
    // 盒外逐通道夹住。逐通道而不是整体缩放：一个通道越界不代表另外两个也该被动
    const glm::vec3 outside{0.9F, -0.2F, 0.5F};
    const glm::vec3 clamped = clampToNeighbourhood(outside, low, high);
    requireNear(clamped.x, high.x, 1e-6F, "越上界的通道夹到上界");
    requireNear(clamped.y, low.y, 1e-6F, "越下界的通道夹到下界");
    requireNear(clamped.z, outside.z, 1e-6F, "没越界的通道不动");
}

void testFrameState() {
    using namespace mc::render;
    TemporalFrameState state;
    // 没有上一帧 ⇒ 权重 0 且重投影是恒等。这一条同时是"第一帧"和"刚重建交换链"的答案
    require(!state.valid, "初始状态没有历史");
    requireNear(state.historyWeight(), 0.0F, 1e-6F, "没有历史时权重必须是 0");
    require(state.reprojection() == glm::mat4{1.0F}, "没有历史时重投影必须是恒等而不是垃圾");

    const std::uint32_t firstWrite = state.historyWriteSlot;
    require(state.historyReadSlot() != firstWrite, "读写两张历史靶必须互斥");
    state.advance();
    require(state.valid, "推进一帧之后历史有效");
    requireNear(state.historyWeight(), kTemporalHistoryWeight, 1e-6F, "有历史时权重是那个常数");
    require(state.historyWriteSlot != firstWrite, "历史靶必须逐帧翻面");
    require(state.historyReadSlot() == firstWrite, "本帧读的正是上一帧写的那张");
    // 翻面两次回到原处，而且每一帧读写都互斥——「结构上不可能重合」这句话的全部内容
    for (int frame = 0; frame < 8; ++frame) {
        require(state.historyReadSlot() != state.historyWriteSlot, "读写靶在任何一帧都互斥");
        state.advance();
    }
    state.invalidate();
    requireNear(state.historyWeight(), 0.0F, 1e-6F, "交换链重建之后历史必须失效");
}

// ---- 抗锯齿这一档是三态 ---------------------------------------------------

void testAntiAliasingIsThreeWay() {
    const mc::ui::OptionDesc* desc = mc::ui::findCyclingOption(mc::ui::WidgetId::AntiAliasing);
    require(desc != nullptr, "抗锯齿必须还是一个循环选项");
    if (desc == nullptr) {
        return;
    }
    require(desc->values.size() == 3U, "抗锯齿是三档：关 / MSAA / TAA");
    bool hasOff = false;
    bool hasMsaa = false;
    bool hasTaa = false;
    for (const mc::ui::OptionValue& value : desc->values) {
        hasOff = hasOff || value.value == static_cast<int>(mc::config::AntiAliasingMode::Off);
        hasMsaa = hasMsaa || value.value == static_cast<int>(mc::config::AntiAliasingMode::Msaa);
        hasTaa = hasTaa || value.value == static_cast<int>(mc::config::AntiAliasingMode::Taa);
    }
    require(hasOff && hasMsaa && hasTaa, "三档都必须在取值表里，步进才走得到 TAA");
    // MSAA 与 TAA 同时开是纯浪费，做成一行三态就是为了让那个组合**不可表达**。
    // 两个独立开关会让它可表达，而界面上没有任何东西说得清它意味着什么
    mc::config::GameOptions options;
    options.antiAliasing = mc::config::AntiAliasingMode::Taa;
    require(options.antiAliasing != mc::config::AntiAliasingMode::Msaa,
            "同一个字段的两个取值天然互斥");
}

} // namespace

int main() {
    testJitterSequence();
    testJitteredProjection();
    testJitterIsCameraOnly();
    testReprojection();
    testNeighbourhoodClamp();
    testFrameState();
    testAntiAliasingIsThreeWay();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "temporal_antialiasing ok\n";
    return 0;
}
