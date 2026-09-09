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

// RN-35：纹素的世界边长（格）**逐级不同**。下面几个函数因此都收一个 texelSizeBlocks
// 参数，而不是读一个全局常量：它们表达的全是「多少个纹素」，而纹素有多大是级别的函数。
// 漏传就是按远段的纹素去抬近段的偏置，也就是抬高 8 倍——影子整片从投射者脚下浮起来。
//
// RN-47：那两个常量已经删了。近段框成了玩家可调的一档，纹素因此从**正在采的那张矩阵**
// 里推（`sunShadowTexelBlocksOf`，见 sun_shadow.glsl）——C++ 与 GLSL 各存一份常量再靠
// 测试钉在一起的做法，到这里为止。

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

// RN-35：上限仍是 **0.5 个纹素**，不是一个固定的物理宽度。于是近段的最大半影是
// 0.5 x 0.0078 = 0.0039 格，远段仍是 0.031 格。两个理由：
//
//  * 真太阳的视角半径 0.27 度，一格遮挡距离的真实半影是 0.005 格。上面把太阳放大到
//    0.9 度，正是因为 1/16 格的纹素撑不住真值；近段的 1/128 格撑得住，0.0039 格恰好
//    落回真值量级。**分辨率提上去之后，同一个「0.5 纹素」的口径自己走回了物理。**
//  * 2x2 的 tap 布局一个字都不用动。改成固定物理宽度会让近段要 4 纹素的半径，而
//    2x2 个 tap 摆在 ±4 纹素上中间是空的——那需要更多 tap，把 RN-33 省下的 55% 吐回去。
//
// 代价是级联接缝处半影宽度有 8 倍的跳变。登记在 RN-35 §5。
// RN-36：天气。云层把直射光散掉，阴影因此**变浅**——而从前它一点都不知道在下雨。
//
// 现场（用户实机）：下雨时天空盒暗下来，影子却还是同样浓、同样锐，割裂感明显。
// 成因在 `grass_block.frag` 那一行：
//
//     skyFactor = SKY_LIGHT_FACTOR × 天气减光 × 阴影可见度
//
// 三者是**乘性**的。天气减光全雨时是 0.6875（vanilla 的 5/16），可阴影仍旧把天光压到
// 0.35——影子相对周围的深度恒为 65%，晴天雨天一个样。物理上反了：全阴天没有直射光，
// 也就没有明显的影子。
//
// 云量：下雨即云满天，雷暴再压满。两条 gradient 都是 vanilla 的 0..1 渐变量，所以
// 天气转换期间这一档也是连续的，不会在某一 tick 上跳。
// 权重 0.9 / 0.1 的意思是：纯下雨留一丝残影（对比度约 6.5%），雷暴则完全没有影子。
float sunShadowOvercast(float rainGradient, float thunderGradient) {
    return clamp(rainGradient * 0.9F + thunderGradient * 0.1F, 0.0F, 1.0F);
}

// RN-38：天光是**两项**，不是一个被阴影乘掉的数。
//
// 直射（太阳本体）与环境（整片天空的散射）在物理上是两种光，只有前者会被一个方块
// 挡住。从前两者合成一个 `skyFactor` 一起乘 `shadowFactor`，于是：
//
//   * 影子里保留的那 35% 是一个硬编码的系数，不是「天空散射有多少」这个量；
//   * 影子把环境天光也压暗了，所以它偏灰而不是偏蓝；
//   * RN-36 的雨天只能在那个系数上再叠一层近似。
//
// 晴天正午天空散射占地面照度的比例。RN-42 从 0.2 收到 0.15：实机反馈「影子偏亮」，
// 而晴空正午的散射份额实测在 10%~15% 之间，0.15 取的是这一档的上沿。
const float kSkyAmbientFraction = 0.15F;

// RN-46b：假反射光的强度，相对**直射**的份额。0.06 让全影处从 0.15 抬到约 0.19，
// 也就是 RN-42 收窄散射份额之前的量级——但只在**有直射可弹**的时候。
// Photon 用的是 0.033 乘它自己的 ao 与 skylight^4；我们没有逐片元的 ao，
// 天光曲线替它做了「洞里没有」这一半。
const float kBouncedLight = 0.06F;

// 云厚到这个程度，直射已经不剩什么，整套遮挡搜索加 PCF 都可以省掉——
// 逐屏幕像素的开销，换一个看不见的差别。
const float kSunShadowInvisibleOvercast = 0.98F;

