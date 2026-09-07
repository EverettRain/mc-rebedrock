// RN-19d0：把 GPU 时间戳的原始 tick 换成毫秒。
//
// 这段算术全是「不出错就看不出来」的那一类：多算一个数量级、掉进一次回绕、
// 或者在一台 timestampValidBits == 0 的设备上照样返回一个数——三种错误产出的都是
// **看起来很正常的毫秒数**，没有任何错误码，没有任何画面异常。而这些数字接下来会被
// 用来决定「哪条优化值得做」。所以判据必须钉在这里，不能钉在「真机跑一遍看着像」。

#include "render/graph/GpuTimestamps.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using namespace mc::render::graph;

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void checkNear(double actual, double expected, const char* what) {
    if (std::abs(actual - expected) > 1e-9) {
        std::cerr << "FAIL: " << what << " (got " << actual << ", want " << expected << ")\n";
        ++failures;
    }
}

// 一个典型的桌面 GPU：1 tick = 1 ns，36 位有效。
constexpr GpuTimestampScale kNanosecond{1.0, 36};
// AMD 那一类：一个 tick 好几十纳秒。刻度乘错的话这一档立刻现形，1 ns/tick 那档不会。
constexpr GpuTimestampScale kCoarse{40.0, 64};

void testSlotCount() {
    check(gpuTimestampSlotCount(0) == 1U, "零步也要一个点（空图的跨度是 0，不是未定义）");
    check(gpuTimestampSlotCount(1) == 2U, "一步两个点");
    check(gpuTimestampSlotCount(5) == 6U, "生产的五步要六个点");
}

void testUnusableScaleReturnsZero() {
    // timestampValidBits == 0 是**唯一**会告诉你这个队列不支持时间戳的东西：
    // vkCmdWriteTimestamp 照样接受，查询照样"成功"，写出来的是垃圾。
    constexpr GpuTimestampScale noBits{1.0, 0};
    check(!noBits.usable(), "有效位为 0 = 不可用");
    checkNear(noBits.millisecondsBetween(0, 1'000'000), 0.0,
              "不可用时必须返回 0，而不是一个看起来很正常的毫秒数");

    constexpr GpuTimestampScale noPeriod{0.0, 64};
    check(!noPeriod.usable(), "刻度为 0 = 不可用");
    checkNear(noPeriod.millisecondsBetween(0, 1'000'000), 0.0, "刻度为 0 时同样返回 0");

    constexpr GpuTimestampScale absurdBits{1.0, 65};
    check(!absurdBits.usable(), "有效位超过 64 位不是一个可信的驱动答复");

    check(kNanosecond.usable() && kCoarse.usable(), "两个正常刻度都可用");
}

void testMaskAvoidsUndefinedShift() {
    // `1 << 64` 是 UB。64 位这一档必须单独一路，而不是靠移位碰运气。
    constexpr GpuTimestampScale full{1.0, 64};
    check(full.mask() == ~std::uint64_t{0}, "64 位有效 = 全掩码");
    check(kNanosecond.mask() == (std::uint64_t{1} << 36) - 1U, "36 位有效的掩码是 2^36-1");
    constexpr GpuTimestampScale none{1.0, 0};
    check(none.mask() == 0U, "零位的掩码是 0");
}

void testOrdinaryDelta() {
    // 1 ns/tick：1,000,000 tick = 1 ms。掉一个数量级在这里就是 0.1 或 10。
    checkNear(kNanosecond.millisecondsBetween(0, 1'000'000), 1.0, "1e6 个 1ns tick = 1 ms");
    checkNear(kNanosecond.millisecondsBetween(500, 1'000'500), 1.0, "差值与起点无关");
    checkNear(kCoarse.millisecondsBetween(0, 25'000), 1.0, "40 ns/tick：25000 tick = 1 ms");
    checkNear(kNanosecond.millisecondsBetween(7, 7), 0.0, "同一点之差是 0");
}

void testHighBitsAreMasked() {
    // 有效位以上是**未定义**的，驱动爱写什么写什么。先掩再减，否则高位垃圾会把
    // 一个 1 ms 的阶段放大成天文数字——而它同样是「一个数」，没有任何东西会报错。
    constexpr std::uint64_t garbage = std::uint64_t{0xABCD} << 40;
    checkNear(kNanosecond.millisecondsBetween(garbage, garbage + 1'000'000), 1.0,
              "两端带同样的高位垃圾：结果不变");
    checkNear(kNanosecond.millisecondsBetween(0, garbage + 1'000'000), 1.0,
              "只有一端带高位垃圾：也必须被掩掉，而不是产出一个巨大的假数");
}

void testWraparound() {
    // 36 位计数器绕回：end 在数值上小于 begin，但真实间隔很短。
    const std::uint64_t nearTop = kNanosecond.mask() - 1000U;
    checkNear(kNanosecond.millisecondsBetween(nearTop, 1000U - 1U), 2000.0 / 1'000'000.0,
              "跨过回绕点的 2000 个 tick 仍是 2000 个 tick");
    // 回绕在 64 位上同样要成立（无符号减法自然绕，掩码是全 1）。
    checkNear(kCoarse.millisecondsBetween(~std::uint64_t{0} - 9U, 15U), 25.0 * 40.0 / 1'000'000.0,
              "64 位全掩码下的回绕");
}

void testWriterDefaultIsInactive() {
    // 默认值必须是「不计时」。反过来的话，任何忘了初始化的调用点都会去写一个空句柄。
    constexpr GpuTimestampWriter idle{};
    check(!idle.active(), "默认构造的 writer 不计时");
    check(idle.pool == VK_NULL_HANDLE && idle.firstQuery == 0U, "默认值就是空池、零偏移");
}

} // namespace

int main() {
    testSlotCount();
    testUnusableScaleReturnsZero();
    testMaskAvoidsUndefinedShift();
    testOrdinaryDelta();
    testHighBitsAreMasked();
    testWraparound();
    testWriterDefaultIsInactive();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "gpu_timestamp_scale ok\n";
    return 0;
}
