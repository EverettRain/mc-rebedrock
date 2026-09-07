// RN-23：实体圆形阴影贴花的采集判定。
//
// 贴花的观感要 GPU，但它「贴在地上」的那部分全是可证的：哪些格出片、每片贴在多高、
// 每片多淡、什么时候整只实体一片也不出。这些都是 extractShadow/extractShadowPiece
// 逐条转写出来的判定，世界访问是一个回调，所以这里喂它一个数组世界就能逐条断言，
// 不需要 Vulkan、不需要窗口。
//
// 出处（Mojang 映射，26.1）：
//   client/net/minecraft/client/renderer/entity/EntityRenderer.java:270-304  extractShadow
//   client/net/minecraft/client/renderer/entity/EntityRenderer.java:307-325  extractShadowPiece
//   client/net/minecraft/client/renderer/feature/ShadowFeatureRenderer.java:20-42  几何与 UV
//   client/net/minecraft/client/renderer/Lightmap.java:89-93                 getBrightness
//   common/net/minecraft/world/level/lighting/LevelLightEngine.java:145-148  getRawBrightness

#include "render/EntityShadowDecal.hpp"
#include "render/SkyLight.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error("line " + std::to_string(line) + ": " + message);
    }
}
#define REQUIRE(condition, message) require(condition, message, __LINE__)

using mc::render::EntityShadowCell;
using mc::render::EntityShadowInput;
using mc::render::EntityShadowPiece;
using mc::world::Block;
using mc::world::BlockState;
using mc::world::SlabPortion;

// 一个数组世界。两张覆盖表分开存，因为「这一格下面是什么」和「这一格多亮」是采集器
// 分别问的两件事，把它们放进同一条记录会让「只改光照」的夹具连带改掉地面——那样
// 亮度门的测试就会退化成形状门的测试，而且是静默退化。
//
// 它同时记下每一次访问：亮度取的是实体脚踩的那一格，方块取的是它**下面**那一格，
// 两者错位一格，影子就会在错误的地方消失，而 piece 数量看起来还是对的。
class FakeWorld final {
  public:
    // sample(x, y, z).below 的值，即「(x,y,z) 这一格下面那个方块」。
    void setBelow(int x, int y, int z, BlockState state) { below_[{x, y, z}] = state; }
    void setLight(int x, int y, int z, int sky, int block) { light_[{x, y, z}] = {sky, block}; }
    // 一整层地面：y 这一层的每一格，其下方都是 `state`。
    void setGroundUnder(int y, BlockState state) { groundLayer_ = {y, state}; }

    [[nodiscard]] auto sampler() {
        return [this](int x, int y, int z) {
            visited_.emplace_back(x, y, z);
            EntityShadowCell cell{BlockState{Block::Air}, 15, 0};
            if (groundLayer_.has_value() && groundLayer_->first == y) {
                cell.below = groundLayer_->second;
            }
            if (const auto found = below_.find({x, y, z}); found != below_.end()) {
                cell.below = found->second;
            }
            if (const auto found = light_.find({x, y, z}); found != light_.end()) {
                cell.skyLight = found->second.first;
                cell.blockLight = found->second.second;
            }
            return cell;
        };
    }

    [[nodiscard]] const std::vector<std::tuple<int, int, int>>& visited() const { return visited_; }
    void clearVisits() { visited_.clear(); }

  private:
    using Key = std::tuple<int, int, int>;
    std::map<Key, BlockState> below_;
    std::map<Key, std::pair<int, int>> light_;
    std::optional<std::pair<int, BlockState>> groundLayer_;
    std::vector<Key> visited_;
};

constexpr float kPlayerShadowRadius = 0.5F;  // AvatarRenderer.java:50

// 一只站在 (8, 65, 8) 的玩家，脚下是一整层石头，正午，贴着相机。
EntityShadowInput standingPlayer() {
    return EntityShadowInput{{8.0F, 65.0F, 8.0F}, kPlayerShadowRadius, 1.0F, 0.0F, 0};
}

// r = 0.5、站在格点上的足迹是 floor(7.5)..floor(8.5) = 7..8，两轴各两格。
constexpr std::size_t kPlayerFootprintCells = 4;

