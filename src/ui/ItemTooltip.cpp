#include "ui/ItemTooltip.hpp"

#include "ui/Language.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace mc::ui {
namespace {

// 缺键一律退到英文兜底，绝不出现裸 key；language 为空（测试、无资源包启动）
// 时同样走兜底而不是崩。
[[nodiscard]] std::string translate(const TooltipContext& context, std::string_view key,
                                    std::string_view fallback) {
    if (context.language == nullptr) {
        return std::string{fallback};
    }
    return std::string{context.language->translate(key, fallback)};
}

// 名字不叫 `format`：实参是 std::string，ADL 会把 `std::format` 一并拉进重载集，
// 而它的第一个形参是 consteval 的 `format_string`，于是"用运行期字符串当模式"在
// libc++（mac）上直接编译不过。libstdc++ 只是碰巧没传递包含 <format> 才没报。
[[nodiscard]] std::string fillTemplate(std::string_view pattern, std::string_view first,
                                       std::string_view second) {
    const std::array<std::string_view, 2> arguments{first, second};
    return formatTranslation(pattern, arguments);
}

// `ItemStack#getHoverName` 的名字解析：**全仓唯一一处**。
// 它是文件内的静态函数而不是导出符号，于是"单一来源"由构造保证——
// 外面拿不到它，也就不可能再长出第二份答案（新增显示物品的地方一律走
// itemNameLine / itemTooltipLines）。
// legacy 方块栈（item 指针为空）由 itemDescriptionId 归一到方块自己的 BlockItem。
[[nodiscard]] std::string itemDisplayName(const gameplay::ItemStack& stack,
                                          const TooltipContext& context) {
    const gameplay::DescriptionId descriptionId = gameplay::itemDescriptionId(stack);
    if (descriptionId.empty()) {
        return {};
    }
    if (context.language == nullptr) {
        return std::string{descriptionId.source.path};
    }
    return std::string{context.language->translate(descriptionId.prefix(),
                                                   descriptionId.source.space,
                                                   descriptionId.source.path,
                                                   descriptionId.source.path)};
}

// `enchantment.level.<n>` 没有对应语言键时的兜底：vanilla 自己那些键拼出的罗马
// 数字。只会用到 1..10（没有附魔更高），超出的退回十进制，数据包给的高等级
// 仍然读得出来。
[[nodiscard]] std::string romanNumeral(int level) {
    static constexpr std::array<std::string_view, 10> kNumerals{
        "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    if (level >= 1 && level <= static_cast<int>(kNumerals.size())) {
        return std::string{kNumerals[static_cast<std::size_t>(level - 1)]};
    }
    return std::to_string(level);
}

// `ItemAttributeModifiers.ATTRIBUTE_MODIFIER_FORMAT`（`DecimalFormat("#.##")`）：
// 最多两位小数，尾随的零与小数点都不写（4.0 → "4"，1.6 → "1.6"）。
[[nodiscard]] std::string formatAttributeAmount(float amount) {
    std::array<char, 32> buffer{};
    const int written =
        std::snprintf(buffer.data(), buffer.size(), "%.2f", static_cast<double>(amount));
    if (written <= 0) {
        return "0";
    }
    std::string text{buffer.data(), static_cast<std::size_t>(written)};
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text.empty() ? std::string{"0"} : text;
}

// `attribute.name.<path>` 的译名。
[[nodiscard]] std::string attributeName(const TooltipContext& context, std::string_view path,
                                        std::string_view fallback) {
    return translate(context, "attribute.name." + std::string{path}, fallback);
}

// `Display.Default#apply` 的 displayWithBase 分支：主手攻击伤害/攻击速度把玩家
// 的基础值算进去后整值显示，键是 `attribute.modifier.equals.0`（ADD_VALUE），
// 前面还有一个空格（vanilla 的 `CommonComponents.space().append(...)`）。
// 本项目 toolAttributes 里的 attackDamage/attackSpeed 存的就是这个"已含基础值"
// 的展示值（剑 4.0/1.6 而不是修饰量 +3.0/-2.4），直接用。
[[nodiscard]] TooltipLine equalsLine(const TooltipContext& context, float amount,
                                     std::string_view attributePath,
                                     std::string_view attributeFallback) {
    return TooltipLine{
        " " + fillTemplate(translate(context, "attribute.modifier.equals.0", "%s %s"),
                           formatAttributeAmount(amount),
                           attributeName(context, attributePath, attributeFallback)),
        TooltipStyle::AttributeBase};
}

// 同一个 apply 的 `amount > 0` 分支：护甲值/盔甲韧性，键 `attribute.modifier.plus.0`，
// 没有前导空格，颜色取 Attribute.Sentiment 的正向色（蓝）。
[[nodiscard]] TooltipLine plusLine(const TooltipContext& context, float amount,
                                   std::string_view attributePath,
                                   std::string_view attributeFallback) {
    return TooltipLine{
        fillTemplate(translate(context, "attribute.modifier.plus.0", "+%s %s"),
                     formatAttributeAmount(amount),
                     attributeName(context, attributePath, attributeFallback)),
        TooltipStyle::AttributeBonus};
}

// 哪些工具类型带主手攻击修饰。26.1 里剪刀/弓/打火石只有耐久，没有
// attack_damage / attack_speed 修饰，提示框因此不给它们出属性段。
[[nodiscard]] bool hasMainhandModifiers(gameplay::ToolType type) {
    switch (type) {
    case gameplay::ToolType::Sword:
    case gameplay::ToolType::Pickaxe:
    case gameplay::ToolType::Axe:
    case gameplay::ToolType::Shovel:
    case gameplay::ToolType::Hoe:
        return true;
    case gameplay::ToolType::None:
    case gameplay::ToolType::Shears:
    case gameplay::ToolType::Bow:
    case gameplay::ToolType::FlintAndSteel:
        return false;
    }
    return false;
}

// `EquipmentSlotGroup#getSerializedName`，护甲那四档——`item.modifiers.<slot>`
// 小标题的键尾。
struct SlotLabel final {
    std::string_view key;
    std::string_view fallback;
};

[[nodiscard]] SlotLabel armorSlotLabel(gameplay::EquipmentSlot slot) {
    switch (slot) {
    case gameplay::EquipmentSlot::Head: return {"item.modifiers.head", "When on Head:"};
    case gameplay::EquipmentSlot::Chest: return {"item.modifiers.chest", "When on Chest:"};
    case gameplay::EquipmentSlot::Legs: return {"item.modifiers.legs", "When on Legs:"};
    case gameplay::EquipmentSlot::Feet: return {"item.modifiers.feet", "When on Feet:"};
    case gameplay::EquipmentSlot::Offhand: break;
    }
    return {"item.modifiers.offhand", "When in Off Hand:"};
}

// `addAttributeTooltips`：一个空行 + `item.modifiers.<槽位>` 小标题 + 逐条修饰。
// 本项目的修饰不是 DataComponents 里的一张表，而是 toolAttributes /
// armorAttributes 这两张 constexpr 表，所以这里按物品形态取值而不是遍历组件；
// 行的顺序、键与颜色仍逐条对齐 vanilla。
void appendAttributeLines(const gameplay::ItemStack& stack, const TooltipContext& context,
                          std::vector<TooltipLine>& lines) {
    const gameplay::Item* item = stack.item;
    if (item == nullptr) {
        return;
    }
    if (hasMainhandModifiers(item->toolType)) {
        const gameplay::ToolAttributes attributes =
            gameplay::toolAttributes(item->toolType, item->toolTier);
        lines.push_back({std::string{}, TooltipStyle::Detail});
        lines.push_back({translate(context, "item.modifiers.mainhand", "When in Main Hand:"),
                         TooltipStyle::Detail});
        lines.push_back(
            equalsLine(context, attributes.attackDamage, "attack_damage", "Attack Damage"));
        lines.push_back(
            equalsLine(context, attributes.attackSpeed, "attack_speed", "Attack Speed"));
        return;
    }
    if (!gameplay::isArmor(item)) {
        return;
    }
    const gameplay::ArmorAttributes attributes =
        gameplay::armorAttributes(item->armorMaterial, item->armorSlot);
    const SlotLabel label = armorSlotLabel(item->armorSlot);
    lines.push_back({std::string{}, TooltipStyle::Detail});
    lines.push_back({translate(context, label.key, label.fallback), TooltipStyle::Detail});
    if (attributes.protection > 0U) {
        lines.push_back(
            plusLine(context, static_cast<float>(attributes.protection), "armor", "Armor"));
    }
    // 修饰量为 0 的那条 vanilla 一行都不出（Display.Default 只在 amount != 0 时
    // 产出），皮革/锁链/铁/金的韧性正是 0。
    if (attributes.toughness > 0.0F) {
        lines.push_back(
            plusLine(context, attributes.toughness, "armor_toughness", "Armor Toughness"));
    }
}

// `ItemEnchantments#addToTooltip`：vanilla 按 `#minecraft:tooltip_order` 这个标签
// 的顺序列举，与栈里的物理存放顺序无关（组件那边是哈希表，这边是定容数组 +
// swap-erase，两者的物理顺序都会漂）。本项目没有 tag 数据，注册表顺序
// （EnchantmentId 的枚举序）就是等价的稳定序：产出只由**集合**决定。
void appendEnchantmentLines(const gameplay::ItemStack& stack, const TooltipContext& context,
                            std::vector<TooltipLine>& lines) {
    std::array<gameplay::EnchantmentInstance, gameplay::kMaxEnchantmentsPerStack> sorted{};
    std::size_t count = 0;
    for (std::uint8_t index = 0; index < stack.enchantmentCount; ++index) {
        const gameplay::EnchantmentInstance entry = stack.enchantments[index];
        // 存档/网络来的 id 有可能越界（旧存档、坏数据），越界就跳过而不是拿它
        // 去下标 kEnchantmentTable。
        if (static_cast<std::size_t>(entry.id) >= gameplay::kEnchantmentCount) {
            continue;
        }
        sorted[count] = entry;
        ++count;
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count),
              [](const gameplay::EnchantmentInstance& first,
                 const gameplay::EnchantmentInstance& second) { return first.id < second.id; });
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = static_cast<gameplay::EnchantmentId>(sorted[index].id);
        lines.push_back({enchantmentLabel(id, static_cast<int>(sorted[index].level), context),
                         gameplay::enchantmentIsCurse(id) ? TooltipStyle::Curse
                                                          : TooltipStyle::Detail});
    }
}

} // namespace

