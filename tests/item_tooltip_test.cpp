// I-2 物品展示层的断言。整层是纯值计算（零渲染依赖、零静态状态），
// 所以它的**全部**表面都在这里 headless 断住：行的顺序、颜色语义、稀有度升档、
// 附魔集的稳定序、advanced 的两条附加行、以及缺键时的英文兜底。
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "ui/ItemTooltip.hpp"
#include "ui/Language.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::gameplay::EnchantmentId;
using mc::gameplay::ItemStack;
using mc::ui::TooltipContext;
using mc::ui::TooltipLine;
using mc::ui::TooltipStyle;

// 只放本测试用得上的那些键，其余键一律走兜底——"缺键不出现裸 key"因此也被
// 同一份语言表覆盖到。
mc::ui::Language testLanguage() {
    return mc::ui::Language::fromJsonText(R"({
        "item.minecraft.diamond_sword": "Diamond Sword",
        "item.minecraft.diamond_chestplate": "Diamond Chestplate",
        "item.minecraft.iron_chestplate": "Iron Chestplate",
        "item.minecraft.enchanted_book": "Enchanted Book",
        "item.minecraft.apple": "Apple",
        "block.minecraft.stone": "Stone",
        "enchantment.minecraft.sharpness": "Sharpness",
        "enchantment.minecraft.unbreaking": "Unbreaking",
        "enchantment.minecraft.mending": "Mending",
        "enchantment.minecraft.silk_touch": "Silk Touch",
        "enchantment.minecraft.binding_curse": "Curse of Binding",
        "enchantment.level.1": "I",
        "enchantment.level.2": "II",
        "enchantment.level.3": "III",
        "item.durability": "Durability: %s / %s",
        "item.modifiers.mainhand": "When in Main Hand:",
        "item.modifiers.chest": "When on Chest:",
        "attribute.modifier.equals.0": "%s %s",
        "attribute.modifier.plus.0": "+%s %s",
        "attribute.name.attack_damage": "Attack Damage",
        "attribute.name.attack_speed": "Attack Speed",
        "attribute.name.armor": "Armor",
        "attribute.name.armor_toughness": "Armor Toughness"
    })");
}

ItemStack itemStack(const mc::gameplay::Item& item, std::uint8_t count = 1U) {
    ItemStack stack{};
    stack.count = count;
    stack.item = &item;
    return stack;
}

void enchant(ItemStack& stack, EnchantmentId id, std::uint8_t level) {
    stack.setEnchantmentRaw(static_cast<mc::gameplay::EnchantmentIdStorage>(id), level);
}

[[nodiscard]] std::vector<std::string> texts(const std::vector<TooltipLine>& lines) {
    std::vector<std::string> result;
    result.reserve(lines.size());
    for (const auto& line : lines) {
        result.push_back(line.text);
    }
    return result;
}

[[nodiscard]] bool containsLine(const std::vector<TooltipLine>& lines, std::string_view text) {
    return std::any_of(lines.begin(), lines.end(),
                       [text](const TooltipLine& line) { return line.text == text; });
}

[[nodiscard]] bool anyLineContains(const std::vector<TooltipLine>& lines,
                                   std::string_view fragment) {
    return std::any_of(lines.begin(), lines.end(), [fragment](const TooltipLine& line) {
        return line.text.find(fragment) != std::string::npos;
    });
}

} // namespace

