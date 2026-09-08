#pragma once

// TAA-1：时间性抗锯齿的**纯算术**。
//
// 这个头里没有一个 Vulkan 句柄。抖动序列、抖动怎么进投影、重投影矩阵怎么拼、
// 邻域钳制的判据——四样东西全是可以在无头测试里逐值断言的函数，而它们正是
// TAA 唯一能被静默做错的部分：一趟全屏 resolve 只要跑起来，画面就永远"看着像
// 抗锯齿"，抖动方向错一个符号、重投影少乘一个逆，症状都只是"有点糊"。
// 「判据不能只是看起来不抖」那条验收（TAA README）落在这里。
//
// 与 SunShadowMap 的分法相同：几何与判据是纯函数，GPU 对象在 render/vulkan/。

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cstdint>

namespace mc::render {

// 抖动相位数。8 是 Halton(2,3) 在这个用途上的常见长度：够长到把一个像素铺满，
// 又短到相机静止时历史在 8 帧内收敛而不是永远慢慢爬。
inline constexpr std::uint32_t kTemporalJitterPhaseCount = 8U;

// 历史的混合权重。0.9 = 每帧只有 10% 是新样本，等效 ~10 帧的指数窗口。
// 再高会把拖影拖长（TAA-2 之前没有运动矢量兜底），再低则收敛不到亚像素。
inline constexpr float kTemporalHistoryWeight = 0.9F;

// Halton 低差异序列的第 index 项（index 从 1 起）。
// 用它而不是随机数：随机抖动在任意有限窗口里都会聚簇，留下一块块没被采到的
// 亚像素位置，画面因此在"抖"与"糊"之间摇摆。
[[nodiscard]] constexpr float haltonSequence(std::uint32_t index, std::uint32_t base) {
    float fraction = 1.0F;
    float result = 0.0F;
    while (index > 0U) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

// 第 frameIndex 帧的抖动，单位是**像素**，范围 (-0.5, 0.5]。
// 相位对帧号取模，所以相机静止时这 8 个样本无限循环，历史因此稳定在同一个平均值上，
// 而不是每帧引入一个新的采样位置（那会让静止画面持续微微蠕动）。
[[nodiscard]] constexpr glm::vec2 temporalJitterOffset(std::uint64_t frameIndex) {
    const auto phase = static_cast<std::uint32_t>(frameIndex % kTemporalJitterPhaseCount) + 1U;
    return {haltonSequence(phase, 2U) - 0.5F, haltonSequence(phase, 3U) - 0.5F};
}

// 把亚像素抖动加进一个**已经建好的**投影矩阵。
//
// ★ 加在第三列，不是加在平移那一列：后者会平移整个视锥，近处与远处的位移量不同——
// 那是相机动了，不是采样位置动了。要的效果恰好是 `clip.xy += offset · clip.w`，
// 对任何深度都一样。
//
// ★ **减**不是加。第三列乘的是视空间的 z，而右手系投影的 `clip.w = -z`：
// 于是 `m[2][0] += k` 给出的增量是 `k·z = -k·w`，方向正好反了。这一个符号在画面上
// 的表现只是"稍微软一点"——抖动仍在，只是每一帧都朝着与重投影相反的方向偏。
// jittered_projection 的数值断言就是为它写的。
//
// 单位换算：一个像素 = 2/宽 的 NDC，因为 NDC 横跨 [-1, 1]。
//
// y 不额外取反。本作的投影是 `perspectiveRH_ZO` 之后 `projection[1][1] *= -1`，
// NDC 的 +y 因此已经与帧缓冲的 +y（向下）同向，也与着色器里 `uv = ndc * 0.5 + 0.5`
// 同向——三处必须是同一个方向，否则重投影会把历史往反方向找。
[[nodiscard]] inline glm::mat4 jitteredProjection(const glm::mat4& projection,
                                                  glm::vec2 offsetPixels, float width,
                                                  float height) {
    glm::mat4 jittered = projection;
    jittered[2][0] -= 2.0F * offsetPixels.x / width;
    jittered[2][1] -= 2.0F * offsetPixels.y / height;
    return jittered;
}

// 重投影矩阵：把**当前帧**的裁剪空间坐标送到**上一帧**的裁剪空间。
//
//     prevClip = previousViewProjection · T(cameraDelta) · currentViewProjection⁻¹ · curClip
//
// 两个 view-projection 都必须是「相机在原点」的那一版（旋转 + 投影，不含平移），
// 相机位移单独走 cameraDelta = 当前眼点 - 上一帧眼点。
// ★ 不要把绝对世界坐标直接塞进矩阵：世界坐标可以到 ±3000 万，float 在那里的
// 分辨率比一个像素对应的深度差还粗，重投影会在远离原点的地方整体错位。
//
// 传进来的矩阵必须是**未抖动**的。用抖动过的那份，重投影会把两帧各自的亚像素
// 偏移一起算进去，等于把抖动又抵消掉一次——画面回到没有 TAA 的样子，而且没人看得出来。
[[nodiscard]] inline glm::mat4 temporalReprojection(const glm::mat4& previousViewProjection,
                                                    const glm::mat4& currentViewProjection,
                                                    glm::vec3 cameraDelta) {
    glm::mat4 translation{1.0F};
    translation[3] = glm::vec4{cameraDelta, 1.0F};
    return previousViewProjection * translation * glm::inverse(currentViewProjection);
}

// 邻域钳制：把历史色夹进当前帧 3×3 邻域的颜色包围盒。
//
// 这是 TAA-1 唯一的拖影抑制手段（运动矢量是 TAA-2）。判据是"历史色如果落在
// 当前帧邻域能产生的颜色范围之外，它描述的就是别的东西"——遮挡关系变了、
// 物体移走了。夹住而不是丢弃：夹到盒边仍保留了历史里那一份亚像素信息。
[[nodiscard]] inline glm::vec3 clampToNeighbourhood(glm::vec3 history, glm::vec3 minimum,
                                                    glm::vec3 maximum) {
    return {std::clamp(history.x, minimum.x, maximum.x),
            std::clamp(history.y, minimum.y, maximum.y),
            std::clamp(history.z, minimum.z, maximum.z)};
}

// 逐帧的时间性状态。一帧写、下一帧读，所以它必须活在渲染器上而不是某个 body 的栈上。
//
// `valid` 从 false 起步，并在**每一次**交换链重建时回到 false：历史图那时被销毁重建，
// 而一张刚分配的图里是垃圾。少了这条，改一次窗口大小就会有一帧显示随机内存。
struct TemporalFrameState final {
    glm::mat4 previousViewProjection{1.0F};
    glm::vec3 previousCameraPosition{0.0F};
    // 本帧的（未抖动、相机在原点的）view-projection 与眼点，由 updateUniform 写入，
    // resolve 那一步读走，帧末覆盖成 previous
    glm::mat4 currentViewProjection{1.0F};
    glm::vec3 currentCameraPosition{0.0F};
    // 本帧写哪一张历史。两张真正的 ping-pong 靶，逐帧翻转，读的永远是另一张。
    //
    // ★ 曾经试过蹭交换链图像那一维（写 history[imageIndex]、读 history[上一帧的
    // imageIndex]），它省掉了这个字段，但**不成立**：vkAcquireNextImageKHR 并不保证
    // 每帧给出不同的下标。隐藏窗口的离屏导出上它实测恒为同一个值，于是同一张图像
    // 在同一趟里既是颜色附件又被采样——校验层报 imageLayout-00344，真机上是未定义行为。
    // "先看看它实际会不会重复"是不够的：这里要的是一个结构上不可能重合的下标。
    std::uint32_t historyWriteSlot = 0;
    bool valid = false;

    void invalidate() {
        valid = false;
        historyWriteSlot = 0;
    }

    // 本帧采样哪一张。与写的那张互斥，这是结构性的而不是运行期检查出来的
    [[nodiscard]] std::uint32_t historyReadSlot() const { return historyWriteSlot ^ 1U; }

    // 本帧的重投影矩阵。历史无效时返回单位阵——它不会被采用（权重是 0），
    // 但返回一个未初始化的矩阵会让 NaN 顺着 push constant 流进着色器。
    [[nodiscard]] glm::mat4 reprojection() const {
        if (!valid) {
            return glm::mat4{1.0F};
        }
        return temporalReprojection(previousViewProjection, currentViewProjection,
                                    currentCameraPosition - previousCameraPosition);
    }

    // 历史的权重。无效历史一律 0——这一条同时是"第一帧"和"刚重建交换链"的答案。
    [[nodiscard]] float historyWeight() const {
        return valid ? kTemporalHistoryWeight : 0.0F;
    }

    void advance() {
        previousViewProjection = currentViewProjection;
        previousCameraPosition = currentCameraPosition;
        historyWriteSlot ^= 1U;
        valid = true;
    }
};

} // namespace mc::render