// ---- 形状单一源：满方块这道门本身 -------------------------------------------

void checkFullBlockGate() {
    REQUIRE(mc::world::isCollisionShapeFullBlock(BlockState{Block::Stone}),
            "a full cube must answer isCollisionShapeFullBlock");
    REQUIRE(!mc::world::isCollisionShapeFullBlock(BlockState{Block::Air}),
            "air collides with nothing and cannot carry a shadow");
    REQUIRE(!mc::world::isCollisionShapeFullBlock(
                BlockState{Block::StoneSlab}.withSlabPortion(SlabPortion::Bottom)),
            "a bottom slab fills half its cell and must not answer full block");
    REQUIRE(!mc::world::isCollisionShapeFullBlock(
                BlockState{Block::StoneSlab}.withSlabPortion(SlabPortion::Top)),
            "a top slab is no fuller than a bottom one");
    REQUIRE(mc::world::isCollisionShapeFullBlock(
                BlockState{Block::StoneSlab}.withSlabPortion(SlabPortion::Double)),
            "a double slab is a full cube again");
    REQUIRE(!mc::world::isCollisionShapeFullBlock(BlockState{Block::OakStairs}),
            "a stair is two boxes, neither of which fills the cell");
    // 关着的栅栏门是本仓唯一碰撞形状高过自己格子的方块（1.5 格）。高不等于满：
    // 它是一根柱子，横向填不满，所以这道门必须看三个轴而不是只看高度。
    REQUIRE(!mc::world::isCollisionShapeFullBlock(BlockState{Block::OakFenceGate}),
            "a closed fence gate is 1.5 cells tall and still not a full block");
    REQUIRE(!mc::world::isCollisionShapeFullBlock(BlockState{Block::Torch}),
            "a torch has no collision at all");
}

// ---- 亮度：公式、门、以及问的是哪一格 ---------------------------------------

void checkBrightness() {
    // LevelLightEngine.getRawBrightness = max(blockLight, skyLight - skyDarken)
    REQUIRE(mc::render::maxLocalRawBrightness(15, 0, 0) == 15, "full sky by day is 15");
    REQUIRE(mc::render::maxLocalRawBrightness(15, 0, 11) == 4,
            "full sky at night is 15 - 11 = 4, which is still above the shadow gate");
    REQUIRE(mc::render::maxLocalRawBrightness(0, 7, 11) == 7,
            "block light is not darkened by the sky track");

    // Lightmap.getBrightness with the overworld's ambient_light of 0:
    //   v = level/15, curved = v / (4 - 3v)
    REQUIRE(std::abs(mc::render::lightmapBrightness(15) - 1.0F) < 1e-6F,
            "level 15 must reach full brightness");
    REQUIRE(std::abs(mc::render::lightmapBrightness(0)) < 1e-6F, "level 0 must be black");
    const float four = 4.0F / 15.0F;
    REQUIRE(std::abs(mc::render::lightmapBrightness(4) - four / (4.0F - 3.0F * four)) < 1e-6F,
            "the brightness curve must be v / (4 - 3v), not v itself");
    REQUIRE(mc::render::lightmapBrightness(4) < 0.1F,
            "the curve is far from linear: level 4 is under a tenth of full");
    // 环境光把整条曲线往 1 抬（下界的 0.1）。留着这个参数是为了它，不是为了让调用方
    // 随手换一个数。
    REQUIRE(mc::render::lightmapBrightness(0, 0.1F) > mc::render::lightmapBrightness(0, 0.0F),
            "ambient light must lift the dark end of the curve");

    // 天光衰减来自 SkyLight —— 本仓这条公式的单一源，不是这里又抄一份。
    REQUIRE(mc::render::SkyLight::skyDarken(6000.0) == 0, "noon must not darken the sky track");
    REQUIRE(mc::render::SkyLight::skyDarken(18000.0) == 11, "midnight's skyDarken is 11");
}

