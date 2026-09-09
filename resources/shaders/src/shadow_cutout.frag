#version 450

// 镂空地形在太阳空间的深度：几何与 shadow.frag 那一条完全一样，只是先按图集的
// alpha 把空像素丢掉。少了这一步，树叶会按整块方块投影，一棵树在地上是一个方块阴影
// 而不是一片斑驳；栅栏、门、草会投出它们并不存在的实心矩形。
//
// 阈值取 **0.5**，与主通道的 `block_cutout.frag:77` 一致（实体那条走 0.1，因为它对的是
// `item_entity.frag`）：影子的轮廓必须与看得见的轮廓是同一条线，两处取不同的阈值就会
// 在树叶边缘露出一圈投了影却看不见的像素。
//
// 这里读的是顶点带来的**基础**层号，不做主通道那套动画帧解析：图集的动画帧区只给流体
// （见方块图集的约束），而流体是半透明层，压根不进这条通道。
layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 2) flat in float fragmentCasterFacing;

// RN-51：薄投射者（玻璃那一层）侧对光到这个程度就不投影了。
//
// 玻璃边框宽 1/16 格，挡光的投影宽度是 1/16 x |N·L|。取 0.25 ⇒ 至少 1/64 格，
// 也就是默认档近段纹素（1/128 格）的两倍——刚好够被采样到。低于它渲出来的是噪声：
// 一条随纹素中心通断的发丝影。非薄投射者的这一档恒为 1，这条判断对它们不存在。
const float kThinCasterMinFacing = 0.25;

layout(binding = 1) uniform sampler2DArray blockTextures;

void main() {
    if (fragmentCasterFacing < kThinCasterMinFacing) {
        discard;
    }
    if (texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer)).a < 0.5) {
        discard;
    }
}