// 天光通道的最终强度。
//
//   直射：随云量**转给**散射（云把它散开，不是吸收掉），并被阴影完整挡住
//   散射：不受阴影影响
//
// 总量的下降由 `weatherDimming` 单独表达（vanilla 的 5/16），这里只分配比例——
// 所以全阴天的总亮度与从前一样，变的只是「影子还起不起作用」。
// RN-42：直射项终于带上了入射角。
//
// 现场（用户实机）：树影盖在地上，可旁边那面墙照旧是满亮的——墙根一道 4 倍的台阶。
// 成因是接收端那句 `if (incidence <= 0.0) return 1.0;`：正午每一个竖直面的入射角余弦
// 都约等于 0，可见度恒为 1，**永远进不了阴影**。而 cardinalShade 是方向无关的 0.6/0.8，
// 没有任何东西会因为太阳在头顶而让侧面暗下来。
//
// 没有「又亮又能接受阴影」的写法：正午的竖直面在几何上就位于**它自己那一格的影子里**，
// 诚实的遮挡查询给出的答案就是暗；而 cos → 0 时接收端偏置在任何有界代价下都不可靠
// （见 RN-41 那条发散）。唯一稳定的写法就是让直射权重在那里归零——权重为 0 的地方，
// 偏置失不失效都看不见。
//
// ★ 但不能直接乘 N·L：**时段的明暗 vanilla 已经用 skyLightFactor 表达过一遍了**
// （`sunDirection.w`）。再乘一次 sin(太阳仰角) 就是把一天的曲线算两遍，清晨傍晚会平白
// 暗一倍。所以权重是**相对于水平面**的：
//
//     directWeight = clamp(max(N·L, 0) / max(太阳的仰角余弦, eps), 0, 1)
//
// 于是水平地面在任何时刻都恰好是 1——今天的地面亮度一个字不动；正午的竖直面是 0；
// 而清晨朝向太阳的那一面仍旧吃满直射（比值远大于 1，被 clamp 收到 1），
// 与 vanilla 那种「早上东面亮」的观感一致。
//
// eps 只在太阳压到地平线（仰角约 1 度以内）时起作用，那时 skyLightFactor 本来就快到 0。
const float kSunUpEpsilon = 0.02F;

float sunDirectWeight(float incidenceCosine, float sunUpCosine) {
    return clamp(max(incidenceCosine, 0.0F) / max(sunUpCosine, kSunUpEpsilon), 0.0F, 1.0F);
}

// 太阳在不在地平线以上。落下之后**它那一份直射整份转给散射**——与云做的是同一件事
// （RN-36/38 的模型），所以夜里天光通道的总量与从前逐位相同：月光本来就是没有方向的，
// 给它乘一个入射角权重会让整个夜晚平白暗掉六倍。
//
// 0.05 ≈ 太阳高出地平线 2.9 度。在那之上直射是满的，日出日落的戏剧性一分不减。
const float kSunPresenceCosine = 0.05F;

float sunPresence(float sunUpCosine) {
    return clamp(sunUpCosine / kSunPresenceCosine, 0.0F, 1.0F);
}

// RN-46a：水下的直射。
//
// 现场（用户实机）：水下的影子和水面上一样浓。参照 Photon 的水体（`water_fog_vl.glsl`）：
// 它的阴影查询是**一次硬 step，连 PCF 都没有**——水下影子的观感完全不来自阴影图的锐度，
// 来自 `exp(-消光系数 x 光在水里走过的距离)`：水越深直射越少，影子与周围的对比自然消失。
//
// 所以这里做的是**淡**，不是**糊**。真要更糊得加 tap（半影上限是 0.5 纹素，加宽会在
// 2x2 的布局里留洞），而那不是水下影子变浅的机制。
//
// 丢掉的那份**转给散射**——水把定向的光散成漫射，与云做的是同一件事（RN-36/38 的模型，
// 这是它的第四个取值）。吸收那一半由既有的水色与水雾表达，不在这里重算一遍。
//
// 半深 4 格：1 格几乎不变（0.84），4 格剩一半，15 格（掩码能装下的上限）剩 0.074。
const float kWaterDirectHalfDepthBlocks = 4.0F;

float sunWaterTransmittance(float submergedBlocks) {
    return exp2(-max(submergedBlocks, 0.0F) / kWaterDirectHalfDepthBlocks);
}