TooltipStyle nameStyle(gameplay::Rarity rarity) {
    switch (rarity) {
    case gameplay::Rarity::Common: return TooltipStyle::NameCommon;
    case gameplay::Rarity::Uncommon: return TooltipStyle::NameUncommon;
    case gameplay::Rarity::Rare: return TooltipStyle::NameRare;
    case gameplay::Rarity::Epic: return TooltipStyle::NameEpic;
    }
    return TooltipStyle::NameCommon;
}

std::string enchantmentLabel(gameplay::EnchantmentId id, int level,
                             const TooltipContext& context) {
    const std::string vanilla{gameplay::enchantmentVanillaName(id)};
    std::string label{translate(context, "enchantment.minecraft." + vanilla, vanilla)};
    // Enchantment#getFullname：等级数字只在"等级不是 1"或"这条附魔的上限不是 1"
    // 时出现（精准采集永远是 "Silk Touch"，不会是 "Silk Touch I"）。
    if (level != 1 || gameplay::enchantmentDefinition(id).maxLevel > 1) {
        label += ' ';
        label +=
            translate(context, "enchantment.level." + std::to_string(level), romanNumeral(level));
    }
    return label;
}

TooltipLine itemNameLine(const gameplay::ItemStack& stack, const TooltipContext& context) {
    return TooltipLine{itemDisplayName(stack, context),
                       nameStyle(gameplay::itemRarity(stack))};
}