void checkDarkCellsProduceNoPiece() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;

    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells,
            "a lit full block under the entity must carry a shadow piece in every footprint cell");

    // 亮度正好 3：vanilla 的门是 `brightness > 3`，所以 3 不出片。
    for (int z = 7; z <= 8; ++z) {
        for (int x = 7; x <= 8; ++x) {
            world.setLight(x, 65, z, 3, 0);
        }
    }
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.empty(),
            "brightness 3 is at the gate, not past it: a shadow must not appear in the dark");

    // 4 就过。这一格差是这道门唯一的判据，所以两边都要钉住。
    for (int z = 7; z <= 8; ++z) {
        for (int x = 7; x <= 8; ++x) {
            world.setLight(x, 65, z, 4, 0);
        }
    }
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells,
            "brightness 4 is past the gate and must restore every piece");
    REQUIRE(std::ranges::all_of(pieces, [](const EntityShadowPiece& p) { return p.alpha < 0.05F; }),
            "a barely-lit floor must carry a barely-visible shadow, not a full-strength one");

    // 火把也算：门读的是 max(sky, block)，不是天光。
    for (int z = 7; z <= 8; ++z) {
        for (int x = 7; x <= 8; ++x) {
            world.setLight(x, 65, z, 0, 14);
        }
    }
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells && pieces.front().alpha > 0.3F,
            "block light alone must light a shadow: the gate reads max(sky, block)");
}

void checkNightSkyDarkening() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;

    auto atNight = standingPlayer();
    atNight.skyDarken = 11;  // 午夜，露天满天光 -> 15 - 11 = 4
    mc::render::collectEntityShadowPieces(atNight, world.sampler(), pieces);
    REQUIRE(!pieces.empty(), "open ground at midnight is brightness 4 and still casts a shadow");
    const float nightAlpha = pieces.front().alpha;

    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.front().alpha > nightAlpha * 4.0F,
            "the same ground by day must carry a far stronger shadow than at night");

    // 再暗一档就整个消失：这是 skyDarken 真的参与了判定的证据，而不是被当成 0。
    atNight.skyDarken = 12;
    mc::render::collectEntityShadowPieces(atNight, world.sampler(), pieces);
    REQUIRE(pieces.empty(), "sky darkening must feed the gate, or night shadows never fade out");
}

void checkTheWalkStaysInItsRows() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;
    world.clearVisits();
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    // depth = min(pow/0.5 - 1, r) = 0.5，所以 y0 = floor(64.5) = 64、y1 = 65。
    for (const auto& [x, y, z] : world.visited()) {
        REQUIRE(y >= 64 && y <= 65,
                "the walk must stay inside [floor(y - depth), floor(y)], not read a neighbour row");
        REQUIRE(x >= 7 && x <= 8 && z >= 7 && z <= 8,
                "the walk must stay inside the shadow radius footprint");
    }
    REQUIRE(std::ranges::all_of(
                pieces, [](const EntityShadowPiece& p) { return std::abs(p.relativeY) < 1e-6F; }),
            "a piece on the floor the entity stands on sits exactly at the entity's own y");
}

// ---- 形状门：影子不挂在台阶和栅栏门上 ---------------------------------------

void checkPartialBlocksCarryNoPiece() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells,
            "a 0.5 radius footprint covers floor(x-r)..floor(x+r) = 2x2 cells");

    // 把其中一格换成下半台阶：那一格不出片，其余照旧。这正是导出夹具里那块
    // stone_slab 会给出的画面证据。
    world.setBelow(8, 65, 8, BlockState{Block::StoneSlab}.withSlabPortion(SlabPortion::Bottom));
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells - 1U,
            "a slab is not a full block: the shadow must be cut away over it, not draped on it");

    // 楼梯同理 —— 夹具里那级 oak_stairs 就是这条门的第二个画面证据。
    world.setBelow(8, 65, 8, BlockState{Block::OakStairs});
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells - 1U,
            "a stair must not carry a shadow piece either");

    // 空气：vanilla 的第一道门是 renderShape != INVISIBLE，空气是唯一命中的。
    world.setBelow(8, 65, 8, BlockState{Block::Air});
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells - 1U,
            "a hole in the floor must show through the shadow");
}

// ---- 距离：16 格外一片也没有 -------------------------------------------------

