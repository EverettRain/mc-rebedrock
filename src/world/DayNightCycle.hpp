#pragma once

#include <glm/vec3.hpp>

namespace mc::world {

struct DayNightState final {
    float dayFraction = 0.25F;
    float skyBrightness = 1.0F;
    glm::vec3 sunDirection{0.0F, 1.0F, 0.0F};
    glm::vec3 horizonColor{0.68F, 0.78F, 0.90F};
};

class DayNightCycle final {
  public:
    static constexpr double kTicksPerSecond = 20.0;
    static constexpr double kTicksPerDay = 24'000.0;
    static constexpr double kSecondsPerDay = kTicksPerDay / kTicksPerSecond;
    static constexpr double kNewWorldTick = 6'000.0;

    // 太阳轨道的 z 倾角。stateAtTick 的方向式子里就是这个系数，别在那里再写一遍字面量：
    // 轨道法线（下面那条）必须和它同源，否则谁调了倾角，法线还指着旧平面，而
    // 「光源基零 roll」这条性质会静默失效且没有任何断言会红。
    static constexpr float kSunOrbitTilt = 0.28F;

    // 太阳轨道平面的法线（**未归一化**——glm::lookAt 只用它做 cross 再归一化，长度无关）。
    //
    // sunDirection 是 cos(a)·(0, 1, kSunOrbitTilt) + sin(a)·(-1, 0, 0) 归一化后的结果，
    // 两个基向量互相正交、且都与 (0, kSunOrbitTilt, -1) 正交，所以太阳整天严格落在这一个
    // 平面里：dot(sunDirection, kSunOrbitNormal) 全天不超过 1e-16。
    //
    // 阴影图拿它当光源的 up（RN-24）。用世界的 (0,1,0) 当 up 时，光源基除了跟着太阳转，
    // 还会绕光轴多出一个自旋；那个自旋是唯一在阴影图**平面内**转动纹素网格的分量，物理
    // 上什么也不做，正午附近却能达到太阳自身转速的 3.7 倍。用轨道法线当 up 时 up 恒等于
    // 光源基的 y 轴，自旋恒为 0，而且 |cross(-sun, up)| 恒等于 1（用 (0,1,0) 时正午只有
    // 0.2696，归一化会在那里放大误差）。
    static constexpr glm::vec3 kSunOrbitNormal{0.0F, kSunOrbitTilt, -1.0F};

    [[nodiscard]] static double worldTick(double elapsedSeconds);
    [[nodiscard]] static DayNightState state(double elapsedSeconds);
    [[nodiscard]] static DayNightState stateAtTick(double tick);
};

} // namespace mc::world
