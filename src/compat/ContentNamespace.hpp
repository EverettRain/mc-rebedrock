#pragma once

// ADV-0b：内容标识符的命名空间归一化——**唯一**一处实现。
//
// 口径（用户裁定）：本作自己的底座一律落在 `rebedrock:`，而**每一个解析边界**
// 同时接受 `minecraft:` 与 `rebedrock:`，两者指同一件东西。这样一份 vanilla
// 数据包里的 `minecraft:oak_planks` 配方**覆盖**我们的同名配方，而不是变成第二
// 条重复配方。
//
// 为什么需要这一层：R0 的身份层（block/item/entity）早就双命名空间通吃——
// `world::blockFromIdentifier` / `itemFromIdentifier` /
// `EntityTypeRegistry::byId` 都各自挂了 `minecraft:` 别名（见
// compat/VanillaMapping.hpp:15-17）。没跟上的是**数据文件 id** 那一类：配方 id
// 是裸字符串比较，谁都没给它别名。RecipeBakedData.inc 从前正是半截口径——配方
// id 写 `minecraft:oak_planks`，配料写 `rebedrock:oak_planks`。
//
// ★★ 铁律：归一化必须落在**每一个**解析边界，不是某一个调用点。本仓在
// 「同名 block/item 双端桥」那次栽过一模一样的坑——codec 层不改、只改命令层是
// 无效的，因为另一条入口绕过了修好的那一处。配方 id 今天的边界清单写在
// gameplay/RecipeBook.hpp 与 gameplay/RecipeTable.hpp 的注释里，加新边界时
// 同步补上。
//
// 这里只做命名空间替换，不做别的：一个第三方命名空间（`somemod:foo`）原样返回，
// 无命名空间的裸名（`oak_planks`）也原样返回——补默认命名空间是解析器的事
// （core::Identifier），不是归一化的事，两件事混在一起会让「这个 id 从哪来」
// 不可辨。

#include <string>
#include <string_view>

namespace mc::compat {

// 本作自己的命名空间。烘焙数据、存档、内部查表一律用它。
inline constexpr std::string_view kOwnNamespace = "rebedrock";
// vanilla 的命名空间。数据包与老存档里出现的那个。
inline constexpr std::string_view kVanillaNamespace = "minecraft";

// 「这个 id 带的是 vanilla 命名空间吗」——归一化的**前提判断只此一份**，
// canonicalContentId 自己也走它，抄第二份就是第二个口径。
[[nodiscard]] inline bool isVanillaNamespaced(std::string_view identifier) {
    return identifier.size() > kVanillaNamespace.size() &&
           identifier.substr(0, kVanillaNamespace.size()) == kVanillaNamespace &&
           identifier[kVanillaNamespace.size()] == ':';
}

// `minecraft:foo` -> `rebedrock:foo`；其它一切（已经是 `rebedrock:` 的、第三方
// 命名空间的、裸名的）原样返回。
//
// 返回**拥有**的字符串而不是 view：`minecraft:` 与 `rebedrock:` 长度不同，替换
// 后的串不可能是入参的子串，所以没有零拷贝的诚实写法。调用点都在解析/查表这类
// 冷路径上（装数据包、开配方书、读存档），一次小分配换「只有一处实现」值得。
[[nodiscard]] inline std::string canonicalContentId(std::string_view identifier) {
    if (!isVanillaNamespaced(identifier)) {
        return std::string{identifier};
    }
    std::string canonical;
    canonical.reserve(kOwnNamespace.size() + identifier.size() - kVanillaNamespace.size());
    canonical.append(kOwnNamespace);
    canonical.append(identifier.substr(kVanillaNamespace.size()));
    return canonical;
}

} // namespace mc::compat
