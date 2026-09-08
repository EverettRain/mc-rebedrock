// 光照的不透明度是它自己的一根轴，不是渲染分桶。
//
// 现场口径：台阶铺的屋顶下面一片死黑。根因是 `skyLightOpacity` 的第一档问的是
// `isOpaque`，也就是 `renderLayer == Opaque`——渲染分桶。26.1 的
// `BlockBehaviour.getLightDampening` 问的是**遮挡形状是不是满方块**：
//
//     isSolidRender ? 15 : (propagatesSkylightDown ? 0 : 1)
//     isSolidRender = isShapeFullBlock(canOcclude ? getOcclusionShape() : empty)
//
// 台阶渲染在 Opaque 桶里（它的面是实心纹理），但它**不填满格子**，所以 vanilla 给它
// 的衰减是 0。楼梯渲染在 Cutout 桶里，因而一直是对的——这也是为什么这个缺陷能藏这么久：
// 它只在「渲染分桶说不透明、遮挡形状说不是满方块」的那一小撮方块身上出现。
//
// 这是 RN-8 立项时那个三合一字段的最后一份：`canOcclude` 已由 RN-8e 拆出，
// 渲染分桶自己一轴，光这一轴到 RN-8f 才拆。

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using mc::world::Block;
using mc::world::BlockRenderLayer;
using mc::world::BlockState;
using mc::world::SlabPortion;

int failures = 0;

void expect(int actual, int wanted, std::string_view what) {
    if (actual != wanted) {
        std::cerr << "FAILED: " << what << ": expected " << wanted << ", actual " << actual << "\n";
        ++failures;
    }
}

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

[[nodiscard]] BlockState slab(SlabPortion portion) {
    return BlockState{Block::OakSlab, mc::world::defaultOrientation(Block::OakSlab), 0U}
        .withSlabPortion(portion);
}

// ---- 逐块的值 ------------------------------------------------------------
//
// 这一组是 26.1 的答案，逐条对着源码：石头满遮挡 → 15；玻璃 `TransparentBlock`
// 覆写 `propagatesSkylightDown` 为 true → 0；水 `LiquidBlock` 覆写为 false → 1；
// 树叶 `LeavesBlock` 覆写 `getLightDampening` → 1；台阶/楼梯/附魔台/铁砧的遮挡形状
// 都不是满方块，且默认的 `propagatesSkylightDown` 为 true → 0。
void testIdentityValues() {
    expect(mc::world::skyLightOpacity(Block::Stone), 15, "石头是满遮挡");
    expect(mc::world::skyLightOpacity(Block::Glass), 0, "玻璃不衰减");
    expect(mc::world::skyLightOpacity(Block::Water), 1, "水衰减 1");
    expect(mc::world::skyLightOpacity(Block::OakLeaves), 1, "树叶衰减 1");
    expect(mc::world::skyLightOpacity(Block::OakStairs), 0, "楼梯不衰减（一直是对的）");
    expect(mc::world::skyLightOpacity(Block::OakSlab), 0, "台阶不衰减（本轮修的就是它）");
    expect(mc::world::skyLightOpacity(Block::StoneSlab), 0, "石台阶同理");
    expect(mc::world::skyLightOpacity(Block::EnchantingTable), 0, "附魔台不是满方块");
    expect(mc::world::skyLightOpacity(Block::Anvil), 0, "铁砧不是满方块");

    // 台阶确实在 Opaque 桶里——如果哪天它被改成 Cutout，这个缺陷就不再能通过台阶
    // 观察到，上面那条断言也就不再证明「拆轴」这件事。钉住前提。
    require(mc::world::blockDefinition(Block::OakSlab).renderLayer == BlockRenderLayer::Opaque,
            "台阶必须仍然渲染在 Opaque 桶里，否则这条测试证明不了拆轴");
}

