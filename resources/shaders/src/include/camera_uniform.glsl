// RN-20f-0b — set 0 / binding 0 的相机 UBO：**声明的单一源**
//
// 这份声明从前手抄在 **13 个** 着色器里，每一份各自截断到自己用得到的长度
// （7 到 19 个字段不等）。std140 下截断本身是合法的——只要前缀逐字段一致，
// 读得到的偏移就是对的。代价全在**改动**上：
//
//   * 加一个字段要在 13 处判断「这一份该不该跟着加」；
//   * 在中间插一个字段，任何一份漏改都是**静默的 UBO 错位**——不报错、不崩溃，
//     只是某个 vec4 从此读到隔壁那一个。`block_cutout.frag` 的抬头就记着这样一次事故
//     （RN-35 加 `lightViewProj[2]` 那轮），而 RN-19 §5-5 记着另一次
//     （`grass_block.vert` 与 `.frag` 声明的长度对不上）。
//
// 冻成一份之后这两件事都不存在了：**所有消费者声明同一个完整的块**。未用的成员不会
// 改变布局（std140 的偏移只由类型与顺序决定），也不会进 SPIR-V 的活跃变量集——
// 截断买到的那点东西本来就不是省下来的字节，是「少写几行」。
//
// ★ 这是光影包作者要看到的第一份稳定契约（RN-20 §5.4-1）。外部包不可能去手抄 13 份，
//   它 `#include` 这一个文件。
//
// ⚠ 与 `VulkanRenderer.cpp` 的 `struct CameraUniform` **逐字段对齐**，由
//   `tests/camera_uniform_contract_test.cpp` 在每次 ctest 里比对名字、类型与顺序。
//   改一边不改另一边会当场变红，而不是等到画面上某个数读串了才发现。
//
// **没有**做的事：按更新频率拆成多个 UBO（RN-20 §5.4-1 设想的
// Globals / Projection / Fog / Lighting / …）。那要动 descriptor set layout、13 处
// binding 与每帧上传逻辑，而它买到的「每帧上传字节下降」是 1 KB × 60 fps ≈ 60 KB/s
// ——一个**没有量过**的收益。先量再改；这一轮只解决「声明有几份」。

#ifndef MC_REBEDROCK_CAMERA_UNIFORM_GLSL
#define MC_REBEDROCK_CAMERA_UNIFORM_GLSL

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
    vec4 pointLights[8];
    vec4 lightColors[8];
    // x = 点光源数量, y = 平滑光照开关, z = 保留位（恒 0，见 RN-19b）,
    // w = 光影包这一位（RN-20f-0：既门控采不采样阴影图，也门控天光走不走直射/散射拆分）
    vec4 lightingSettings;
    vec4 celestialLayers;
    vec4 weatherSettings;
    vec4 fluidAnimationLayers;
    vec4 fluidAnimationFrameCounts;
    vec4 fluidAnimationFrameTimes;
    vec4 fluidAnimationSettings;
    // RN-35：级联的两个光源矩阵（0 = 近段可调框，1 = 远段 128 格框）。
    // 数组而不是两个具名字段：std140 下 mat4 数组的元素间距就是 64 字节，
    // 与两个相邻的 mat4 逐字节相同，而数组让「加一级」是改一个数字
    mat4 lightViewProj[2];
    // RN-4b: appended after lightViewProj so earlier offsets are unchanged.
    vec4 blockAnimationSettings;      // x = active animation count
    vec4 blockAnimations[16];         // x=base layer, y=frame count, z=frame time
} camera;

#endif
