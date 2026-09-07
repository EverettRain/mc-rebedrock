#pragma once

#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace mc::render {

// RN-23：实体的圆形阴影贴花。
//
// 这不是「在实体脚下画一个圆」。26.1 的贴花是**逐格采集**的：
// `EntityRenderer.extractShadow` (client/.../entity/EntityRenderer.java:270-304)
// 在实体周围 radius 的方形足迹里逐格向下探几层，
// `extractShadowPiece` (同文件 307-325) 决定每一格出不出一片影子；
// `ShadowFeatureRenderer.renderTranslucent`
// (client/.../renderer/feature/ShadowFeatureRenderer.java:20-42) 再把每一片画成
// 一个水平四边形。
//
// 每一片贴在**它下方那个方块的顶面**上，带自己的 alpha，UV 是那一格在影子圆盘
// [0,1]² 里的子矩形。所以一个影子是一**组** piece：它会沿地形起伏铺开、被台阶和
// 栅栏截断、在暗处整个消失、随距离淡出。这正是原版实体影子看起来「贴在地上」而不是
// 浮着一个圆盘的原因，也是这个文件存在的全部理由 —— 单片圆盘做不到其中任何一条。
//
// 消费点故意只有采集这一半：世界访问是一个模板回调，生产喂渲染器的客户端方块缓存，
// headless 测试喂一个数组世界。这条判定链因此不需要 Vulkan、不需要窗口，也就能被
// 逐条断言。

// 一片影子。坐标相对实体位置，与 vanilla 的 `EntityRenderState.ShadowPiece`
// (client/.../entity/state/EntityRenderState.java:58) 逐字段对应，只是把
// `shapeBelow` 这个 VoxelShape 换成了它的 `bounds()` —— 消费点
// (ShadowFeatureRenderer) 本来就只读 bounds，而入口门 isCollisionShapeFullBlock
// 又保证了 bounds 恒是整格，所以存整个形状是存了一份没人读的东西。
struct EntityShadowPiece final {
    // 这一格的西北下角相对实体位置的偏移（vanilla 的 relativeX + aabb.minX 等）。
    float relativeX = 0.0F;
    // 下方方块的**顶面**高度：vanilla 的 relativeY + aabb.minY，其中 relativeY 是
    // 上方那格的 y。满方块的 minY 是 0，于是它落在下方方块的顶面上。
    float relativeY = 0.0F;
    float relativeZ = 0.0F;
    // 这一格的水平尺寸，来自下方形状 bounds 的 x/z 跨度。
    float sizeX = 1.0F;
    float sizeZ = 1.0F;
    float alpha = 0.0F;
};

// 一只实体投影所需的一切。半径与强度是**逐物种**的常量，在各自的注册处声明
// （生物在 EntityRenderDescriptor，玩家/掉落物/经验球/下落方块在渲染器的采集点），
// 不在这里写一张 switch 表。
struct EntityShadowInput final {
    // 实体脚点，世界坐标。vanilla 的 state.x/y/z 就是这个点。
    glm::vec3 position{0.0F};
    // getShadowRadius，vanilla 在 extractShadow 里先 min 到 32。
    float radius = 0.0F;
    // getShadowStrength，默认 1；掉落物与经验球是 0.75。
    float strength = 1.0F;
    // 相机到实体的距离平方。贴花在 16 格外完全消失（256 = 16²）。
    float distanceToCameraSq = 0.0F;
    // `Level.updateSkyBrightness` 的整数天光衰减，白天 0、夜里 11。
    // 由 render::SkyLight::skyDarken(dayTick) 提供 —— 那是这条公式在本仓的单一源。
    int skyDarken = 0;
};

// 一格的世界样本：这一格自身的光照，加上它**下方**那一格的方块状态。
// 采集器一格只问一次世界，问的就是这三样。
struct EntityShadowCell final {
    world::BlockState below{};
    int skyLight = 0;
    int blockLight = 0;
};

// vanilla 在 extractShadow 里对半径的钳位。
inline constexpr float kMaxEntityShadowRadius = 32.0F;

// `Lightmap.getBrightness` (client/.../renderer/Lightmap.java:89-93)：
//   v = level/15; curved = v/(4 - 3v); lerp(ambientLight, curved, 1)
// 主世界的 ambient_light 是 0，所以这里默认就是那条曲线本身。参数留着是因为下界
// （0.1）迟早要用它，而不是为了让调用方随手换一个数。
[[nodiscard]] float lightmapBrightness(int level, float ambientLight = 0.0F);

// `getMaxLocalRawBrightness` 的本体：
// `LevelLightEngine.getRawBrightness` (common/.../lighting/LevelLightEngine.java:145-148)
// 是 max(blockLight, skyLight - skyDarken)。
[[nodiscard]] int maxLocalRawBrightness(int skyLight, int blockLight, int skyDarken);

