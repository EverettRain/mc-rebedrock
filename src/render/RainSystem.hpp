#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::render {

// 一滴 CPU 模拟的下落雨滴
// 异步粒子雨把这批雨滴实例化，整片雨一次绘制画完
// 贴图雨走 vanilla 的逐列渲染，此时雨滴不会变成公告板，但仍然驱动落地水花与天气音效
struct RainDrop final {
    glm::vec3 position{0.0F};
    float size = 0.03F;
    // 这一滴自己的侧风偏移量，单位是每秒格数，生成时定下之后不再变
    // 整片雨跟随本帧的基准风向，而每滴各自带一点差异，斜度因此不会整齐划一
    glm::vec2 windJitter{0.0F};
    // 这一滴要落向的面，取它所在列里生成天花板以下最高的碰撞方块或流体的顶面
    // 位置越过该面的那一帧就溅射
    // 取 -1 表示探测范围内什么都没找到，此后一路自由落体直到落到相机下方重生
    // columnX 与 columnZ 记住这个值是为哪一列算的
    // 风把雨滴吹进新的一列时重新取那一列的面，那只是一次缓存读取而不是重新扫描世界
    float targetSurface = -1.0F;
    bool surfaceWater = false;
    int columnX = 0;
    int columnZ = 0;
};

// 上一次更新里落到某个面上的一滴雨，记下溅射位置、落点是水面还是固体地面，以及水平喷射方向
// 撞墙的溅射带非零 direction，那是雨滴撞上那一面的外法线，水珠沿固定扇形弹回来
// 地面与水面的溅射把它留成零，水珠四散喷出
// 渲染器每帧把这批事件排空并灌进粒子系统
struct RainSplash final {
    glm::vec3 position{0.0F};
    bool onWater = false;
    glm::vec2 direction{0.0F};
    // vanilla tickRainSplashing 采样出的 RAIN 粒子是单颗向上的原味水珠
    // 模拟雨滴自己落地时走的仍是多颗水珠的喷射
    bool sampledImpact = false;
};

// 把降雨推进一帧
// 碰撞不是逐滴逐帧算的，只有第一滴进入某一列的雨去扫描世界并把该列最高的面缓存下来
// 之后进入同一列的雨滴直接落到缓存的面上并在那里溅射
// 这让一场大雨从每帧几千次世界查询降到少量探测加廉价的缓存读取
// 也正因为便宜，雨域才能铺得更高更宽，高屋顶不再漏雨，远处也能看到水花
// 缓存有一个较长的过期窗口，超出容量上限时整表清空，世界被编辑或相机移动后都会重新探测
//
// 需要提醒的是，上面这套说法在相机高速飞行时并不成立
// 螺旋飞行会不断带入新列，缓存始终是冷的，实测每滴每帧仍有约六次世界查询
// 但整段代价只有零点三毫秒，因此不值得为此改数据结构
class RainSystem final {
  public:
    // 把降雨推进一帧
    // 先把雨滴数量增减到 targetCount 乘以降雨强度，再让每滴下落
    // 已经落到所在列的面上的雨滴在那里溅射，然后回到顶部重生
    void update(float deltaSeconds, const glm::vec3& cameraPosition, float intensity,
                std::size_t targetCount, const world::World& world, const glm::vec2& wind);

    // 贴图雨那一侧的视觉部分，对应 WorldRenderer#tickRainSplashing
    // 按 20 TPS 采样相机附近的列，在固体碰撞顶面或流体水面上追加一次撞击事件
    // 它与贴图档刻意压得很小的模拟雨滴数量互不相干
    void emitTextureImpacts(float deltaSeconds, const glm::vec3& cameraPosition,
                            float intensity, const world::World& world);

    // 选择碰撞策略
    // 缓存打开时是默认行为，第一滴进入某列的雨探测该列的面，其余雨滴直接用缓存值
    // 关闭时每滴每帧都探测自己所在位置的方块，那是最初的全保真路径，留给性能有余量的机器
    void setCollisionCache(bool use) { useCollisionCache_ = use; }
    [[nodiscard]] bool collisionCacheEnabled() const { return useCollisionCache_; }

    [[nodiscard]] const std::vector<RainDrop>& drops() const { return drops_; }
    // 上一次更新收集到的溅射事件，渲染器把它们排空灌进粒子系统，下一次更新会清空这张表
    [[nodiscard]] const std::vector<RainSplash>& splashes() const { return splashes_; }
    // 上一次更新的碰撞处理发起了多少次世界查询，只统计探测扫描
    // 这是诊断用的计数，用来量化列表面缓存把逐滴逐帧的开销压成了多少次探测
    [[nodiscard]] std::size_t lastUpdateLookups() const { return lastUpdateLookups_; }

    // 返回某一列里第一个挡住降水的面的顶部高度，用的是与下落雨滴同一份有界缓存
    // 贴图渲染器把它当作 vanilla 的 MOTION_BLOCKING 高度图来用
    // 竖直雨列从这个高度起画，雨因此留在屋顶之上，不会把雨帘画穿到下面的房间里
    // 返回 -1 表示在 ceiling 以下的探测范围内没有找到任何面
    [[nodiscard]] float precipitationSurfaceY(const world::World& world, int blockX, int blockZ,
                                              float ceiling);

  private:
    // 一条缓存下来的列表面，记下生成天花板以下最高的碰撞或流体方块的顶部 Y
    // 另外记下它是不是水面，以及这次探测发生的时刻
    struct ColumnSurface final {
        float surfaceY = -1.0F;
        bool water = false;
        float probedAt = -1000.0F;
    };

    // 取某一列缓存下来的面，未命中或已过期时探测一次并写回缓存
    // 探测从生成窗口顶部的天花板往下扫，因此找到的是屋顶和树冠而不只是地面
    [[nodiscard]] ColumnSurface columnSurface(const world::World& world, int blockX, int blockZ,
                                              float ceiling);

    // 在相机周围的盒子里挑一列新的，把雨滴放到那一列的面之上并记下它的目标面
    // 这样雨滴总要下落一小段再落地，绝不会凭空出现在遮蔽物底下
    void respawnDrop(RainDrop& drop, const glm::vec3& cameraPosition, const world::World& world);
    // 逐滴直接碰撞那条路径用的无缓存重生，只在盒子里随机取个位置
    // 它不需要维护目标面，因为那条路径每帧自己去探测世界
    void respawnDropFree(RainDrop& drop, const glm::vec3& cameraPosition);

    [[nodiscard]] float randomUnit();
    [[nodiscard]] static std::uint64_t columnKey(int blockX, int blockZ);
    // 撞墙面的打包键，由 blockX、blockZ 与 blockY 三者组成
    [[nodiscard]] static std::uint64_t wallKey(int blockX, int blockZ, int blockY);

    std::vector<RainDrop> drops_;
    std::vector<RainSplash> splashes_;
    std::unordered_map<std::uint64_t, ColumnSurface> surfaceCache_;
    // 雨滴被风吹进去的那些墙面，键是列加高度，值是背离该面的水平方向也就是墙的外法线
    // 第一滴撞上时缓存下来，之后到达同一面的雨滴照这个固定方向溅射，不再重新探测世界
    std::unordered_map<std::uint64_t, glm::vec2> wallCache_;
    std::uint32_t randomState_ = 0x5EED41U;
    float timeSeconds_ = 0.0F;
    float textureImpactAccumulator_ = 0.0F;
    std::uint64_t textureImpactTick_ = 0U;
    std::size_t lastUpdateLookups_ = 0U;
    bool useCollisionCache_ = true;
};

} // namespace mc::render
