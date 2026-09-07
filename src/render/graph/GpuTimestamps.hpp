#pragma once

// RN-19d0 — GPU 侧的阶段计时，作为 frame graph 的一个属性
//
// 为什么这是一个独立节点而不是某条优化的附属品：RN-19 §4 把「拿到 GPU 侧分布之前
// 不改代码」写成了纪律，而在这之前全仓 `vkCmdWriteTimestamp` 命中数是 **0**，
// FrameTrace 的每一项都是 CPU 墙钟。于是 §4 那张表里九条优化的「量级」全都标着
// 「代码形态推理，不是实测」——纪律有了，仪器没有。RN-19b 恰恰是靠「先量后决」
// 推翻了一整档（Standard 比 High 慢），这个节点要做的就是把那种判断变成常备能力。
//
// 为什么落在 graph 上：RN-20a 之后，「这一帧有哪些阶段」的答案只有一处——编译出来的
// 那张扁平步表。计时按步边界打点，于是阶段集合与执行集合天然同源，不会出现
// 「加了一趟 pass 却忘了给它加计时」。N 个步骤打 N+1 个点，相邻两点之差就是一步。
//
// 本文件只放**算术与布局**，不调任何 vk* 入口：刻度换算、有效位掩码与回绕、槽位数。
// 那些全是纯值计算，因此可以在 headless 测试里钉死，而不是靠真机跑一遍看数字像不像。

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace mc::render::graph {

// N 个步骤要 N+1 个时间戳：每个步骤**开始前**一个，全部结束后再补一个。
// 边界打点而不是「每步一对」，是因为相邻两步之间没有空隙——后者会把同一个时刻
// 写两遍，白费一半的查询槽，也给「两步之间的时间去哪了」留下一个假问题。
[[nodiscard]] constexpr std::uint32_t gpuTimestampSlotCount(std::size_t stepCount) {
    return static_cast<std::uint32_t>(stepCount) + 1U;
}

// 时间戳刻度：把两次 `vkCmdWriteTimestamp` 之间的原始 tick 差换成毫秒。
//
// 两个输入都来自驱动，都可能让结果毫无意义，所以它们一起构成 `usable()`：
//   * `nanosecondsPerTick` = `VkPhysicalDeviceLimits::timestampPeriod`
//   * `validBits`          = 该队列族的 `VkQueueFamilyProperties::timestampValidBits`
//     0 表示这个队列根本不支持时间戳；此时查询池写出来的是垃圾而不是错误——
//     **不会有任何 Vulkan 错误码告诉你这件事**，只有这个字段会。
struct GpuTimestampScale final {
    double nanosecondsPerTick = 0.0;
    std::uint32_t validBits = 0;

    [[nodiscard]] constexpr bool usable() const {
        return validBits != 0U && validBits <= 64U && nanosecondsPerTick > 0.0;
    }

    // 高位是未定义的，必须先掩掉再比较。`1 << 64` 是 UB，所以 64 位单独一路。
    [[nodiscard]] constexpr std::uint64_t mask() const {
        if (validBits >= 64U) {
            return ~std::uint64_t{0};
        }
        if (validBits == 0U) {
            return 0U;
        }
        return (std::uint64_t{1} << validBits) - 1U;
    }

    // 两点之差，毫秒。
    //
    // 计数器会回绕：掩过之后 `end` 可能小于 `begin`。无符号减法在掩码内自然绕回，
    // 所以这里不需要分支——但**需要先掩**，否则高位垃圾会把差值放大到荒谬。
    // 一帧的跨度远小于一次回绕周期（36 位、1 ns/tick 也有 68 秒），因此
    // 「绕了一圈以上」不在可表达范围内，也不该被当成可恢复情况。
    [[nodiscard]] constexpr double millisecondsBetween(std::uint64_t begin,
                                                       std::uint64_t end) const {
        if (!usable()) {
            return 0.0;
        }
        const std::uint64_t ticks = (end - begin) & mask();
        return static_cast<double>(ticks) * nanosecondsPerTick / 1'000'000.0;
    }
};

// 执行期交给 `BakedGraph::execute` 的计时目标。
//
// 默认值（空句柄）= 不计时，`execute()` 因此一个 vk 入口都不多调。诊断默认关闭时
// 热路径必须与没有这个特性时逐条指令相同——一个常驻但默认关闭的仪器，代价要真的是零。
struct GpuTimestampWriter final {
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t firstQuery = 0;

    [[nodiscard]] bool active() const { return pool != VK_NULL_HANDLE; }
};

} // namespace mc::render::graph