// vanilla 的 `pow`：(1 - distSq/256) * shadowStrength。<= 0 表示这只实体在 16 格
// 之外，一片影子也不出。
[[nodiscard]] float entityShadowPower(const EntityShadowInput& entity);

// vanilla 的 `depth`：min(pow/0.5 - 1, shadowRadius)。往下探几格 —— 强的影子探得深，
// 所以站在坑边缘时影子会顺着坑壁往下铺一段。
[[nodiscard]] float entityShadowDepth(float power, float radius);

// 一片的 alpha：clamp(powerAtDepth * 0.5 * lightmapBrightness(brightness), 0, 1)，
// 其中 powerAtDepth = pow - (实体 y - 该格 y) * 0.5 —— 每往下一格淡 0.5。
[[nodiscard]] float entityShadowPieceAlpha(float power, float entityY, int pieceY, int brightness);

// 一片在影子圆盘贴图上的 UV 矩形。
// `ShadowFeatureRenderer.java:33-36`：u = -x/2/radius + 0.5，v 同理用 z。
//
// UV 完全由**位置**决定，与这一片自己有多大、贴在多高无关 —— 这正是整组 piece 能拼
// 成一个圆的原因：相邻两格的边界坐标相同，算出来的 UV 就相同，接缝天生对齐；而某一格
// 因为地形低了一级，它那一小片圆也就跟着落到低一级的方块顶面上。
struct EntityShadowPieceUv final {
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 0.0F;
    float v1 = 0.0F;
};

// u0/v0 配 piece 的 min 角，u1/v1 配 max 角。负号让两者反序（u0 > u1），这照抄
// vanilla：贴图关于中心对称，反序不改变画面，改动它却会让这份转写与出处对不上。
[[nodiscard]] EntityShadowPieceUv entityShadowPieceUv(const EntityShadowPiece& piece, float radius);

// 采集一只实体的全部 piece，逐条照 extractShadow/extractShadowPiece 的顺序。
// `sample(x, y, z)` 返回该格的 EntityShadowCell。`out` 先被清空。
//
// 三道门的顺序与 vanilla 一致，并且**必须**一致：亮度门在满方块门之前，所以一个
// 暗处的满方块和一个亮处的台阶都不出片，而两道门谁先谁后决定了世界被问了几次。
template <typename Sample>
void collectEntityShadowPieces(const EntityShadowInput& entity, Sample&& sample,
                               std::vector<EntityShadowPiece>& out) {
    out.clear();
    const float radius = std::min(entity.radius, kMaxEntityShadowRadius);
    if (!(radius > 0.0F)) {
        return;
    }
    const float power = entityShadowPower(entity);
    if (!(power > 0.0F)) {
        return;
    }
    const auto floorToInt = [](float value) { return static_cast<int>(std::floor(value)); };
    const int x0 = floorToInt(entity.position.x - radius);
    const int x1 = floorToInt(entity.position.x + radius);
    const int z0 = floorToInt(entity.position.z - radius);
    const int z1 = floorToInt(entity.position.z + radius);
    const float depth = entityShadowDepth(power, radius);
    const int y0 = floorToInt(entity.position.y - depth);
    const int y1 = floorToInt(entity.position.y);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                const EntityShadowCell cell = sample(x, y, z);
                // vanilla: belowState.getRenderShape() != RenderShape.INVISIBLE。
                // 空气是唯一的不可见方块，isRenderable 就是那个问题的单一源。
                if (!world::isRenderable(cell.below.block())) {
                    continue;
                }
                const int brightness =
                    maxLocalRawBrightness(cell.skyLight, cell.blockLight, entity.skyDarken);
                if (brightness <= 3) {
                    continue;
                }
                if (!world::isCollisionShapeFullBlock(cell.below)) {
                    continue;
                }
                // vanilla 这里还有一道 `!belowShape.isEmpty()`，以及把 bounds 的
                // minX/minZ 加进相对坐标、用 maxX-minX 当尺寸。过了上面那道门之后
                // 三者都是定值：满方块的 bounds 就是 0..1，空形状不可能满。所以这里
                // 的 0 偏移与 1×1 尺寸是**推出来的**，不是省事写死的 —— 哪天出现一个
                // 碰撞满而轮廓不满的方块，改的是上面那道门的定义，不是这三个数。
                out.push_back(EntityShadowPiece{
                    static_cast<float>(x) - entity.position.x,
                    static_cast<float>(y) - entity.position.y,
                    static_cast<float>(z) - entity.position.z,
                    1.0F,
                    1.0F,
                    entityShadowPieceAlpha(power, entity.position.y, y, brightness),
                });
            }
        }
    }
}

} // namespace mc::render
