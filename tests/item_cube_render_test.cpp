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

// 集合的定义本身：立方体路径收下整格立方体、六面立方体、箱子和台阶
// 台阶在内是因为它走同一条立方体路径，只是 Y 向减半，减半与否由调用方读 isSlab 决定
[[nodiscard]] constexpr bool expectedForModel(BlockModel model) {
    return model == BlockModel::Cube || model == BlockModel::DirectionalCube ||
           model == BlockModel::Chest || model == BlockModel::Slab;
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

// 明确留在集合外的那些：十字植物、火把、作物，以及楼梯与门这类异形方块
// HUD 图标另有一层 isShapedBlockModel 近似，把楼梯之类也画成 3D 方块图标
// 那一层是图标独有的，不属于本判断
// 哪天要把它推广到掉落物与手持物，该改的是那一层，而不是悄悄把它们塞进这个集合
void testNonCubeModelsStayOut() {
    for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        const auto model = blockDefinition(block).model;
        if (model == BlockModel::Cross || model == BlockModel::Crop ||
            model == BlockModel::Torch || model == BlockModel::Stairs ||
            model == BlockModel::Door || model == BlockModel::TrapDoor) {
            assert(!rendersAsCubeItem(block));
        }
    }
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
    testIsFullCubeIsAStrictSubset();
    return 0;
}
