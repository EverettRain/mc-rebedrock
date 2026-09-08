#version 450

// TAA-1 的 resolve 那一趟：一个覆盖全屏的三角形，没有顶点缓冲。
// 形状与理由同 menu_blur.vert，各写一份是因为两趟的输出附件数不同
// （模糊是一个颜色附件，resolve 是两个：8 位的场景图 + 16F 的历史）。
layout(location = 0) out vec2 texCoord;

const vec2 triangle[3] = vec2[](
    vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0)
);

void main() {
    vec2 ndc = triangle[gl_VertexIndex];
    // 不做 y 翻转：本作的投影是 perspectiveRH_ZO 之后 projection[1][1] *= -1，
    // NDC 的 +y 已经与帧缓冲的 +y 同向。片元着色器把 texCoord 反算回 NDC
    // 去做重投影，翻一次就等于让历史往上下颠倒的方向找。
    texCoord = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
