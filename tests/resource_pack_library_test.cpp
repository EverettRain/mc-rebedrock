// 资源包选择的后端：包身份、启用名单的落盘、以及「提交只改草稿不换栈」这条契约。
//
// 这条测试要钉住的是四件在界面上看不出来、坏了也不报错的事：
//
//  1. **旧行为不变**：没有启用名单文件时，扫描到的包全部启用、顺序即扫描顺序。
//     这一支覆盖所有旧游戏目录；如果哪天改成「首次运行就把名单写出来」，
//     后来放进 resourcepacks/ 的包会默默不生效。
//  2. **id 是真身份**：id 用目录名/文件名而不是 pack0/pack1。序号 id 一旦落盘，
//     玩家增删一个包就会让名单整体错位，指到别的包上去——而且不会有任何报错。
//  3. **顺序即优先级**，且和 LayeredResourceProvider 真实解析出来的字节一致。
//     order 是自下而上的，buildProvider 那一步要把它翻过来；调序只改草稿的话，
//     调完再 buildStack 出来的栈必须照新顺序解析。
//  4. **提交不换栈**：commit() 之后 provider() 解析到的仍是启动时那一份，
//     restartRequired 为真。做成热重载之前，这是「开关没静默生效」的唯一保证。

#include "assets/ResourcePackLibrary.hpp"
#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mc::assets::ResourcePackEntry;
using mc::assets::ResourcePackLibrary;
using mc::assets::StandardPackResourceProvider;

