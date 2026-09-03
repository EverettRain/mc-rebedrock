// world::rendersAsCubeItem 是三条物品渲染面共用的单一判断
// 那三条面分别是掉落物、第一人称手持与 HUD 背包图标
// 它回答的是按立方体几何画，还是按挤出的 2.5D 图标画
//
// 这个测试存在的理由是一个真实发生过的静默 bug
// 这三处曾各自手写 model == Cube || model == Chest || ... 之类的条件，三份口径互不相同
// 引入 DirectionalCube 也就是侦测器时补进了手持与图标两处，漏了掉落物那处
// 于是同一个侦测器拿在手里是立方体、放进背包是立方体、掉在地上却是一张扁平贴图
// 三处合一之后，这里把口径本身钉死，任何新增的 BlockModel 都必须在这里表态
//
// 同时 include BlockAtlasLayout.hpp
// 它那组 static_assert 把图集固定特殊区各段的起始层号表述成上一段起点加上一段层数
// 编译进这个目标就等于把层号表也一并校验了
// 改了某段层数却忘了顺移后面的起点时，这个测试先编译不过

#include "render/vulkan/BlockAtlasLayout.hpp"
#include "world/Block.hpp"

#include <cassert>
#include <cstddef>

namespace {

using mc::world::Block;
using mc::world::BlockModel;
using mc::world::blockDefinition;
using mc::world::isFullCube;
using mc::world::isSlab;
using mc::world::rendersAsCubeItem;

// 集合的定义本身，逐 model 列举而不是复述实现里的谓词
// 这里刻意用**穷举 switch 且不写 default**：新增一个 BlockModel 会在这里触发
// -Wswitch，逼它表态，而抄一份 `isShapedBlockModel(...) && !isThinLeafIcon(...)`
// 只会跟着实现一起错
//
// RN-10f 把楼梯/墙/栅栏门/按钮/压力板收进来了。它们本来就被画成 3D 图标，
// 只不过那条规则**只写在 HUD 图标那一面**，掉落物与手持物没有，于是同一个栅栏门
// 掉在地上是扁平贴图、拿在背包里是方块。本文件旧注释说"要推广就改那一层，别悄悄
// 塞进这个集合"——推广的正确做法就是把那条规则并回单点、删掉图标那一面的例外，
// 而这里同步表态，所以它既不悄悄也没留下第二处口径
[[nodiscard]] constexpr bool expectedForModel(BlockModel model) {
    switch (model) {
    // 立方体几何：整格立方体、六面立方体、箱子、台阶（Y 向减半由调用方读 isSlab）
    case BlockModel::Cube:
    case BlockModel::DirectionalCube:
    case BlockModel::Chest:
    case BlockModel::Slab:
    // 异形但有 3D 轮廓的：vanilla 的物品渲染同样画方块图标
    case BlockModel::Stairs:
    case BlockModel::Wall:
    case BlockModel::FenceGate:
    case BlockModel::Button:
    case BlockModel::PressurePlate:
        return true;
    // 薄片：vanilla 的门/活板门物品是扁平贴图
    case BlockModel::Door:
    case BlockModel::TrapDoor:
    // 本来就不是盒子的
    case BlockModel::Cross:
    case BlockModel::Crop:
    case BlockModel::Torch:
    case BlockModel::ElementModel:
    case BlockModel::RedstoneWire:
    case BlockModel::Fire:
        return false;
    }
    return false;
}

// 每个方块的判断都只由它的 model 决定，没有任何按方块身份开的后门
// 这正是原先三处内联判断做不到的，它们各自漏掉了不同的 model
void testKeyedOnModelAlone() {
    for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        assert(rendersAsCubeItem(block) == expectedForModel(blockDefinition(block).model));
    }
}

// 回归点：侦测器是 DirectionalCube，掉落物路径当初就是漏了它
void testDirectionalCubeIsACubeItem() {
    assert(blockDefinition(Block::Observer).model == BlockModel::DirectionalCube);
    assert(rendersAsCubeItem(Block::Observer));
}

// 三个仍在集合内的老成员，防止合一时被顺手漏掉
void testCubeChestSlabStayIn() {
    assert(blockDefinition(Block::Stone).model == BlockModel::Cube);
    assert(rendersAsCubeItem(Block::Stone));

    assert(blockDefinition(Block::Chest).model == BlockModel::Chest);
    assert(rendersAsCubeItem(Block::Chest));

    bool sawSlab = false;
    for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        if (!isSlab(block)) {
            continue;
        }
        sawSlab = true;
        assert(rendersAsCubeItem(block));
    }
    assert(sawSlab);  // 台阶存在，上面那圈断言不是空转
}

// 明确留在集合外的那些：十字植物、火把、作物，以及薄片状的门与活板门
// RN-10f 之后楼梯不在此列——它进了集合，见 expectedForModel 的注释
void testNonCubeModelsStayOut() {
    for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        const auto model = blockDefinition(block).model;
        if (model == BlockModel::Cross || model == BlockModel::Crop ||
            model == BlockModel::Torch || model == BlockModel::Door ||
            model == BlockModel::TrapDoor) {
            assert(!rendersAsCubeItem(block));
        }
    }
}

// RN-10f 的回归点，也是本文件存在的那个 bug 的第二次发作：异形方块的 3D 图标规则
// 只写在 HUD 那一面，掉落物与手持物没有。三条面共用单点之后，这些必须在集合内
void testShapedBlocksAreCubeItemsToo() {
    assert(blockDefinition(Block::OakStairs).model == BlockModel::Stairs);
    assert(rendersAsCubeItem(Block::OakStairs));
    assert(blockDefinition(Block::OakFenceGate).model == BlockModel::FenceGate);
    assert(rendersAsCubeItem(Block::OakFenceGate));
    assert(blockDefinition(Block::StonePressurePlate).model == BlockModel::PressurePlate);
    assert(rendersAsCubeItem(Block::StonePressurePlate));
    // 而薄片仍然在外：vanilla 的门/活板门物品是扁平贴图
    assert(blockDefinition(Block::OakDoor).model == BlockModel::Door);
    assert(!rendersAsCubeItem(Block::OakDoor));
    assert(blockDefinition(Block::OakTrapdoor).model == BlockModel::TrapDoor);
    assert(!rendersAsCubeItem(Block::OakTrapdoor));
}

// 与 isFullCube 的关系：那个谓词回答的是填不填满自己那一格，服务于遮挡与面稳固
// 它因此排除了箱子和台阶，而这两者拿在手里都画成盒子
// 所以两者是真包含关系，不是等价
void testIsFullCubeIsAStrictSubset() {
    bool sawStrictlyMore = false;
    for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        if (isFullCube(block)) {
            assert(rendersAsCubeItem(block));  // 包含
        }
        if (rendersAsCubeItem(block) && !isFullCube(block)) {
            sawStrictlyMore = true;  // 箱子/台阶落在这里
        }
    }
    assert(sawStrictlyMore);
}

}  // namespace

int main() {
    testKeyedOnModelAlone();
    testDirectionalCubeIsACubeItem();
    testCubeChestSlabStayIn();
    testNonCubeModelsStayOut();
    testShapedBlocksAreCubeItemsToo();
    testIsFullCubeIsAStrictSubset();
    return 0;
}
