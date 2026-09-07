// RN-11b：世界单位的接收端偏置。直接被 headless C++ 测试编译，测试不另抄公式。
// 0.08 格上限比 RN-11 的 0.30 格收紧；0.005 的底值盖住深度/地形顶点量化。
// PCF 网格各 tap 先沿接收平面修正深度，斜率项只需覆盖硬件双线性的半纹素足迹。
// 半纹素为 0.03125 格，0.03*slope 加 0.005 的底值覆盖常见入射角，最大仍限制在 0.08。
const float kSunShadowMinBiasBlocks = 0.005F;
const float kSunShadowSlopeBiasBlocks = 0.03F;
const float kSunShadowMaxBiasBlocks = 0.08F;
const float kSunShadowMinCosTheta = 0.15F;
float sunShadowBiasBlocks(float incidenceCosine) {
    float cosTheta = clamp(incidenceCosine, 0.0F, 1.0F);
    float slope = sqrt(max(1.0F - cosTheta * cosTheta, 0.0F)) /
        max(cosTheta, kSunShadowMinCosTheta);
    return min(kSunShadowMinBiasBlocks + kSunShadowSlopeBiasBlocks * slope,
               kSunShadowMaxBiasBlocks);
}

const float kSunShadowTexelSizeBlocks = 0.0625F;
// 法线在光源的 right/up/sun 三个正交轴上的分量。正负法线得到相同的平面斜率。
float sunShadowTapOffsetBlocks(float normalRight, float normalUp, float normalSun,
                               float tapX, float tapY) {
    float denominator = max(abs(normalSun), kSunShadowMinCosTheta);
    if (normalSun < 0.0F) denominator = -denominator;
    return kSunShadowTexelSizeBlocks * (normalRight * tapX + normalUp * tapY) / denominator;
}