void writeFile(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

[[nodiscard]] std::string readFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// 解析 stone.png 得到的字节，用来判定"哪个包赢了这次查询"
[[nodiscard]] std::string resolvedStone(const mc::assets::ResourceProvider& provider) {
    const auto bytes = provider.readBytes(mc::assets::textures("block/stone.png"));
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::vector<std::string> idsOf(const std::vector<ResourcePackEntry>& packs) {
    std::vector<std::string> ids;
    ids.reserve(packs.size());
    for (const auto& pack : packs) {
        ids.push_back(pack.id);
    }
    return ids;
}

} // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "rebedrock_resource_pack_library_test";
    std::error_code cleanup;
    fs::remove_all(tmp, cleanup);

    // 夹具：一个内置底座 + 三个各自覆盖 stone.png 的包，字节各不相同，
    // 于是"谁赢了"是可以直接读出来的，而不是靠推断栈里的指针顺序
    const fs::path baseRoot = tmp / "bundled";
    writeFile(baseRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png", "bundled");
    const StandardPackResourceProvider bundled{baseRoot};

    const fs::path alphaRoot = tmp / "resourcepacks" / "alpha";
    writeFile(alphaRoot / "pack.mcmeta",
              R"({"pack": {"pack_format": 84, "description": "阿尔法包"}})");
    writeFile(alphaRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png", "alpha");

    const fs::path bravoRoot = tmp / "resourcepacks" / "bravo";
    writeFile(bravoRoot / "pack.mcmeta", R"({"pack": {"min_format": 80, "max_format": 84}})");
    writeFile(bravoRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png", "bravo");

    const fs::path charlieRoot = tmp / "resourcepacks" / "charlie";
    writeFile(charlieRoot / "pack.mcmeta",
              R"({"pack": {"pack_format": 3, "description": "太老的包"}})");
    writeFile(charlieRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png", "charlie");

    const StandardPackResourceProvider alpha{alphaRoot};
    const StandardPackResourceProvider bravo{bravoRoot};
    const StandardPackResourceProvider charlie{charlieRoot};

    const fs::path selectionFile = tmp / "config" / "resourcepacks.txt";

    const auto populate = [&](ResourcePackLibrary& library) {
        // 元数据从 provider 自己解析出来的那一份取，与启动流程做法一致
        library.addPack(ResourcePackEntry{"alpha", "alpha", alpha.metadata().description,
                                          alpha.metadata().minFormat, alpha.metadata().maxFormat,
                                          true, false},
                        alpha);
        library.addPack(ResourcePackEntry{"bravo", "bravo", bravo.metadata().description,
                                          bravo.metadata().minFormat, bravo.metadata().maxFormat,
                                          true, false},
                        bravo);
        library.addPack(ResourcePackEntry{"charlie", "charlie", charlie.metadata().description,
                                          charlie.metadata().minFormat,
                                          charlie.metadata().maxFormat, false, false},
                        charlie);
    };

    // --- 1. 真实身份：id/标题/描述/格式范围都来自磁盘 ---
    {
        ResourcePackLibrary library{selectionFile};
        populate(library);
        assert(idsOf(library.packs()) == (std::vector<std::string>{"alpha", "bravo", "charlie"}));
        const auto* entry = library.find("alpha");
        assert(entry != nullptr);
        // 描述是包自己 pack.mcmeta 里写的那一句，不是空串——从前这里传的是
        // 一份默认构造的 PackMetadata，界面上永远只有一个 "pack0"
        assert(entry->description == "阿尔法包");
        assert(entry->minFormat == 84 && entry->maxFormat == 84);
        // 单值 pack_format 会归一成 min == max；范围形态则原样保留
        const auto* range = library.find("bravo");
        assert(range != nullptr);
        assert(range->minFormat == 80 && range->maxFormat == 84);
        assert(range->description.empty()); // 包没写 description
        assert(library.find("pack0") == nullptr);
    }

    // --- 1b. 身份由磁盘路径导出，不是序号 ---
    {
        ResourcePackLibrary library{selectionFile};
        // ★ 落盘的键必须是包自己的名字。序号 id（pack0/pack1）撑不住玩家增删一个包：
        //   名单会整体错位、指到别的包上，而且不会有任何报错
        assert(library.deriveId(alphaRoot) == "alpha");
        assert(library.deriveId(fs::path{"/somewhere/漂亮材质.zip"}) == "漂亮材质.zip");
        assert(library.deriveId(fs::path{"/somewhere/alpha/"}) == "alpha");
        // 撞名：两个都叫 alpha 的包不能共用一个 id
        library.addPack(ResourcePackEntry{"alpha", "alpha", "", 84, 84, true, false}, alpha);
        assert(library.deriveId(fs::path{"/别处/alpha"}) == "alpha (2)");
        // 标题去掉 .zip；目录包原样
        assert(ResourcePackLibrary::titleFromId("漂亮材质.zip") == "漂亮材质");
        assert(ResourcePackLibrary::titleFromId("alpha") == "alpha");
    }

    // --- 2. 没有名单文件 = 全部启用，顺序即扫描顺序（旧行为） ---
    {
        assert(!fs::exists(selectionFile));
        ResourcePackLibrary library{selectionFile};
        populate(library);
        library.loadSelection();
        assert(library.draftOrder() ==
               (std::vector<std::string>{"alpha", "bravo", "charlie"}));
        assert(library.activeOrder() == library.draftOrder());
        assert(!library.draftDiffersFromActive());
        // ★ 只是读一次名单不该把文件创建出来：写了就等于把当前这批包冻结成名单，
        //   之后新放进 resourcepacks/ 的包会默认不启用
        assert(!fs::exists(selectionFile));

        library.buildStack(bundled);
        // 自下而上：charlie 在最上面，赢
        assert(resolvedStone(library.provider()) == "charlie");
    }

    // --- 3. 启停 / 调序 / 提交 ---
    {
        ResourcePackLibrary library{selectionFile};
        populate(library);
        library.loadSelection();
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "charlie");

        // 启停
        assert(library.setEnabled("charlie", false));
        assert(!library.isEnabled("charlie"));
        assert(!library.setEnabled("charlie", false)); // 重复停用不算变化
        assert(!library.setEnabled("不存在的包", true));
        assert(library.draftOrder() == (std::vector<std::string>{"alpha", "bravo"}));

        // 调序：Up = 提高优先级 = 往栈顶挪
        assert(library.movePriorityUp("alpha"));
        assert(library.draftOrder() == (std::vector<std::string>{"bravo", "alpha"}));
        assert(!library.movePriorityUp("alpha")); // 已在栈顶
        assert(library.movePriorityDown("alpha"));
        assert(library.draftOrder() == (std::vector<std::string>{"alpha", "bravo"}));
        assert(!library.movePriorityDown("alpha")); // 已在栈底
        assert(!library.movePriorityUp("charlie")); // 没启用的包不参与排序

        // ★ 改草稿绝不能悄悄换掉正在用的栈：charlie 刚被停用，但本次运行仍是它赢
        assert(resolvedStone(library.provider()) == "charlie");

        // 提交
        const auto outcome = library.commit();
        assert(outcome.written);
        assert(outcome.error.empty());
        assert(outcome.restartRequired); // 与生效集合不同 → 界面要提示重启
        // 提交之后依然不换栈
        assert(resolvedStone(library.provider()) == "charlie");
        assert(library.activeOrder() ==
               (std::vector<std::string>{"alpha", "bravo", "charlie"}));

        // 落盘内容：注释 + 一行一个 id，自下而上
        const auto text = readFile(selectionFile);
        assert(ResourcePackLibrary::parseSelection(text) ==
               (std::vector<std::string>{"alpha", "bravo"}));
        // id 必须是真名字。序号 id 落了盘，玩家一删包名单就整体错位
        assert(text.find("alpha") != std::string::npos);
        assert(text.find("pack0") == std::string::npos);
    }

    // --- 4. 下一次启动读回名单：只启用列到的，顺序照文件 ---
    {
        ResourcePackLibrary library{selectionFile};
        populate(library);
        library.loadSelection();
        assert(library.draftOrder() == (std::vector<std::string>{"alpha", "bravo"}));
        assert(!library.draftDiffersFromActive());
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "bravo");
        // 名单里没有的包仍然列得出来，只是没启用——这正是「可用」那一列
        assert(library.packs().size() == 3U);
        assert(!library.isEnabled("charlie"));
    }

    // --- 5. 名单里的顺序真的决定谁赢，而不只是 draftOrder 的字面顺序 ---
    {
        writeFile(selectionFile, "bravo\nalpha\n");
        ResourcePackLibrary library{selectionFile};
        populate(library);
        library.loadSelection();
        assert(library.draftOrder() == (std::vector<std::string>{"bravo", "alpha"}));
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "alpha");
    }

    // --- 6. 名单里的幽灵条目与重复行 ---
    {
        writeFile(selectionFile,
                  "# 注释\n"
                  "\n"
                  "  bravo  \n"      // 前后空白被去掉
                  "已经删掉的包\n"    // 磁盘上不存在 → 丢弃，不留幽灵
                  "bravo\n"          // 重复行 → 只算一次
                  "alpha\n");
        ResourcePackLibrary library{selectionFile};
        populate(library);
        library.loadSelection();
        assert(library.draftOrder() == (std::vector<std::string>{"bravo", "alpha"}));
    }

    // --- 7. --pack 点名的包：强制启用、强制置顶、关不掉 ---
    {
        writeFile(selectionFile, "alpha\n");
        ResourcePackLibrary library{selectionFile};
        library.addPack(ResourcePackEntry{"alpha", "alpha", "", 84, 84, true, false}, alpha);
        library.addPack(ResourcePackEntry{"bravo", "bravo", "", 84, 84, true, false}, bravo);
        // charlie 被命令行点名，尽管名单里没有它
        library.addPack(ResourcePackEntry{"charlie", "charlie", "", 3, 3, false, true}, charlie);
        library.loadSelection();
        assert(library.draftOrder() == (std::vector<std::string>{"alpha", "charlie"}));
        assert(!library.setEnabled("charlie", false));
        assert(library.isEnabled("charlie"));
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "charlie");
    }

    // --- 7b. 草稿与生效是**两份**：改草稿不动生效值，commit 才合流 ---
    //
    // ★ 这是这个类的核心语义（26.1 的"浏览时只改草稿，Done 才提交"），而它
    //   **第一轮 sabotage 没被抓住**：让 setEnabled 顺手也改 active_，
    //   上面那些断言一条都不会红——它们全在问草稿，没有一条问过生效值。
    //   一个只读草稿的测试，对"草稿泄漏进生效值"是瞎的。
    {
        writeFile(selectionFile, "alpha\n");
        ResourcePackLibrary library{selectionFile};
        library.addPack(ResourcePackEntry{"alpha", "alpha", "", 84, 84, true, false}, alpha);
        library.addPack(ResourcePackEntry{"bravo", "bravo", "", 84, 84, true, false}, bravo);
        library.loadSelection();
        const auto activeAtStart = library.activeOrder();
        assert(activeAtStart == (std::vector<std::string>{"alpha"}));
        assert(!library.draftDiffersFromActive());

        // 改草稿：生效值**必须原封不动**
        assert(library.setEnabled("bravo", true));
        assert(library.draftOrder() == (std::vector<std::string>{"alpha", "bravo"}));
        assert(library.activeOrder() == activeAtStart);
        assert(library.draftDiffersFromActive());

        // 调序同样只动草稿
        assert(library.movePriorityUp("alpha"));
        assert(library.activeOrder() == activeAtStart);

        // 放弃草稿后两者重新一致
        library.discardDraft();
        assert(library.draftOrder() == activeAtStart);
        assert(!library.draftDiffersFromActive());

        // ★ 提交**不**把 active 合流过来——这一条反直觉，但它正是
        //   `restartRequired` 能成立的原因：换包不做热重载，`activeOrder()` 的语义是
        //   "**本次运行**实际生效的那一份"（启动时装配的），而 draft 是写进文件、
        //   下次启动才生效的。commit 后若把 active 同步了，`draftDiffersFromActive()`
        //   立刻变成假，`restartRequired` 就永远是 false，界面再也不会提示"重启后生效"。
        //   （我第一次写这条测试时按"提交即合流"断言，红了才发现设计是对的、断言是错的。）
        assert(library.setEnabled("bravo", true));
        assert(library.draftDiffersFromActive());
        const auto result = library.commit();
        assert(result.written);
        assert(result.restartRequired);
        assert(library.activeOrder() == activeAtStart);      // 本次运行仍是老那份
        assert(library.draftOrder() != library.activeOrder());
        // 再次 commit 仍然说要重启（差异还在，直到进程重启）
        assert(library.commit().restartRequired);
    }

    // --- 8. 一个包的 overlay 只叠在这个包之上，不越过下一个包 ---
    {
        // alpha 带一个 overlay（覆盖 stone.png），bravo 叠在 alpha 之上。
        // 若 overlay 被当成独立的一层接在整条栈的最顶上，赢的会是 overlay。
        const fs::path overlayRoot = alphaRoot / "drop26_1";
        writeFile(overlayRoot / "assets" / "minecraft" / "textures" / "block" / "stone.png",
                  "alpha-overlay");
        const StandardPackResourceProvider alphaOverlay{overlayRoot};

        ResourcePackLibrary library{tmp / "config" / "overlay-selection.txt"};
        library.addPack(ResourcePackEntry{"alpha", "alpha", "", 84, 84, true, false}, alpha,
                        {&alphaOverlay});
        library.addPack(ResourcePackEntry{"bravo", "bravo", "", 84, 84, true, false}, bravo);
        library.loadSelection();
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "bravo");

        // 反过来把 alpha 挪到栈顶，这时才轮到它的 overlay 赢
        assert(library.movePriorityUp("alpha"));
        library.buildStack(bundled);
        assert(resolvedStone(library.provider()) == "alpha-overlay");
        // overlay 不是包，不出现在界面列表里
        assert(library.packs().size() == 2U);
    }

    // --- 9. 写不回来的 id 宁可拒绝落盘 ---
    {
        const fs::path file = tmp / "config" / "hostile.txt";
        ResourcePackLibrary library{file};
        library.addPack(ResourcePackEntry{"#像注释的包", "x", "", 84, 84, true, false}, alpha);
        library.loadSelection();
        const auto outcome = library.commit();
        assert(!outcome.written);
        assert(!outcome.error.empty());
        assert(!fs::exists(file));
        assert(!ResourcePackLibrary::idIsPersistable("#x"));
        assert(!ResourcePackLibrary::idIsPersistable("a\nb"));
        assert(!ResourcePackLibrary::idIsPersistable(" a"));
        assert(!ResourcePackLibrary::idIsPersistable(""));
        assert(ResourcePackLibrary::idIsPersistable("寻常包.zip"));
    }

    // --- 10. 放弃草稿回到生效那一份 ---
    {
        ResourcePackLibrary library{tmp / "config" / "discard.txt"};
        populate(library);
        library.loadSelection();
        library.setEnabled("alpha", false);
        assert(library.movePriorityUp("bravo")); // [bravo, charlie] -> [charlie, bravo]
        assert(library.draftOrder() == (std::vector<std::string>{"charlie", "bravo"}));
        assert(library.draftDiffersFromActive());
        library.discardDraft();
        assert(!library.draftDiffersFromActive());
        assert(library.draftOrder() ==
               (std::vector<std::string>{"alpha", "bravo", "charlie"}));
    }

    // --- 11. 兼容性判定量的是 JE 的尺子，不是 ReBedrock 自己的世代号 ---
    {
        // 界面上那个「不兼容」标记来自这里。拿 core::kVersion.packVersion（1/1，
        // ReBedrock 自己的世代号）去量一个 JE 资源包，每一个真包都会被判成不兼容
        const auto modern = mc::assets::PackManager::checkCompatibility(
            alpha.metadata(), mc::assets::PackStackKind::Resources,
            mc::assets::kTargetJavaPackVersion);
        assert(modern.compatible); // alpha 声明 84 = 26.1 的资源包格式
        const auto ancient = mc::assets::PackManager::checkCompatibility(
            charlie.metadata(), mc::assets::PackStackKind::Resources,
            mc::assets::kTargetJavaPackVersion);
        assert(!ancient.compatible); // charlie 声明 3，确实太老
    }

    fs::remove_all(tmp, cleanup);
    return 0;
}