int main() {
    const mc::ui::Language language = testLanguage();
    const TooltipContext plain{/*advanced=*/false, &language};
    const TooltipContext advanced{/*advanced=*/true, &language};

    // 空栈什么都不产出（悬停在空槽位上不该冒出一个空框）。
    {
        assert(mc::ui::itemTooltipLines(ItemStack{}, plain).empty());
    }

    // 普通工具：名称 + 属性段。名称行是白（Common），属性段是 vanilla 的
    // 「空行 + When in Main Hand: + 两条 equals」。
    {
        const ItemStack sword = itemStack(mc::gameplay::items::DiamondSword);
        const auto lines = mc::ui::itemTooltipLines(sword, plain);
        assert(lines.size() == 5U);
        assert(lines[0].text == "Diamond Sword");
        assert(lines[0].style == TooltipStyle::NameCommon);
        assert(lines[1].text.empty());
        assert(lines[2].text == "When in Main Hand:");
        assert(lines[2].style == TooltipStyle::Detail);
        // toolAttributes 的钻石剑是 7.0 / 1.6；"#.##" 的写法去掉尾随零。
        assert(lines[3].text == " 7 Attack Damage");
        assert(lines[3].style == TooltipStyle::AttributeBase);
        assert(lines[4].text == " 1.6 Attack Speed");
        assert(lines[4].style == TooltipStyle::AttributeBase);
    }

    // 数量不进提示框（vanilla 的数量画在槽位上，I-2 删掉了历史遗留的 " xN"）。
    {
        const ItemStack apples = itemStack(mc::gameplay::items::Apple, 17U);
        const auto lines = mc::ui::itemTooltipLines(apples, plain);
        assert(lines.size() == 1U); // 苹果没有属性修饰，只有名字
        assert(lines[0].text == "Apple");
        assert(!anyLineContains(lines, "17"));
        assert(!anyLineContains(lines, " x"));
    }

    // 附魔：名字之后逐条附魔行，灰色。
    {
        ItemStack sword = itemStack(mc::gameplay::items::DiamondSword);
        enchant(sword, EnchantmentId::Sharpness, 3);
        const auto lines = mc::ui::itemTooltipLines(sword, plain);
        assert(lines[1].text == "Sharpness III");
        assert(lines[1].style == TooltipStyle::Detail);
    }

    // Enchantment#getFullname：上限为 1 的附魔不写等级数字。
    {
        ItemStack pickaxe = itemStack(mc::gameplay::items::DiamondPickaxe);
        enchant(pickaxe, EnchantmentId::SilkTouch, 1);
        const auto lines = mc::ui::itemTooltipLines(pickaxe, plain);
        assert(lines[1].text == "Silk Touch");
    }

    // 顺序无关的附魔集产出稳定顺序：同一个集合以不同插入序（并经历
    // setEnchantmentRaw 的 swap-erase 把物理顺序打乱）落到同一份行。
    {
        ItemStack first = itemStack(mc::gameplay::items::DiamondSword);
        enchant(first, EnchantmentId::Sharpness, 3);
        enchant(first, EnchantmentId::Unbreaking, 2);
        enchant(first, EnchantmentId::Mending, 1);

        ItemStack second = itemStack(mc::gameplay::items::DiamondSword);
        enchant(second, EnchantmentId::Mending, 1);
        enchant(second, EnchantmentId::Sharpness, 3);
        // 插入再删掉一条，让尾部经历一次 swap-erase——物理顺序与 first 不同。
        enchant(second, EnchantmentId::Knockback, 1);
        enchant(second, EnchantmentId::Unbreaking, 2);
        enchant(second, EnchantmentId::Knockback, 0);

        assert(second.enchantmentCount == first.enchantmentCount);
        assert(texts(mc::ui::itemTooltipLines(first, plain)) ==
               texts(mc::ui::itemTooltipLines(second, plain)));
        // 注册表序（EnchantmentId 枚举序）：Sharpness < Unbreaking < Mending。
        const auto lines = mc::ui::itemTooltipLines(first, plain);
        assert(lines[1].text == "Sharpness III");
        assert(lines[2].text == "Unbreaking II");
        assert(lines[3].text == "Mending");
    }

    // 诅咒走 Curse，同一件物品上的普通附魔仍是 Detail。
    {
        ItemStack chestplate = itemStack(mc::gameplay::items::DiamondChestplate);
        enchant(chestplate, EnchantmentId::BindingCurse, 1);
        enchant(chestplate, EnchantmentId::Unbreaking, 2);
        const auto lines = mc::ui::itemTooltipLines(chestplate, plain);
        assert(lines[1].text == "Curse of Binding");
        assert(lines[1].style == TooltipStyle::Curse);
        assert(lines[2].text == "Unbreaking II");
        assert(lines[2].style == TooltipStyle::Detail);
    }

    // 稀有度：附魔后名称行升一档（Common → Rare），未附魔的仍是 Common。
    {
        const ItemStack plainSword = itemStack(mc::gameplay::items::DiamondSword);
        ItemStack enchantedSword = plainSword;
        enchant(enchantedSword, EnchantmentId::Sharpness, 1);
        const auto plainName = mc::ui::itemNameLine(plainSword, plain);
        const auto enchantedName = mc::ui::itemNameLine(enchantedSword, plain);
        assert(plainName.style == TooltipStyle::NameCommon);
        assert(enchantedName.style == TooltipStyle::NameRare);
        assert(enchantedName.style > plainName.style);
        assert(mc::ui::itemTooltipLines(enchantedSword, plain)[0].style == enchantedName.style);
    }

    // 附魔书的附魔是"存着的"而不是"生效的"（vanilla 的 STORED_ENCHANTMENTS），
    // 所以它停在自己的基础档 Rare，不会再升到 Epic。
    {
        ItemStack book = itemStack(mc::gameplay::items::EnchantedBook);
        enchant(book, EnchantmentId::Mending, 1);
        assert(mc::ui::itemNameLine(book, plain).style == TooltipStyle::NameRare);
        assert(mc::ui::nameStyle(mc::gameplay::itemRarity(book)) == TooltipStyle::NameRare);
    }

    // 护甲的属性段：`item.modifiers.<槽位>` + 护甲值，韧性为 0 的材质不出那一行。
    {
        const auto diamond =
            mc::ui::itemTooltipLines(itemStack(mc::gameplay::items::DiamondChestplate), plain);
        assert(diamond[1].text.empty());
        assert(diamond[2].text == "When on Chest:");
        assert(diamond[3].text == "+8 Armor");
        assert(diamond[3].style == TooltipStyle::AttributeBonus);
        assert(diamond[4].text == "+2 Armor Toughness");

        const auto iron =
            mc::ui::itemTooltipLines(itemStack(mc::gameplay::items::IronChestplate), plain);
        assert(iron.size() == 4U);
        assert(iron[3].text == "+6 Armor");
        assert(!anyLineContains(iron, "Toughness"));
    }

    // advanced：未损耗的工具不出耐久行，损耗后出且是 剩余/上限。
    {
        ItemStack sword = itemStack(mc::gameplay::items::DiamondSword);
        assert(!anyLineContains(mc::ui::itemTooltipLines(sword, advanced), "Durability"));
        sword.damage = 20U;
        const auto lines = mc::ui::itemTooltipLines(sword, advanced);
        // 钻石工具的耐久上限 1561（toolAttributes）。
        assert(containsLine(lines, "Durability: 1541 / 1561"));
        assert(lines[lines.size() - 2U].text == "Durability: 1541 / 1561");
        assert(lines[lines.size() - 2U].style == TooltipStyle::Advanced);
        // id 行永远是最后一行。
        assert(lines.back().text == "minecraft:diamond_sword");
        assert(lines.back().style == TooltipStyle::Advanced);
    }

    // 非 advanced：既没有 id 行，也没有耐久行。
    {
        ItemStack sword = itemStack(mc::gameplay::items::DiamondSword);
        sword.damage = 20U;
        const auto lines = mc::ui::itemTooltipLines(sword, plain);
        assert(!anyLineContains(lines, "minecraft:"));
        assert(!anyLineContains(lines, "Durability"));
    }

    // 方块栈：名字经 BlockItem 归一（legacy 的空 item 指针也认），id 行也是它。
    {
        ItemStack stone{};
        stone.block = mc::world::Block::Stone;
        stone.count = 1U;
        const auto lines = mc::ui::itemTooltipLines(stone, advanced);
        assert(lines[0].text == "Stone");
        assert(lines[0].style == TooltipStyle::NameCommon);
        assert(lines.back().text == "minecraft:stone");
        assert(!anyLineContains(lines, "Durability")); // 方块没有耐久
    }

    // 缺键一律英文兜底，任何一行都不会露出裸 key。
    {
        const mc::ui::Language empty = mc::ui::Language::fromJsonText("{}");
        const TooltipContext bare{/*advanced=*/true, &empty};
        ItemStack sword = itemStack(mc::gameplay::items::DiamondSword);
        sword.damage = 20U;
        enchant(sword, EnchantmentId::Sharpness, 3);
        const auto lines = mc::ui::itemTooltipLines(sword, bare);
        assert(lines[0].text == "diamond_sword"); // Item#getDescriptionId 的路径段
        assert(lines[1].text == "sharpness III"); // 附魔名兜底 + 罗马数字兜底
        assert(containsLine(lines, "When in Main Hand:"));
        assert(containsLine(lines, " 7 Attack Damage"));
        assert(containsLine(lines, "Durability: 1541 / 1561"));
        assert(!anyLineContains(lines, "enchantment."));
        assert(!anyLineContains(lines, "attribute."));
        assert(!anyLineContains(lines, "item.durability"));
        assert(!anyLineContains(lines, "item.modifiers"));
    }

    // language 为空指针（无资源包启动、纯测试装配）同样兜底，不崩。
    {
        const TooltipContext none{/*advanced=*/false, nullptr};
        const auto lines = mc::ui::itemTooltipLines(itemStack(mc::gameplay::items::Apple), none);
        assert(lines.size() == 1U);
        assert(lines[0].text == "apple");
    }

    return 0;
}