// ---- 逐状态：满遮挡是状态的问题 ------------------------------------------
void testDoubleSlabStillBlocks() {
    expect(mc::world::skyLightOpacity(slab(SlabPortion::Bottom)), 0, "下半砖不衰减");
    expect(mc::world::skyLightOpacity(slab(SlabPortion::Top)), 0, "上半砖不衰减");
    // 双层台阶填满格子，它必须仍然是完全挡光的——按身份问的 `isFullCube` 对三者
    // 给同一个答案，所以状态那一层不能只是转发给身份那一层。
    expect(mc::world::skyLightOpacity(slab(SlabPortion::Double)), 15, "双层台阶仍然完全挡光");

    require(!mc::world::blocksLight(slab(SlabPortion::Bottom)), "下半砖不阻断光");
    require(!mc::world::blocksLight(slab(SlabPortion::Top)), "上半砖不阻断光");
    require(mc::world::blocksLight(slab(SlabPortion::Double)), "双层台阶阻断光");
    require(mc::world::blocksLight(BlockState{Block::Stone}), "石头阻断光");
    require(!mc::world::blocksLight(BlockState{Block::Glass}), "玻璃不阻断光");
}

// ---- 含水格仍按水衰减 ----------------------------------------------------
void testSubmergedSlabReadsWater() {
    if (!mc::world::canBeSubmerged(Block::OakSlab)) {
        std::cerr << "FAILED: 台阶应当可含水，否则下面这条无从谈起\n";
        ++failures;
        return;
    }
    const BlockState wet = slab(SlabPortion::Bottom).withSubmergedFluid(
        mc::world::SubmergedFluid::Water);
    expect(mc::world::skyLightOpacity(wet), mc::world::skyLightOpacity(Block::Water),
           "含水的下半砖按水衰减，而不是按它自己的 0");
    require(!mc::world::blocksLight(wet), "含水半砖衰减 1，不是完全阻断");
}

// ---- 整份名册：渲染分桶不得参与这个答案 ----------------------------------
//
// 上面那些是点，这条是面：对**每一个**可渲染方块，衰减 15 当且仅当
// 「能遮挡 且 是满方块」。渲染分桶不出现在这条判据里，所以将来任何一块
// 「Opaque 桶 + 非满方块」的新方块都不会重蹈覆辙，而不必有人记得回来加一条断言。
void testRosterFollowsShapeNotBucket() {
    int mismatches = 0;
    int bucketDisagreesWithShape = 0;
    for (int index = 0; index < static_cast<int>(Block::Count); ++index) {
        const Block block = static_cast<Block>(index);
        if (!mc::world::isRenderable(block)) {
            continue;
        }
        const bool solidRender = mc::world::canOcclude(block) && mc::world::isFullCube(block);
        const int wanted = solidRender ? 15 : mc::world::blockDefinition(block).lightFilter;
        if (mc::world::skyLightOpacity(block) != wanted) {
            std::cerr << "FAILED: " << mc::world::blockDefinition(block).identifier.toString()
                      << " 的衰减是 " << int{mc::world::skyLightOpacity(block)} << "，按形状应为 "
                      << wanted << "\n";
            ++mismatches;
        }
        // 「渲染分桶说不透明，而遮挡形状说不是满方块」——这一撮就是缺陷的栖息地。
        if (mc::world::isOpaque(block) != solidRender) {
            ++bucketDisagreesWithShape;
        }
    }
    failures += mismatches;
    // 若这一撮是空的，上面那条全名册断言就成了恒真句：分桶与形状处处一致时，
    // 用哪一个都得到同样的答案，测试也就不再区分正确与错误的实现。
    require(bucketDisagreesWithShape > 0,
            "必须存在「Opaque 桶但非满方块」的方块，否则这条名册断言是空的");
    std::cout << "  名册中分桶与形状不一致的方块：" << bucketDisagreesWithShape << " 个\n";
}

} // namespace

int main() {
    testIdentityValues();
    testDoubleSlabStillBlocks();
    testSubmergedSlabReadsWater();
    testRosterFollowsShapeNotBucket();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "light_opacity_axis ok\n";
    return 0;
}