void checkDistanceFalloff() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;

    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    const float nearAlpha = pieces.front().alpha;

    auto middle = standingPlayer();
    middle.distanceToCameraSq = 144.0F;  // 12 格
    mc::render::collectEntityShadowPieces(middle, world.sampler(), pieces);
    REQUIRE(!pieces.empty() && pieces.front().alpha < nearAlpha,
            "a shadow must fade with distance, not switch off at a threshold");

    auto atSixteen = standingPlayer();
    atSixteen.distanceToCameraSq = 256.0F;  // 正好 16 格：pow == 0
    mc::render::collectEntityShadowPieces(atSixteen, world.sampler(), pieces);
    REQUIRE(pieces.empty(), "pow reaches 0 at 16 blocks and no piece may be collected there");

    auto beyond = standingPlayer();
    beyond.distanceToCameraSq = 400.0F;  // 20 格：pow < 0
    mc::render::collectEntityShadowPieces(beyond, world.sampler(), pieces);
    REQUIRE(pieces.empty(), "past 16 blocks pow is negative and the whole entity drops out");

    REQUIRE(mc::render::entityShadowPower({{}, 0.5F, 1.0F, 0.0F, 0}) == 1.0F,
            "pow at the camera is the entity's full shadow strength");
    REQUIRE(std::abs(mc::render::entityShadowPower({{}, 0.15F, 0.75F, 0.0F, 0}) - 0.75F) < 1e-6F,
            "a dropped item's 0.75 strength must scale pow, not the alpha alone");
}

// ---- 深度：影子顺着地形往下铺，越深越淡 -------------------------------------

