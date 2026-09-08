#version 450

// TAA-1 的 resolve。世界那趟画完之后、界面那趟之前跑一次，逐屏幕像素。
//
// 三个输入：本帧的世界画面（抖动过的采样位置）、本帧的深度（用来把像素反投影回
// 上一帧）、上一帧的 resolve 结果。两个输出：写回 8 位的场景图给界面与呈现链用，
// 同时写一份 16F 的历史给下一帧。
//
// ★ 两个输出附件而不是「写完再拷一份」：拷贝会先把结果量化成 8 位再进历史，
// 于是历史是 16F 这件事一点用都没有——累积几十帧之后的 banding 正是它要躲的。

layout(set = 0, binding = 0) uniform sampler2D sceneColour;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 1, binding = 0) uniform sampler2D history;

layout(push_constant) uniform TemporalPush {
    // 当前帧裁剪空间 → 上一帧裁剪空间。历史无效时是单位阵，配合 settings.x = 0
    mat4 reprojection;
    // x = 历史权重（0 = 这一帧不吃历史），yzw 保留
    vec4 settings;
} push;

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outHistory;

void main() {
    const ivec2 size = textureSize(sceneColour, 0);
    const ivec2 centre = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(sceneColour, centre, 0).rgb;

    // 3x3 邻域的颜色包围盒。texelFetch 而不是 texture()：邻域要的是**整像素**偏移，
    // 双线性会把半个像素的邻居混进来，盒子因此比真实邻域窄，钳制过头 = 边缘闪烁。
    vec3 lowest = current;
    vec3 highest = current;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const ivec2 tap = clamp(centre + ivec2(dx, dy), ivec2(0), size - ivec2(1));
            const vec3 neighbour = texelFetch(sceneColour, tap, 0).rgb;
            lowest = min(lowest, neighbour);
            highest = max(highest, neighbour);
        }
    }

    // 反投影：本像素的深度决定它在世界里的位置，重投影矩阵把那个位置送回上一帧的屏幕。
    // 静态几何靠这一条就够；会动的东西（实体、掉落物、粒子）由下面的钳制兜着，
    // 真正的运动矢量是 TAA-2。
    const float depth = texelFetch(sceneDepth, centre, 0).r;
    const vec4 previousClip = push.reprojection * vec4(texCoord * 2.0 - 1.0, depth, 1.0);

    float weight = push.settings.x;
    vec2 previousUv = vec2(0.0);
    if (previousClip.w > 0.0) {
        previousUv = (previousClip.xy / previousClip.w) * 0.5 + 0.5;
    } else {
        // 上一帧在相机背后。没有历史可用，只能拿本帧顶上
        weight = 0.0;
    }
    // 画面外没有历史。CLAMP_TO_EDGE 会给出边缘像素，那是一条会沿着屏幕边框
    // 涂开的假影，比没有历史更糟
    if (any(lessThan(previousUv, vec2(0.0))) || any(greaterThan(previousUv, vec2(1.0)))) {
        weight = 0.0;
    }

    const vec3 historyColour = clamp(texture(history, previousUv).rgb, lowest, highest);
    const vec3 resolved = mix(current, historyColour, weight);
    outScene = vec4(resolved, 1.0);
    outHistory = vec4(resolved, 1.0);
}
