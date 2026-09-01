#pragma once
// I-2 物品展示层：ItemStack → 可显示信息的单一来源
//
// 对标 26.1 `ItemStack#getStyledHoverName`(:829) / `#getTooltipLines`(:847) /
// `#addDetailsToTooltip`(:860)，按本项目已有的物品模型取子集。
//
// 这一层是**纯值计算**：零渲染依赖、零静态状态、零时钟，同一个
// (ItemStack, TooltipContext) 恒产出同一份行。它因此能 headless 断言，
// 这也正是它必须离开 `render/vulkan/HudRenderer.hpp` 的原因——ENCH-2 的现场
// 缺陷批把组装逻辑塞在一个 Vulkan 头里，于是这层文本一条断言都没有。
//
// 单一来源铁律：提示框、手持物名弹出、以及将来任何要显示物品的地方
// （聊天、成就、死亡消息），一律问这一层。`itemDisplayName` 的名字解析只在
// 本层的 .cpp 内出现一次，外部拿不到它，也就漂不了。
//
// 明确不做（留接缝不建空壳）：DataComponents（那是 I.comp 的事）、lore、
// 自定义名、药水效果、烟花、旗帜图案——本项目没有这些内容。
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mc::ui {

class Language;

// 行的**颜色语义**，不是颜色值：RGBA 的选择留给渲染器的一张表
// （`HudRenderer::tooltipLineColor`），资源包换主题时只改后者。
//
// 名称行按稀有度分了四档而不是一个笼统的 Name：vanilla 的
// `getStyledHoverName` 用 `getRarity().color()` 上色，渲染器要查表就必须能从
// style 本身分辨出是哪一档。四个名称档按稀有度升序排列，于是"附魔后升一档"
// 在断言里就是 `style` 变大。
enum class TooltipStyle : std::uint8_t {
    NameCommon,     // 白 0xFFFFFF —— Rarity::Common
    NameUncommon,   // 黄 0xFFFF55 —— Rarity::Uncommon
    NameRare,       // 青 0x55FFFF —— Rarity::Rare
    NameEpic,       // 淡紫 0xFF55FF —— Rarity::Epic
    Detail,         // 灰 0xAAAAAA —— 附魔行、`item.modifiers.*` 小标题
    Curse,          // 红 0xFF5555 —— 诅咒附魔（Enchantment#getFullname 的 CURSE 分支）
    AttributeBase,  // 深绿 0x00AA00 —— 主手 `attribute.modifier.equals.*`（含基础值那种）
    AttributeBonus, // 蓝 0x5555FF —— 护甲 `attribute.modifier.plus.*`（Attribute.Sentiment 的正向色）
    Advanced,       // 暗灰 0x555555 —— F3+H 才出现的耐久与物品 id
};

struct TooltipLine final {
    std::string text;
    TooltipStyle style = TooltipStyle::Detail;
    // I-3: vanilla 的 `getStyledHoverName` 给**自定义名**加 ITALIC，那是"这东西被
    // 改过名"的唯一视觉线索。这里把它作为数据表达出来并被测试钉住；
    // ⚠ **渲染侧目前不斜体**——`drawHudText` 没有斜体（vanilla 是对字形做剪切
    // 变换），补它是一条渲染改动，已登记欠账。数据先正确，别为了"看起来对"
    // 而在这一层撒谎说没有斜体。
    bool italic = false;
};

// `Item.TooltipContext` + `TooltipFlag` 的本项目等价物。
// language 为空指针时每条键都退到英文兜底，绝不出现裸 key。
struct TooltipContext final {
    bool advanced = false; // F3+H：vanilla 的 TooltipFlag#isAdvanced
    const Language* language = nullptr;
};

// `getStyledHoverName`：名称行，按稀有度着色。
// 手持物名弹出也走它——那里只要这一行，不必组装整框。
[[nodiscard]] TooltipLine itemNameLine(const gameplay::ItemStack& stack,
                                       const TooltipContext& context);

// `getTooltipLines`：名称 → 附魔 → 属性 → advanced，缺的项跳过不占位。
// 数量不在其中：vanilla 的数量画在槽位上（`drawHudSlot` 已经画了），
// 提示框里再写一遍 " xN" 是历史遗留，I-2 删掉了它。
[[nodiscard]] std::vector<TooltipLine> itemTooltipLines(const gameplay::ItemStack& stack,
                                                        const TooltipContext& context);

// `Enchantment#getFullname` 的文本半边：翻译名 + 等级数字。
// 附魔台的线索提示框（`container.enchant.clue`）与提示框的附魔行共用它。
[[nodiscard]] std::string enchantmentLabel(gameplay::EnchantmentId id, int level,
                                           const TooltipContext& context);

// 稀有度 → 名称行的 style。渲染器不需要认识 Rarity，只认 TooltipStyle。
[[nodiscard]] TooltipStyle nameStyle(gameplay::Rarity rarity);

} // namespace mc::ui
