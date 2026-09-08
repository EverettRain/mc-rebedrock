// RN-11b：世界单位的接收端偏置。直接被 headless C++ 测试编译，测试不另抄公式。
//
// RN-33：随入射角变化的那一项**从深度轴搬到了法线轴**。
//
// 要躲的毛病是同一个：阴影图是 1/16 格一个的网格，一块平地上的像素去查表，查到的那个
// 纹素记的是「稍微偏一点的位置」的深度，可能比它自己还近一丁点，于是它把自己判成阴影
// ——整片地面长出一层斑马纹（acne）。
//
// 从前的解法是**朝太阳压深度**：比较时假装自己近一点。它的代价恰好落在投射者脚下——
// 那里地面与方块之间的真实深度差本来就趋近于 0——一压就被吃掉，影子从方块脚下缩回去，
// 露出一条亮边（peter-panning）。实测这条边值 12/255（该夹具满对比度 55）。
//
// 现在的解法是**沿表面法线抬离表面**：同样把采样点挪到别的纹素上去，但挪的方向不是
// 光线方向，所以脚下那一格仍然被挡住。代价换成了影子**远端**的边缘缩短抬高那么一点，
// 远不如亮边扎眼。
//
// 深度轴因此只剩一个常数底值，盖住深度图与 16 位打包顶点坐标的量化（一格 3855 档）。
const float kSunShadowDepthBiasBlocks = 0.005F;

const float kSunShadowTexelSizeBlocks = 0.0625F;
const float kSunShadowMinCosTheta = 0.15F;

// 法线偏移的半径，单位是纹素。2x2 的 tap 网格（±0.5 纹素）加上每个 tap 自带的硬件
// 双线性（再 ±0.5 纹素），整个足迹的半宽正好是 1 个纹素——偏移要盖住的就是它。
const float kSunShadowNormalOffsetTexels = 1.0F;

// RN-34：接触硬化的半影半径（纹素）。
//
// 固定半径的 PCF 在**接触处**是错的：方块脚下的遮挡距离是 0，那里物理上应当是硬边，
// 却照样吃满整个足迹。实机量到的后果是墙根一条约 0.15 格宽、亮度过量 31% 的软亮带
// （亮带宽度 = 足迹半宽 / sin(太阳仰角)，所以低太阳时格外宽）。
//
// 真实的半影宽度只取决于遮挡物离接收面多远：
//
//     半影 = (接收点深度 - 遮挡物深度) x tan(太阳视角半径)
//
// 真太阳的视角半径是 0.27 度（tan 约 0.0047）——按这个数算，一格高的方块在地面上的
// 半影只有 0.005 格，也就是**处处硬边**。那不好看，也抗不住 1/16 格纹素的锯齿。
// 这里把它放大到约 0.9 度：离地 2 格以上的遮挡物就吃满今天的半径，2 格以内逐渐收紧到 0。
// 于是**远处的影子和今天完全一样，只有贴着投射者那一段变锐**——正是要修的那一段。
// 放大太阳是光影包的通行做法，不是我们自创的取巧。
const float kSunPenumbraTangent = 0.0156F;
// 今天的 tap 偏移，也是收紧后的上限：超过它就会改变远处影子的观感。
const float kSunMaxPenumbraTexels = 0.5F;

float sunShadowPenumbraTexels(float blockerDistanceBlocks) {
    float penumbraBlocks = max(blockerDistanceBlocks, 0.0F) * kSunPenumbraTangent;
    return min(penumbraBlocks / kSunShadowTexelSizeBlocks, kSunMaxPenumbraTexels);
}

// 沿法线抬多高（世界格）。正比于 sin(入射角)：正对太阳的面（sin = 0）同一个纹素里
// 深度几乎不变，不需要抬；越掠射抬得越多，上限是一个纹素 0.0625 格。
//
// clamp 是从旧的 sunShadowBiasBlocks 继承来的护栏，别删：它保证喂进 [0,1] 之外的
// cos（背面、舍入误差）时结果仍然有限且有界——(-1) 给上界 0.0625，(2) 给 0。
// 它**不是**「背面返回 0」的意思；背面在 sunShadowFactor 的开头就提前返回了，
// 根本走不到这里。
float sunShadowNormalOffsetBlocks(float incidenceCosine) {
    float cosTheta = clamp(incidenceCosine, 0.0F, 1.0F);
    float sinTheta = sqrt(max(1.0F - cosTheta * cosTheta, 0.0F));
    return kSunShadowTexelSizeBlocks * kSunShadowNormalOffsetTexels * sinTheta;
}
// 法线在光源的 right/up/sun 三个正交轴上的分量。正负法线得到相同的平面斜率。
float sunShadowTapOffsetBlocks(float normalRight, float normalUp, float normalSun,
                               float tapX, float tapY) {
    float denominator = max(abs(normalSun), kSunShadowMinCosTheta);
    if (normalSun < 0.0F) denominator = -denominator;
    return kSunShadowTexelSizeBlocks * (normalRight * tapX + normalUp * tapY) / denominator;
}
