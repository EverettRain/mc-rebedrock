#ifndef INCLUDE_SHADOW_MAP
#define INCLUDE_SHADOW_MAP

// 阴影图本身的几何：分辨率，以及「一个纹素在世界里有多大」。
//
// RN-52：从 sun_shadow.glsl 搬出来，因为**投射端**也要它了——薄投射者要不要投影，
// 判据是它在阴影图里能盖住几个纹素，而那取决于正在用的那一级有多细。
// 采样端与投射端各存一份就是又一处会漂的常量。

// 必须与 SunShadowMap.hpp 的 kSunShadowMapResolution 一致（sun_shadow_map_test 对拍）
const float kSunShadowMapResolution = 2048.0;

// RN-47：一个纹素的世界边长（格），**从正在用的那张矩阵里推出来**，不是常量。
//
// 正交投影的 x 轴缩放正好是 1/半边长，而 lightViewProj = ortho x 旋转，旋转的行是
// 单位向量，所以 length(vec3(m[0][0], m[1][0], m[2][0])) 就是那个缩放。近段框是玩家
// 可调的一档（8/16/24 格），这样推出来的纹素与它所属的矩阵**结构上不可能脱钩**。
float sunShadowTexelBlocksOf(mat4 lightViewProj) {
    float scale = length(vec3(lightViewProj[0][0], lightViewProj[1][0], lightViewProj[2][0]));
    return 2.0 / (scale * kSunShadowMapResolution);
}

#endif // INCLUDE_SHADOW_MAP
