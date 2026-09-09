#version 450

// The shadow pre-pass is depth-only (no color attachment); the rasterizer
// writes depth from the vertex positions and this trivial fragment keeps the
// pipeline on the well-trodden fragment-shader path across drivers.
//
// 这两个输入声明了但不用。shadow.vert 与镂空那条管线共用，它无条件写出图集坐标；
// 这里不把它们接住，校验层就会对**每一条**不透明地形管线报
// UNASSIGNED-CoreValidation-Shader-OutputNotConsumed。声明它们比为了一条深度通道
// 再复制一份顶点位置的算术便宜——两份位置算术漂移，影子就会与投影它的方块错开。
layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
// RN-51 新加的第三个：薄投射者正对光的程度。不透明地形不是薄投射者（那一档恒为 1），
// 这条通道也就用不上它——但同样得声明，理由与上面两个一模一样。
layout(location = 2) flat in float fragmentCasterFacing;

void main() {}
