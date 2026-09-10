#pragma once

// RN-20f-0 — 光影包：把本作自研的光照特性收成一个可以整体开关的实体
//
// 这个类型解决的是一个**口径**问题，不是一个性能或画质问题。
//
// 渲染线走到 RN-55 时，仓库里同时住着两套互相矛盾的判据：
//
//   * 「与 26.1 逐值对齐」——本作很多测试的真相源（黑羊 `#1D1D21`、方块底面 `0x80`、
//     lightmap 的每一个系数），也是「复刻」这件事本身的定义；
//   * 「物理上更对」——RN-11 起的太阳阴影、RN-38 的直射/散射两项、RN-42 的入射角、
//     RN-46a 的水下衰减、RN-46b 的假反射光。这些 vanilla **一样都没有**。
//
// 两套判据从前搅在同一条代码路径上，后果是可量的：即使太阳阴影是**关**的
// （`sunShadows` 默认就是 false），`sunSkyFactor` 仍然无条件地把天光拆成直射与散射
// 并乘上入射角，于是
//
//     正南/正北的墙   全天只有 vanilla 的 42.5%
//     正东/正西的墙   正午只有 vanilla 的 20.1%
//
// ——一个「默认关闭」的特性，把默认画面改掉了近八成。那不是取舍，是漏了一个开关。
//
// 边界的判据只有一条：**vanilla 26.1 有没有**
// ------------------------------------------------------------------
// 有的**不进包**，它们是复刻不是自研：`cardinalShade` 的四向明暗、平滑光照与 AO
// （RN-19c 已逐值对齐）、实体脚下的阴影贴花（vanilla 自己就画）、菜单模糊。
// 没有的**整体进包**：太阳阴影图那一整条，以及天光的直射/散射拆分。
//
// 「整体」是这个类型存在的理由。阴影与直射模型**不是两个特性**：`sunSkyFactor` 消费
// `shadowFactor`，而 `shadowFactor` 只有在天光分了直射项之后才有东西可挡。分开开关
// 会造出「有阴影但天光不分项」这种没有意义的组合——枚举掉一半状态空间比多一个开关贵。
//
// 为什么是编译期常量而不是运行期配置
// ------------------------------------------------------------------
// 20f-0 是**骨架**：它要立的是「哪些东西属于包」这条边界，不是包的加载器。真正的
// 外部包（自研格式，用户裁定不做 Iris/OptiFine 兼容）要等 20f/20g，那时这个结构会
// 长出资源表与 pass 表，来源从 constexpr 变成解析产物。**现在就把它做成运行期可配
// 是过度设计**：今天只有两个包，而且其中一个的全部内容就是「什么都不加」。
//
// 与帧图的关系：`sunShadowPasses` 决定 `shadow_near` / `shadow_far` 两步在不在图里
// （编译期剪枝，不是运行期 if）；`directSkyModel` 决定接收端走哪条天光公式，
// 经 `lightingSettings.w` 一位传给三个采样者。两者同源于这里，不可能各说各话。

#include <string_view>

namespace mc::render {

struct ShaderPack final {
    // 进日志与诊断；执行期不读。
    std::string_view name;

    // 帧图里有没有 shadow_near / shadow_far 两步。
    bool sunShadowPasses = false;

    // 接收端的天光是不是拆成「直射 + 散射」两项（RN-38/42/46a/46b 的整套）。
    // 关掉时 `sunSkyFactor` 退化成 vanilla 的 `SKY_LIGHT_FACTOR x 天气昏暗`。
    bool directSkyModel = false;

    // 两者永远同开同关，理由见抬头。留成两个字段而不是一个 bool，是因为它们作用在
    // **两个不同的层**（帧图的 pass 集合 / 着色器的一条公式），各自的消费点要能
    // 独立读到自己那一位；`consistent()` 是那条不变量的落点。
    [[nodiscard]] constexpr bool enabled() const noexcept { return sunShadowPasses; }
    [[nodiscard]] constexpr bool consistent() const noexcept {
        return sunShadowPasses == directSkyModel;
    }
};

// 不开包：天光就是 vanilla 的 SKY_LIGHT_FACTOR x 天气昏暗，与 26.1 逐值对齐。
// 这是**验证基线**——既有的那批「逐值对齐」测试对的就是这一条路径。
inline constexpr ShaderPack kNoShaderPack{.name = "none"};

// 本作自研的内置包。名字里带 rebedrock 是因为将来会有外部包，日志要分得出来。
inline constexpr ShaderPack kBuiltinShaderPack{
    .name = "rebedrock-builtin", .sunShadowPasses = true, .directSkyModel = true};

} // namespace mc::render
