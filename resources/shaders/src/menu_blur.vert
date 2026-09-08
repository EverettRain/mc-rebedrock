#version 450

// UI-5：整帧模糊后处理的一趟（26.1 `post_effect/blur.json` 的 `core/screenquad`）。
// 一个覆盖全屏的三角形，没有顶点缓冲。
layout(location = 0) out vec2 texCoord;

const vec2 triangle[3] = vec2[](
    vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0)
);

void main() {
    vec2 ndc = triangle[gl_VertexIndex];
    // Vulkan 的正高度视口把 NDC 的 -1 映到帧缓冲顶部，纹理坐标同向增长，
    // 所以这里**不**做 y 翻转：翻了会让模糊结果上下颠倒地写回去，而模糊后的画面
    // 上下颠倒在一张糊掉的截图上几乎看不出来。
    texCoord = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