void checkDepthFalloff() {
    // 足迹是 x,z ∈ {7,8}。把 (7,7) 那一列的地面挖低一格，影子要顺着掉下去。
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    world.setBelow(7, 65, 7, BlockState{Block::Air});
    world.setBelow(7, 64, 7, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;
    mc::render::collectEntityShadowPieces(standingPlayer(), world.sampler(), pieces);
    REQUIRE(pieces.size() == kPlayerFootprintCells,
            "the pit cell must still carry a piece, one row further down");

    float topAlpha = 0.0F;
    float lowerAlpha = 0.0F;
    bool sawLower = false;
    for (const EntityShadowPiece& piece : pieces) {
        if (std::abs(piece.relativeY) < 1e-6F) {
            topAlpha = piece.alpha;
        } else {
            REQUIRE(std::abs(piece.relativeY + 1.0F) < 1e-6F,
                    "the only other row the walk reaches is one cell down");
            lowerAlpha = piece.alpha;
            sawLower = true;
        }
    }
    REQUIRE(sawLower, "a shadow must spill into the pit beside the entity, not stop at the edge");
    REQUIRE(lowerAlpha > 0.0F && lowerAlpha < topAlpha,
            "each cell of depth costs 0.5 of pow, so the lower row must be visibly fainter");
    REQUIRE(std::abs(lowerAlpha - topAlpha * 0.5F) < 1e-5F,
            "pow 1 loses exactly half over one cell: 0.5*B down to 0.25*B");

    // depth 本身：pow/0.5 - 1，再钳到半径。
    REQUIRE(std::abs(mc::render::entityShadowDepth(1.0F, 0.5F) - 0.5F) < 1e-6F,
            "depth is clamped by the shadow radius");
    REQUIRE(std::abs(mc::render::entityShadowDepth(1.0F, 4.0F) - 1.0F) < 1e-6F,
            "an unclamped depth at full power is pow/0.5 - 1 = 1");
    REQUIRE(mc::render::entityShadowDepth(0.4F, 4.0F) < 0.0F,
            "a weak shadow reaches no deeper than its own cell");

    // 逐格衰减的公式本身，与它在实体正下方的取值。
    REQUIRE(std::abs(mc::render::entityShadowPieceAlpha(1.0F, 65.0F, 65, 15) - 0.5F) < 1e-6F,
            "a full-power shadow on bright ground is alpha 0.5, not 1.0");
    REQUIRE(mc::render::entityShadowPieceAlpha(1.0F, 65.0F, 60, 15) == 0.0F,
            "five cells down, powerAtDepth has gone negative and the alpha clamps to zero");
}

// ---- 足迹：格子范围与半径 ---------------------------------------------------

void checkFootprint() {
    FakeWorld world;
    world.setGroundUnder(65, BlockState{Block::Stone});
    std::vector<EntityShadowPiece> pieces;

    // 猪的 0.7（PigRenderer.java:27）：floor(7.3)=7 .. floor(8.7)=8，仍是 2x2。
    auto pig = standingPlayer();
    pig.radius = 0.7F;
    mc::render::collectEntityShadowPieces(pig, world.sampler(), pieces);
    REQUIRE(pieces.size() == 4, "a 0.7 radius on a cell corner still spans 2x2 cells");

    // 站在格子正中的一只大实体：floor(7.3)=7 .. floor(9.7)=9，3x3。
    auto wide = standingPlayer();
    wide.position = {8.5F, 65.0F, 8.5F};
    wide.radius = 1.2F;
    mc::render::collectEntityShadowPieces(wide, world.sampler(), pieces);
    REQUIRE(pieces.size() == 9, "the footprint is floor(x-r)..floor(x+r), inclusive on both ends");

    // 半径 0 的实体（vanilla 的默认 shadowRadius，只有设过它的 renderer 才有影子）
    // 一片也不出。
    auto invisible = standingPlayer();
    invisible.radius = 0.0F;
    mc::render::collectEntityShadowPieces(invisible, world.sampler(), pieces);
    REQUIRE(pieces.empty(), "a zero shadow radius means no shadow at all, not a degenerate one");
}

// ---- UV：一组 piece 拼成的是一个圆 -----------------------------------------

void checkPieceUv() {
    const float radius = 2.0F;
    // 正好铺满足迹的一片：从 -radius 到 +radius，UV 应当跨满 [0,1]。
    const EntityShadowPiece full{-radius, 0.0F, -radius, 2.0F * radius, 2.0F * radius, 1.0F};
    const auto uv = mc::render::entityShadowPieceUv(full, radius);
    REQUIRE(std::abs(uv.u0 - 1.0F) < 1e-6F && std::abs(uv.u1) < 1e-6F,
            "a piece spanning the whole footprint must span the whole shadow texture");
    REQUIRE(std::abs(uv.v0 - 1.0F) < 1e-6F && std::abs(uv.v1) < 1e-6F, "the same in z");

    // 实体正下方那一点落在贴图正中，也就是圆盘最黑的地方。
    const EntityShadowPiece centred{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const auto middle = mc::render::entityShadowPieceUv(centred, radius);
    REQUIRE(std::abs(middle.u0 - 0.5F) < 1e-6F && std::abs(middle.v0 - 0.5F) < 1e-6F,
            "the cell under the entity maps to the centre of the shadow disc");

    // 接缝：相邻两片在共享边上的 UV 必须相同，否则拼出来的不是一个圆而是一堆错位方块。
    const EntityShadowPiece left{-1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
    const EntityShadowPiece right{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
    const auto leftUv = mc::render::entityShadowPieceUv(left, radius);
    const auto rightUv = mc::render::entityShadowPieceUv(right, radius);
    REQUIRE(std::abs(leftUv.u1 - rightUv.u0) < 1e-6F,
            "neighbouring pieces must agree on the UV of the edge they share");

    // UV 与这一片贴多高无关：地形低一级的那一片，采样的仍是它在圆盘里那一块。
    EntityShadowPiece lowered = right;
    lowered.relativeY = -1.0F;
    const auto loweredUv = mc::render::entityShadowPieceUv(lowered, radius);
    REQUIRE(loweredUv.u0 == rightUv.u0 && loweredUv.v0 == rightUv.v0,
            "a piece that fell to a lower block keeps its place in the disc");

    // 半径变了，同一片格子在圆盘里的位置也跟着变：UV 是位置除以直径，不是格子序号。
    const auto wideUv = mc::render::entityShadowPieceUv(right, 4.0F);
    REQUIRE(std::abs(wideUv.u1 - 0.5F) < std::abs(rightUv.u1 - 0.5F),
            "a larger shadow spreads the same cell over less of the disc");
}

} // namespace

int main() {
    try {
        checkFullBlockGate();
        checkBrightness();
        checkDarkCellsProduceNoPiece();
        checkNightSkyDarkening();
        checkTheWalkStaysInItsRows();
        checkPartialBlocksCarryNoPiece();
        checkDistanceFalloff();
        checkDepthFalloff();
        checkFootprint();
        checkPieceUv();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