// 收的是**几何量**（面的入射角余弦、太阳的仰角余弦、头顶的水柱），不是已经算好的权重：
// 三个采样者各自去算那个比值，就有三个地方可以算错。
float sunSkyFactor(float skyLightFactor, float weatherDimming, float visibility, float rain,
                   float thunder, float incidenceCosine, float sunUpCosine,
                   float submergedBlocks) {
    float directShare = (1.0F - kSkyAmbientFraction) *
                        (1.0F - sunShadowOvercast(rain, thunder)) * sunPresence(sunUpCosine) *
                        sunWaterTransmittance(submergedBlocks);
    float ambientShare = 1.0F - directShare;
    // RN-46b：假反射光。RN-42 把直射项带上入射角之后，正午的竖直面掉到了散射那一份，
    // 画面整体偏暗——真正缺的是**从周围受光表面弹回来的那一点光**（GI），而在 LDR 里
    // 它只能是一项近似。
    //
    // 形状照 Photon 的 `bounced`（`diffuse_lighting.glsl`）：正比于**被挡住了多少**，
    // 不是一个常数底值。理由是物理的——反射光来自周围被照亮的表面，所以：
    //
    //   * 乘 directShare ⇒ 夜里、全阴天、深水下**没有可弹的光**，这一项自动消失；
    //   * 乘 (1 - 已收到的直射) ⇒ 受光面一点不加，全影处加满；
    //   * 整个天光通道随后还要乘 lightmap 的天空曲线，所以洞里（skyLevel = 0）也没有。
    //
    // ★ 受光处因此**仍然恰好是 1.0**——RN-42 立的那个锚一个字不动。
    float missingDirect = 1.0F - sunDirectWeight(incidenceCosine, sunUpCosine) *
                                     clamp(visibility, 0.0F, 1.0F);
    return skyLightFactor * weatherDimming *
           (directShare * sunDirectWeight(incidenceCosine, sunUpCosine) *
                clamp(visibility, 0.0F, 1.0F) +
            ambientShare + kBouncedLight * directShare * missingDirect);
}


// RN-41：竖直薄片（十字植物、作物）自己遮自己。
//
// 现场（用户实机，正午前后）：草上有明显的斑马纹。成因不在阴影图，在**法线是假的**：
// 十字植物四个角的法线写的是朝上（ChunkMesher 的 appendPlantQuad），光照按 vanilla 的
// 观感算，可几何是竖直的。接收端的法线抬升按 sin(入射角) 推，正午 sin(0) = 0，于是
// 一格高的薄片一点偏置都没有，而它在阴影图里横跨的深度是它自己的整个高度。
//
// 要的偏置正是那个高度沿光线的投影：植物高 1 格，其深度跨度 = 1 x sin(太阳仰角)，
// 而 sin(太阳仰角) 就是朝上法线的入射角余弦，也就是这里的 incidence。于是：
//
//   * 正午（incidence = 1）偏置 1 格 —— 整株草被当成**一个光照单元**，与它的 Light
//     等级、它的生物群系着色一样按格给，不再自己遮自己；
//   * 日出日落（incidence -> 0）偏置 -> 0 —— 那时光线几乎垂直于薄片，本来就不自遮，
//     也就不该付 peter-panning 的代价。
//
// 这个偏置**只给薄片**。给普通的朝上面（方块顶面）加它会吃掉一格以内的接触阴影。
const float kThinPlaneHeightBlocks = 1.0F;

float sunShadowThinPlaneBiasBlocks(float thinPlane, float incidenceCosine) {
    return thinPlane * kThinPlaneHeightBlocks * clamp(incidenceCosine, 0.0F, 1.0F);
}

// RN-43 在这里有过一个 `sunShadowCascadeBlend`（近段框最外两成的过渡带）。
// RN-49 把它删了：两级的分歧不只是半影宽度，还有「看没看见这个投射者」，
// 混合会把远段的漏采混进近段的实影（实机现象是阴影线上的光斑）。见 docs 的 RN-49。
float sunShadowPenumbraTexels(float blockerDistanceBlocks, float texelSizeBlocks) {
    float penumbraBlocks = max(blockerDistanceBlocks, 0.0F) * kSunPenumbraTangent;
    return min(penumbraBlocks / texelSizeBlocks, kSunMaxPenumbraTexels);
}

// 沿法线抬多高（世界格）。正比于 sin(入射角)：正对太阳的面（sin = 0）同一个纹素里
// 深度几乎不变，不需要抬；越掠射抬得越多，上限是一个纹素（近段 0.0078 格、远段 0.0625 格）。
//
// clamp 是从旧的 sunShadowBiasBlocks 继承来的护栏，别删：它保证喂进 [0,1] 之外的
// cos（背面、舍入误差）时结果仍然有限且有界——(-1) 给上界一个纹素，(2) 给 0。
// 它**不是**「背面返回 0」的意思；背面在 sunShadowFactor 的开头就提前返回了，
// 根本走不到这里。
float sunShadowNormalOffsetBlocks(float incidenceCosine, float texelSizeBlocks) {
    float cosTheta = clamp(incidenceCosine, 0.0F, 1.0F);
    float sinTheta = sqrt(max(1.0F - cosTheta * cosTheta, 0.0F));
    return texelSizeBlocks * kSunShadowNormalOffsetTexels * sinTheta;
}
// 法线在光源的 right/up/sun 三个正交轴上的分量。正负法线得到相同的平面斜率。
float sunShadowTapOffsetBlocks(float normalRight, float normalUp, float normalSun,
                               float tapX, float tapY, float texelSizeBlocks) {
    float denominator = max(abs(normalSun), kSunShadowMinCosTheta);
    if (normalSun < 0.0F) denominator = -denominator;
    return texelSizeBlocks * (normalRight * tapX + normalUp * tapY) / denominator;
}