std::vector<TooltipLine> itemTooltipLines(const gameplay::ItemStack& stack,
                                          const TooltipContext& context) {
    std::vector<TooltipLine> lines;
    if (stack.empty()) {
        return lines;
    }
    // 顺序即 vanilla 的顺序：名称 → 附魔 → 属性 → advanced。缺的项跳过不占位。
    lines.push_back(itemNameLine(stack, context));
    appendEnchantmentLines(stack, context, lines);
    appendAttributeLines(stack, context, lines);
    if (!context.advanced) {
        return lines;
    }
    // `addDetailsToTooltip` 的 isAdvanced 段。耐久行只在**真的有损耗**时出
    // （vanilla 的 `isDamaged()`），数字是剩余/上限。
    const std::uint16_t maximumDamage = gameplay::itemMaximumDamage(stack);
    if (maximumDamage > 0U && stack.damage > 0U) {
        const auto remaining = static_cast<std::uint16_t>(maximumDamage - stack.damage);
        lines.push_back(
            {fillTemplate(translate(context, "item.durability", "Durability: %s / %s"),
                          std::to_string(remaining), std::to_string(maximumDamage)),
             TooltipStyle::Advanced});
    }
    // 物品 id。vanilla 这行是 `BuiltInRegistries.ITEM.getKey(...)`，本项目的等价
    // 物是描述 id 的那个 Identifier（方块栈已由 itemDescriptionId 归一到它的
    // BlockItem，所以石头显示 minecraft:stone 而不是空）。
    //
    // 与 vanilla 的一处刻意差异：vanilla 的耐久行没有颜色（白），id 行才是暗灰；
    // 这里两行同属 Advanced 一档，F3+H 的附加信息在视觉上成组。
    const gameplay::DescriptionId descriptionId = gameplay::itemDescriptionId(stack);
    if (!descriptionId.empty()) {
        std::string identifier{descriptionId.source.space};
        identifier.push_back(':');
        identifier.append(descriptionId.source.path);
        lines.push_back({std::move(identifier), TooltipStyle::Advanced});
    }
    // vanilla 还会在这里写一行 `item.components`（组件条数）。本项目没有
    // DataComponents（那是 I.comp），这一行留接缝不建空壳。
    return lines;
}

} // namespace mc::ui
