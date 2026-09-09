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

// RN-51/52：薄投射者（玻璃那一层）侧对光到判不出来的程度就不投影了。
//
// 判断本身在 shadow.vert 里做完——它要用**这一级的纹素**（门槛随近段距离那一档变），
// 而纹素是从矩阵推的。这里收到的是结论：0 = 这一面渲进去只会是噪声。

layout(binding = 1) uniform sampler2DArray blockTextures;

void main() {
    if (fragmentCasterFacing < 0.5) {
        discard;
    }
    if (texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer)).a < 0.5) {
        discard;
    }
}
