// UI-2：界面截图通道的纯逻辑。
//
// 通道本身要 GPU，这里测的是它**不需要 GPU 的那一半**：命令行解析与输出路径。
// 两者都直接关系到那条验收条件——"同一条命令行跑两遍逐字节相同"：
//   - 路径必须只是命令行的函数。哪怕文件名里混进一点运行期状态，第二遍就写到别处去了，
//     而 diff -r 会把它报成"只在一边存在的文件"，看起来像通道坏了。
//   - 参数写错必须抛。悄悄拍了另一个屏幕再退出 0，是自动化对照最坏的结果。

#include "render/UiCapture.hpp"
#include "render/UiCaptureFixture.hpp"

#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/TitleScreenLayout.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef MC_REBEDROCK_HUD_RENDERER_SRC
#error "MC_REBEDROCK_HUD_RENDERER_SRC must point at src/render/vulkan/HudRenderer.hpp"
#endif

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("ui_capture_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

[[nodiscard]] std::optional<mc::render::UiCaptureOptions> parse(
    const std::vector<std::string_view>& arguments) {
    return mc::render::parseUiCaptureArguments(arguments);
}

// 一个前端页面的目标（容器为空）。
[[nodiscard]] constexpr mc::render::UiCaptureTarget pageTarget(mc::ui::PageId page) {
    return mc::render::UiCaptureTarget{page};
}

// 一块容器界面的目标。容器屏全挂在 PageId::Game 之上——它们不是 PageId。
[[nodiscard]] constexpr mc::render::UiCaptureTarget containerTarget(
    mc::gameplay::ContainerScreen screen, bool creative = false, bool catalog = false) {
    return mc::render::UiCaptureTarget{mc::ui::PageId::Game, screen, creative, catalog};
}

// 解析这组参数必须抛，且抛的是 invalid_argument。
void expectThrows(const std::vector<std::string_view>& arguments, const char* what, int line) {
    try {
        static_cast<void>(mc::render::parseUiCaptureArguments(arguments));
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        std::printf("ui_capture_test line %d: %s threw the wrong type: %s\n", line, what,
                    error.what());
        ++failures;
        return;
    }
    std::printf("ui_capture_test line %d: %s did not throw\n", line, what);
    ++failures;
}

// 可变参数：参数表里带逗号的花括号初始化列表，单参数宏是接不住的。
#define EXPECT_THROWS(...)                                                                     \
    expectThrows(std::vector<std::string_view>__VA_ARGS__, #__VA_ARGS__, __LINE__)

// --- 1. 没有 --ui-shot 就不是一次截图运行 -------------------------------------
void testAbsent() {
    CHECK(!parse({}).has_value());
    CHECK(!parse({"--test-scene", "stone", "--export-preview"}).has_value());
    // 只给拍摄参数却不说拍什么：报错，而不是默默不拍。
    EXPECT_THROWS({"--ui-scale", "2"});
    EXPECT_THROWS({"--ui-size", "800x600"});
    EXPECT_THROWS({"--ui-out", "/tmp/x"});
}

// --- 2. 默认值 ---------------------------------------------------------------
void testDefaults() {
    const auto parsed = parse({"--ui-shot", "title"});
    CHECK(parsed.has_value());
    CHECK(parsed->targets.size() == 1U);
    CHECK(parsed->targets.front() == pageTarget(mc::ui::PageId::Title));
    // ★ 默认就拍两档：同一个屏幕在不同 GUI scale 下是不同的版面，只拍一档等于没拍。
    CHECK(parsed->guiScales.size() >= 2U);
    CHECK(parsed->width == 1280U);
    CHECK(parsed->height == 720U);
    CHECK(mc::render::uiCaptureImageCount(*parsed) ==
          parsed->targets.size() * parsed->guiScales.size());
}

// --- 3. 页名 ---------------------------------------------------------------
void testPageNames() {
    // 名字与 PageId 双向一致：一张表两个方向读，两边不可能各说各话。
    // ★ 上界用 `Count` 哨兵，**不是**"当时的最后一个枚举值"。这里原本写的是
    //   `<= PageId::AdvancedGraphics`——而 AdvancedGraphics 后面还有 KeyBinds /
    //   Accessibility / SoundSettings / ResourcePacks 四页，它们的双向映射
    //   **从来没有被这条循环检查过**（README 护栏 25 的同一个招，第三次出现）。
    for (int raw = 0; raw < static_cast<int>(mc::ui::PageId::Count); ++raw) {
        const auto page = static_cast<mc::ui::PageId>(raw);
        const auto name = mc::render::uiCaptureTargetName(pageTarget(page));
        CHECK(name != "unknown");
        const auto roundTrip = mc::render::uiCaptureTargetFromName(name);
        CHECK(roundTrip.has_value() && *roundTrip == pageTarget(page));
    }
    CHECK(!mc::render::uiCaptureTargetFromName("Title").has_value());     // 大小写敏感
    CHECK(!mc::render::uiCaptureTargetFromName("world_list").has_value()); // 分词用短横线

    const auto parsed = parse({"--ui-shot", "title,options", "--ui-shot", "language"});
    CHECK(parsed.has_value());
    CHECK(parsed->targets.size() == 3U);
    CHECK(parsed->targets[0] == pageTarget(mc::ui::PageId::Title));
    CHECK(parsed->targets[1] == pageTarget(mc::ui::PageId::Options));
    CHECK(parsed->targets[2] == pageTarget(mc::ui::PageId::Language));

    EXPECT_THROWS({"--ui-shot", "not-a-screen"});
    EXPECT_THROWS({"--ui-shot", "title,title"});            // 同一页给两次
    EXPECT_THROWS({"--ui-shot", "title", "--ui-shot", "title"});
    EXPECT_THROWS({"--ui-shot"});                            // 缺参数
    // ★ UI-6-0：需要世界的四页**不再被拒绝**。渲染器为它们打开一份固定的世界夹具
    //   （一片石台加四根柱子、常量相机位姿、不起模拟线程），拍完靠 worldReady 关掉。
    //   从前这里是四条 EXPECT_THROWS，理由写的是"世界内容不是命令行的函数"——
    //   那句话对的是*真实*世界，不对夹具。
    for (const char* name : {"game", "pause", "death", "loading"}) {
        const auto worldPage = parse({"--ui-shot", name});
        check(worldPage.has_value(),
              std::string{"--ui-shot "} + name + " must be accepted", __LINE__);
    }
    // 混着给也可以：无世界的页面会整屏铺全景，把夹具的世界画面盖掉。
    const auto mixed = parse({"--ui-shot", "title,pause"});
    CHECK(mixed.has_value());
    CHECK(mixed->targets.size() == 2U);
    CHECK(mixed->targets[0] == pageTarget(mc::ui::PageId::Title));
    CHECK(mixed->targets[1] == pageTarget(mc::ui::PageId::Pause));

    // 哪些页要夹具，以及拍它们时游戏暂不暂停。两张表必须互相说得通：
    // 只有游戏内 HUD 是"世界在跑"的那一页，其余三页都是盖在世界上的界面。
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Pause));
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Game));
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Death));
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Loading));
    CHECK(!mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Title));
    CHECK(!mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Options));
    // ★ 游戏内 HUD 是唯一不暂停的一页。把它也判成暂停，拍到的就是暂停菜单，
    //   而那张图看起来"也挺对"——它确实是一张正确的暂停菜单，只是放错了文件名。
    CHECK(!mc::render::uiCapturePageIsPaused(mc::ui::PageId::Game));
    CHECK(mc::render::uiCapturePageIsPaused(mc::ui::PageId::Pause));
    CHECK(mc::render::uiCapturePageIsPaused(mc::ui::PageId::Death));
    // 前端页面本来就没有世界在跑，暂停与否对它们无意义，但取值仍要是良定义的
    CHECK(mc::render::uiCapturePageIsPaused(mc::ui::PageId::Title));

    // ★「要夹具」与「看得见世界」不是同一个问题，loading 是那个差集。
    //   给 loading 也开 worldReady，drawHud 会跳过 `!worldReady` 那条分支，
    //   loading/scale-N.png 里拍到的是游戏内 HUD——一张完全正确的 HUD，
    //   只是文件名写着 loading，而那种错误在自动对照里最不容易发现。
    CHECK(mc::render::uiCapturePageShowsWorld(mc::ui::PageId::Game));
    CHECK(mc::render::uiCapturePageShowsWorld(mc::ui::PageId::Pause));
    CHECK(mc::render::uiCapturePageShowsWorld(mc::ui::PageId::Death));
    CHECK(!mc::render::uiCapturePageShowsWorld(mc::ui::PageId::Loading));
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Loading));
    // 看得见世界的页面必然需要夹具；反过来不成立（loading）
    for (std::size_t i = 0; i < static_cast<std::size_t>(mc::ui::PageId::Count); ++i) {
        const auto page = static_cast<mc::ui::PageId>(i);
        if (mc::render::uiCapturePageShowsWorld(page)) {
            check(mc::render::uiCapturePageNeedsWorld(page),
                  "a page that shows the world must ask for the fixture", __LINE__);
        }
    }
}


// --- 3c. A0-0：容器界面也是拍摄目标 -------------------------------------------
//
// 容器屏此前一张都拍不到，原因是拍摄目标的类型是 PageId，而背包/箱子/工作台
// **不是** PageId——它们是 PageId::Game 之上的 `inventoryOpen + containerScreen`。
// 于是 A 路线要改的那 1351 行零测试代码，连一张能对照的照片都没有。
void testContainerTargets() {
    // 每一块容器界面都点得到名。★ 上界用 ContainerScreen::Count 哨兵：
    //   写"当时的最后一个枚举值"（Anvil）会在追加第七块屏时静默通过。
    for (int raw = 0; raw < static_cast<int>(mc::gameplay::ContainerScreen::Count); ++raw) {
        const auto screen = static_cast<mc::gameplay::ContainerScreen>(raw);
        const auto name = mc::render::uiCaptureTargetName(containerTarget(screen));
        check(name != "unknown",
              "every ContainerScreen must have a capture name", __LINE__);
        const auto roundTrip = mc::render::uiCaptureTargetFromName(name);
        check(roundTrip.has_value() && *roundTrip == containerTarget(screen),
              "container target names must round-trip", __LINE__);
    }

    // ★ 创造背包是 PlayerInventory 的**创造那一档**，不是第七块屏。两个目标同一块
    //   容器、不同的 creative，因此必须是两个**不同**的名字与两条不同的输出路径——
    //   否则第二张图会覆盖第一张，而两遍比对仍然全绿（少的那一张从来没存在过）。
    const auto survival = containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, false);
    const auto creative = containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, true);
    CHECK(!(survival == creative));
    CHECK(mc::render::uiCaptureTargetName(survival) != mc::render::uiCaptureTargetName(creative));
    CHECK(mc::render::uiCaptureTargetName(creative) == "inventory-creative");

    // ★ 创造背包的两个页签也是两个目标：背包页签画的是玩家 36 格 + 护甲 + 删除框，
    //   内容页签画的是 45 格只读目录 + 页签行 + 滚动条——两支几乎没有交集。
    //   只拍一支，另一支在 A1 拆分绘制链时一张可对照的图都没有。
    const auto catalog =
        containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, true, true);
    CHECK(!(catalog == creative));
    CHECK(mc::render::uiCaptureTargetName(catalog) == "creative-catalog");
    const auto parsedCatalog = parse({"--ui-shot", "creative-catalog"});
    CHECK(parsedCatalog.has_value() && parsedCatalog->targets.front() == catalog);
    // 生存背包没有页签这一说：那一维只在 creative 为真时有意义，所以它不该
    // 给生存目标造出第二个名字来。
    CHECK(mc::render::uiCaptureTargetName(
              containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, false, true)) ==
          "unknown");

    // 前端页面的目标与"同一页 + 开着容器"是两个目标：game 与 inventory 都挂在
    // PageId::Game 上，只有容器那一维分得开它们。
    CHECK(mc::render::uiCaptureTargetName(pageTarget(mc::ui::PageId::Game)) == "game");
    CHECK(mc::render::uiCaptureTargetName(survival) == "inventory");

    // 命令行认得它们，而且能与前端页面混着给。
    const auto parsed = parse({"--ui-shot", "title,inventory,chest,inventory-creative"});
    CHECK(parsed.has_value());
    CHECK(parsed->targets.size() == 4U);
    CHECK(parsed->targets[1] == survival);
    CHECK(parsed->targets[2] == containerTarget(mc::gameplay::ContainerScreen::Chest));
    CHECK(parsed->targets[3] == creative);
    EXPECT_THROWS({"--ui-shot", "inventory,inventory"});

    // 容器屏画在世界之上，所以它要夹具、看得见世界、而且**不暂停**（paused 会让
    // drawHud 掉进暂停菜单那条分支，拍到的是一张正确的暂停菜单，只是文件名写着
    // inventory）。这三条都由 PageId::Game 那一页答，容器只是叠在它上面的一层。
    for (int raw = 0; raw < static_cast<int>(mc::gameplay::ContainerScreen::Count); ++raw) {
        const auto target = containerTarget(static_cast<mc::gameplay::ContainerScreen>(raw));
        check(mc::render::uiCapturePageNeedsWorld(target.page),
              "a container target must ask for the world fixture", __LINE__);
        check(mc::render::uiCapturePageShowsWorld(target.page),
              "a container target must show the world", __LINE__);
        check(!mc::render::uiCapturePageIsPaused(target.page),
              "a container target must not be captured paused", __LINE__);
    }
}

// --- 3d. A0-0：容器夹具的内容 -------------------------------------------------
//
// ★ 这些断言钉的不是"某个格子里放的是钻石镐"，而是**每一条绘制路径都有东西喂给它**。
//   一张全空的背包截图看着也"挺对"，但它对图标、堆叠数字、耐久条、附魔提示框
//   一条都没说——而那正是 A1 要改的那 825 行绘制代码。
void testContainerFixture() {
    // 非容器目标拿到的是默认快照：既有十八屏的基线因此逐字节不变。
    const auto frontend = mc::render::uiCaptureWorldSnapshot(pageTarget(mc::ui::PageId::Title));
    CHECK(frontend == mc::gameplay::WorldSnapshot{});

    for (int raw = 0; raw < static_cast<int>(mc::gameplay::ContainerScreen::Count); ++raw) {
        const auto screen = static_cast<mc::gameplay::ContainerScreen>(raw);
        const auto snapshot = mc::render::uiCaptureWorldSnapshot(containerTarget(screen));
        // 打开的那一屏必须是目标说的那一屏——渲染器就是从这个字段派生
        // uiFrameData_.containerScreen 的。
        check(snapshot.openContainerScreen == screen,
              "the fixture must open the target's screen", __LINE__);

        // 每一屏都画玩家自己那 36 格，所以它们在每一屏都要有内容。
        std::size_t filled = 0;
        std::size_t stacked = 0;
        std::size_t damaged = 0;
        std::size_t enchanted = 0;
        std::size_t blockIcons = 0;
        std::size_t itemIcons = 0;
        for (const auto& stack : snapshot.inventorySlots) {
            if (stack.empty()) continue;
            ++filled;
            if (stack.count > 1U) ++stacked;
            if (stack.damage > 0U) ++damaged;
            if (stack.enchantmentCount > 0U) ++enchanted;
            if (stack.item == nullptr) ++blockIcons; else ++itemIcons;
        }
        check(filled >= 12U, "the fixture must fill most of the player's slots", __LINE__);
        // 每一条都对应一条只有它才走得到的绘制路径。
        check(stacked > 0U, "a stack of >1 draws the count text", __LINE__);
        check(damaged > 0U, "a damaged tool draws the durability bar", __LINE__);
        check(enchanted > 0U, "an enchanted item drives the tooltip's extra lines", __LINE__);
        // ★ 方块图标与物品图标是**两条管线**（hudBlockIconPipeline 与图集分支）。
        //   只填一类，另一条管线的图一张都没拍到。
        check(blockIcons > 0U, "block icons go through their own pipeline", __LINE__);
        check(itemIcons > 0U, "item icons go through the atlas path", __LINE__);
        // 留白也要有：空槽画成什么样同样是一条绘制路径。
        check(filled < snapshot.inventorySlots.size(),
              "some slots must stay empty so the empty-slot path is captured", __LINE__);

        // 装备槽在背包屏（生存与创造）上都画，内容与容器无关，所以每一屏都填。
        std::size_t worn = 0;
        for (const auto& stack : snapshot.equipmentSlots) {
            if (!stack.empty()) ++worn;
        }
        check(worn == mc::gameplay::kEquipmentSlotCount,
              "all five equipment slots must be worn", __LINE__);
    }

    // 逐屏：容器那一半也要有东西，否则那一屏拍到的是一个空壳。
    const auto chest = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::Chest));
    CHECK(chest.openChest.has_value());   // 没有它，drawWorkContainer 一个箱子格都不画
    std::size_t chestFilled = 0;
    for (const auto& stack : chest.chestItems) {
        if (!stack.empty()) ++chestFilled;
    }
    CHECK(chestFilled >= 8U);

    const auto furnace = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::Furnace));
    CHECK(!furnace.furnaceInput.empty());
    CHECK(!furnace.furnaceFuel.empty());
    CHECK(!furnace.furnaceOutput.empty());
    // ★ 两条进度必须**既非 0 也非 1**：0 那一档整条不画，1 那一档画满——
    //   两者都绕过了"按比例裁切精灵"这条真正要看的路径。
    CHECK(furnace.furnaceFuelProgress > 0.0F && furnace.furnaceFuelProgress < 1.0F);
    CHECK(furnace.furnaceCookProgress > 0.0F && furnace.furnaceCookProgress < 1.0F);

    const auto table = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::CraftingTable));
    std::size_t gridFilled = 0;
    for (const auto& stack : table.tableCraftingGrid) {
        if (!stack.empty()) ++gridFilled;
    }
    CHECK(gridFilled > 0U && gridFilled < table.tableCraftingGrid.size());
    CHECK(!table.tableCraftingOutput.empty());

    const auto enchanting = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::EnchantingTable));
    CHECK(!enchanting.enchantingItem.empty());
    CHECK(!enchanting.enchantingLapis.empty());
    for (std::size_t bar = 0; bar < enchanting.enchantingRequiredLevels.size(); ++bar) {
        // 0 是"死条"那一档。三条都死，这一屏的主体就没进画。
        check(enchanting.enchantingRequiredLevels[bar] > 0,
              "every enchanting bar must be live", __LINE__);
        check(enchanting.enchantingClueLevels[bar] > 0U,
              "every enchanting bar must carry a clue", __LINE__);
    }
    // 乱码名是这个种子的函数：不钉住它，两遍拍出来的字就可能不同。
    CHECK(enchanting.enchantingSeed != 0);

    const auto anvil = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::Anvil));
    CHECK(!anvil.anvilLeft.empty());
    CHECK(!anvil.anvilRight.empty());
    CHECK(!anvil.anvilResult.empty());
    CHECK(anvil.anvilCost > 0);

    // 生存背包有 2x2 合成格，创造背包没有——这条差异是 ScreenHandler 里那句
    // "creative has no crafting at all"，夹具要跟它一致，否则拍出来的创造背包
    // 会摆着一份根本不存在的合成网格。
    const auto survival = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, false));
    const auto creative = mc::render::uiCaptureWorldSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, true));
    CHECK(!survival.playerCraftingGrid[0].empty());
    CHECK(!survival.playerCraftingOutput.empty());
    CHECK(creative.playerCraftingGrid[0].empty());
    CHECK(creative.playerCraftingOutput.empty());

    // 玩家快照：创造那一档要真的是创造，否则拍到的"创造背包"其实是生存背包。
    CHECK(mc::render::uiCapturePlayerSnapshot(
              containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, true))
              .gameMode == mc::gameplay::GameMode::Creative);
    // ★ **前端页面**（看不见世界的）拿的是默认玩家快照。这条断言守的是它们的基线：
    //   ui::UiFrameData 的默认值与默认 PlayerTickSnapshot 逐字段相等，所以
    //   "从默认快照同步一次"与"从不同步"结果相同——一旦这里开始返回非默认值，
    //   那十几张前端图就会静默改变。
    for (const auto page : {mc::ui::PageId::Title, mc::ui::PageId::Options,
                            mc::ui::PageId::WorldList, mc::ui::PageId::Language}) {
        check(mc::render::uiCapturePlayerSnapshot(pageTarget(page)) ==
                  mc::gameplay::PlayerTickSnapshot{},
              "a frontend page must keep the default player snapshot", __LINE__);
        check(mc::render::uiCaptureWorldSnapshot(pageTarget(page)) ==
                  mc::gameplay::WorldSnapshot{},
              "a frontend page must keep the default world snapshot", __LINE__);
    }

    // ★ UI-8 / D26：**世界页**要有内容——空血、空饥饿、空手的 HUD 对照 26.1 时没有
    //   参考价值。
    //   ★ 三页里画面上真看得见 HUD 的只有 `game`：`pause` 与 `death` 在 `drawHud`
    //     里早退到暂停菜单那一支，HUD 不画。给它们同样的内容是为了**状态一致**
    //     （三页同属一个世界会话，让其中一页的玩家空血空手是自相矛盾的），
    //     而不是因为那两屏会显示它。
    for (const auto page : {mc::ui::PageId::Game, mc::ui::PageId::Pause,
                            mc::ui::PageId::Death}) {
        const auto player = mc::render::uiCapturePlayerSnapshot(pageTarget(page));
        check(player.health > 0.0F && player.health < 20.0F,
              "a world page must show a partial health bar", __LINE__);
        check(player.foodLevel > 0 && player.foodLevel < 20,
              "a world page must show a partial hunger bar", __LINE__);
        std::size_t hotbar = 0;
        const auto world = mc::render::uiCaptureWorldSnapshot(pageTarget(page));
        for (std::size_t i = 0; i < 9U; ++i) {
            if (!world.inventorySlots[i].empty()) ++hotbar;
        }
        check(hotbar >= 8U, "a world page must show a stocked hotbar", __LINE__);
    }

    // ★ `loading` 是 needsWorld 与 showsWorld 的**差集**：它属于世界会话，但画的是
    //   全景加一行进度，HUD 根本不可见。给它内容只会让一张看不见的东西参与比对。
    CHECK(mc::render::uiCaptureWorldSnapshot(pageTarget(mc::ui::PageId::Loading)) ==
          mc::gameplay::WorldSnapshot{});
    CHECK(mc::render::uiCapturePlayerSnapshot(pageTarget(mc::ui::PageId::Loading)) ==
          mc::gameplay::PlayerTickSnapshot{});
    const auto player = mc::render::uiCapturePlayerSnapshot(
        containerTarget(mc::gameplay::ContainerScreen::PlayerInventory, false));
    // ★ 状态条取**非满**值：满血满饥饿只画得出"整排实心图标"，半颗心与半块肉
    //   那两张精灵一张都进不了画。
    CHECK(player.health > 0.0F && player.health < 20.0F);
    CHECK(player.foodLevel > 0 && player.foodLevel < 20);
    CHECK(player.experienceProgress > 0.0F && player.experienceProgress < 1.0F);
}

// --- 3b. 源码护栏：夹具的两处"改了图但不改任何返回值"的地方 -------------------
//
// 相机位姿与 drawHud 的早退条件都只影响**像素**：改坏了每个函数的返回值都一样，
// 全套测试照样全绿，而拍出来的图是一整屏清除色、或者一张没有界面的世界照。
// 本仓不存基线图（方块预览也只做到"确定性已核实、正确性未核实"），所以这两件事
// 只能靠读源码守——同 title_background 的做法，理由也同它。

[[nodiscard]] std::string readSource(const char* path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        std::printf("ui_capture_test: cannot open %s\n", path);
        ++failures;
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    // 去掉 `//` 行注释：这两段的注释里正好提到了要禁止的名字。
    std::string result;
    std::istringstream lines{buffer.str()};
    std::string line;
    while (std::getline(lines, line)) {
        const auto comment = line.find("//");
        result += comment == std::string::npos ? line : line.substr(0, comment);
        result += '\n';
    }
    return result;
}

[[nodiscard]] std::string functionBody(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    if (start == std::string::npos) {
        std::printf("ui_capture_test: %s not found — did it get renamed? "
                    "This guard must be moved with it, not deleted.\n", signature.c_str());
        ++failures;
        return {};
    }
    const auto open = source.find('{', start);
    if (open == std::string::npos) {
        ++failures;
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') { ++depth; }
        if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, i - open + 1U);
            }
        }
    }
    ++failures;
    return {};
}

void testFixtureSourceGuards() {
    const std::string renderer = readSource(MC_REBEDROCK_RENDERER_SRC);
    const std::string fixture = functionBody(renderer, "void ensureUiCaptureWorldFixture(");
    if (!fixture.empty()) {
        // ★ 相机位姿必须**显式**设。踩过的坑：只设位置、朝向留出厂默认 yaw = -90°
        //   （forward = (0,0,-1)），相机正好背对场景，拍出来是一整屏清除色加一个
        //   右下角的手持物——看起来像"世界没渲染"，其实是相机在别处。
        CHECK(fixture.find("camera.setPosition(") != std::string::npos);
        CHECK(fixture.find("camera.setRotation(") != std::string::npos);
        // ★ 而且**不能**用 snapshotCameraEye()：它读 clientMirror 里的玩家位置，
        //   而这条通道不启动模拟线程，镜像永远停在初始值上，与夹具摆的那个点无关。
        CHECK(fixture.find("snapshotCameraEye") == std::string::npos);
    }

    // A0-0：容器目标的四件事全都"只改像素、不改任何函数的返回值"——删掉哪一件，
    // 全套断言照样全绿，只是拍出来的图不是那一屏了。只能读源码守。
    const std::string targetState = functionBody(renderer, "void applyUiCaptureTargetState(");
    if (!targetState.empty()) {
        // ★ 容器屏是靠这个标志打开的，不是靠页面栈。写死 false（A0-0 之前就是
        //   `inventoryOpen = false;`）会让每个容器目标都拍成一张普通的游戏内 HUD——
        //   一张完全正确的 HUD，只是文件名写着 chest。
        CHECK(targetState.find("inventoryOpen = target.container.has_value()") !=
              std::string::npos);
        // 容器内容来自注入的快照。少了这一步，每一个槽位都是空的。
        CHECK(targetState.find("publishUiCaptureSnapshots(target)") != std::string::npos);
        // 创造背包开在哪个页签是屏幕状态；不钉它，creative-catalog 与 inventory-creative
        // 会拍成同一屏。
        CHECK(targetState.find("menuSystem.creativeTab =") != std::string::npos);
        // ★ 滚动行不钉住就会**跨目标串扰**：拍完目录页再拍别的，下一次的目录停在
        //   上一次滚到的地方，而两遍比对**发现不了**（两遍的串扰顺序一模一样）。
        CHECK(targetState.find("menuSystem.creativeScrollRow = 0U") != std::string::npos);
        // ★ 玩家模型的骨骼姿态要动画器求值过一次才绑定。少了它，背包屏那口黑井
        //   （vanilla inventory.png 自带的）里一次都没出现过人物。
        CHECK(targetState.find("playerModelAnimator.update(") != std::string::npos);
    }

    const std::string publish = functionBody(renderer, "void publishUiCaptureSnapshots(");
    if (!publish.empty()) {
        // ★ 快照走**生产的编解码通道**注入，而不是给 ClientMirror 开一个截图专用的
        //   setter：镜像的写入者因此仍然只有一个。
        CHECK(publish.find("makeLoopbackPair()") != std::string::npos);
        CHECK(publish.find("clientMirror_.pump(") != std::string::npos);
        // 而 uiFrameData_ 必须走生产路径那**同一个**函数填，不许在这里再判一次目标。
        CHECK(publish.find("syncUiFrameDataFromMirror()") != std::string::npos);
        CHECK(publish.find("target.creative") == std::string::npos);
    }

    // ★ A1 抓到的一条：**绘制侧只准有一处读光标**。
    //
    //   生存背包屏此前自抄了一份 `glfwGetCursorPos + windowToFramebuffer`（十三行），
    //   而那一份**不认 `pinnedCursor`**——截图通道钉光标那颗钉子对整个背包屏不生效，
    //   于是基线图里有一格槽位被 Xvfb 的屏幕中心指针 (640,512) 常年点亮。
    //
    //   ★ 两遍比对**发现不了它**：两遍读到的是同一个真实指针位置，图当然一样。
    //     这正是"两遍比对证不了每个 knob 都还在"那句话的实证，不是它的理论。
    //   护栏写成"整个文件里 glfwGetCursorPos 只出现一次"，因为那一次必须是
    //   `currentFramebufferCursor`——它是唯一认钉子的那一处。
    {
        const std::string hudSource = readSource(MC_REBEDROCK_HUD_RENDERER_SRC);
        std::size_t reads = 0;
        for (std::size_t at = hudSource.find("glfwGetCursorPos"); at != std::string::npos;
             at = hudSource.find("glfwGetCursorPos", at + 1U)) {
            ++reads;
        }
        check(reads == 1U,
              "the HUD must read the cursor in exactly one place (currentFramebufferCursor)",
              __LINE__);
        const std::string cursor =
            functionBody(hudSource, "ui::UiPoint currentFramebufferCursor(");
        CHECK(cursor.find("glfwGetCursorPos") != std::string::npos);
        CHECK(cursor.find("pinnedCursor") != std::string::npos);
    }

    const std::string hud = readSource(MC_REBEDROCK_HUD_RENDERER_SRC);
    const std::string drawHud = functionBody(hud, "void drawHud(VkCommandBuffer");
    if (!drawHud.empty()) {
        // ★ 测试场景那条早退必须带上"除非在拍界面"。少了它，四个世界页拍到的是
        //   一张没有任何界面的世界照——每一张都存在、大小也正常，只是空的。
        const auto early = drawHud.find("testScene.has_value()");
        CHECK(early != std::string::npos);
        if (early != std::string::npos) {
            CHECK(drawHud.find("uiCaptureActive", early) != std::string::npos);
        }
    }
}

// --- 4. GUI 缩放档 -----------------------------------------------------------
void testScales() {
    const auto parsed = parse({"--ui-shot", "title", "--ui-scale", "1,4"});
    CHECK(parsed.has_value());
    // 第一次 --ui-scale 顶掉默认的两档，而不是追加在它们后面——否则会拍出没要的图。
    CHECK(parsed->guiScales.size() == 2U);
    CHECK(parsed->guiScales[0] == 1);
    CHECK(parsed->guiScales[1] == 4);

    // 之后的 --ui-scale 累加。
    const auto more = parse({"--ui-shot", "title", "--ui-scale", "1", "--ui-scale", "3"});
    CHECK(more.has_value());
    CHECK(more->guiScales.size() == 2U);
    CHECK(more->guiScales[0] == 1);
    CHECK(more->guiScales[1] == 3);

    // 0 是 Auto，合法。
    const auto autoScale = parse({"--ui-shot", "title", "--ui-scale", "0"});
    CHECK(autoScale.has_value() && autoScale->guiScales.size() == 1U &&
          autoScale->guiScales[0] == 0);

    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "-1"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "99"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "two"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "2,2"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "2,"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale"});
}

// --- 5. 画布尺寸 -------------------------------------------------------------
void testSize() {
    const auto parsed = parse({"--ui-shot", "title", "--ui-size", "854x480"});
    CHECK(parsed.has_value() && parsed->width == 854U && parsed->height == 480U);

    // 下界是 spec §1.1 的最小逻辑画布 320x240：比它更小的窗口撑不住任何一档缩放。
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "319x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x239"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "9000x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x480x2"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size"});
}

// --- 5b. A1：光标钉在哪 ------------------------------------------------------
//
// ★ 它存在的理由是一次没抓住的 sabotage：把"手上拖着东西时不画提示框"那条判断改成
//   恒真，全套截图**逐字节不变**——因为夹具里光标在画布外、手上也没东西，两种实现
//   在这个夹具下同解。规矩是"没抓住就补测试或补夹具，不要换一个更好抓的 sabotage"，
//   而这一次差的是**夹具**：再多断言也分不开两个同解的实现。
void testCursorPin() {
    // 默认仍是画布外那个点：既有基线因此不受影响。
    const auto byDefault = parse({"--ui-shot", "title"});
    CHECK(byDefault.has_value());
    CHECK(byDefault->cursorX == mc::render::kUiCaptureCursorX);
    CHECK(byDefault->cursorY == mc::render::kUiCaptureCursorY);

    const auto pinned = parse({"--ui-shot", "chest", "--ui-cursor", "640,360"});
    CHECK(pinned.has_value());
    CHECK(pinned->cursorX == 640.0F);
    CHECK(pinned->cursorY == 360.0F);
    // ★ 负数要收得下：默认值本身就是 -1，而"画布外"正是它最重要的一个取值。
    const auto negative = parse({"--ui-shot", "title", "--ui-cursor", "-8,-9"});
    CHECK(negative.has_value() && negative->cursorX == -8.0F && negative->cursorY == -9.0F);

    EXPECT_THROWS({"--ui-shot", "title", "--ui-cursor", "640"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-cursor", "640x360"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-cursor", "a,b"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-cursor"});
    EXPECT_THROWS({"--ui-cursor", "1,1"});   // 只给参数不说拍什么

    // ★ `--ui-carry` 与 `--ui-cursor` 是两根**正交**的轴：前者开"光标上那一堆 +
    //   抑制提示框"，后者开"悬停高亮 + 提示框"。合成一个开关，"悬停且手上是空的"
    //   那一档——也就是二十张常规基线的那一档——就再也拍不到了。
    CHECK(byDefault.has_value() && !byDefault->carryStack);
    const auto carrying = parse({"--ui-shot", "chest", "--ui-carry"});
    CHECK(carrying.has_value() && carrying->carryStack);
    CHECK(carrying->cursorX == mc::render::kUiCaptureCursorX);   // 两根轴互不牵连
    const auto both = parse({"--ui-shot", "chest", "--ui-cursor", "10,20", "--ui-carry"});
    CHECK(both.has_value() && both->carryStack && both->cursorX == 10.0F);
    EXPECT_THROWS({"--ui-carry"});   // 只给参数不说拍什么

    // 夹具：不拿的时候手上是空的，拿的时候不是——而且拿的那一堆要**带附魔**，
    // 好让"提示框被抑制"这件事在图上看得出区别（多行提示框 vs 一格物品）。
    const auto chest = containerTarget(mc::gameplay::ContainerScreen::Chest);
    CHECK(mc::render::uiCaptureWorldSnapshot(chest, false).cursorStack.empty());
    const auto carried = mc::render::uiCaptureWorldSnapshot(chest, true).cursorStack;
    CHECK(!carried.empty());
    CHECK(carried.enchantmentCount > 0U);
    // 前端目标不受影响：它拿到的仍是一份默认快照（既有十八屏基线的构造性保证）。
    CHECK(mc::render::uiCaptureWorldSnapshot(pageTarget(mc::ui::PageId::Title), true) ==
          mc::gameplay::WorldSnapshot{});
}

// --- 6. 输出路径只是命令行的函数 ---------------------------------------------
void testPaths() {
    auto parsed = parse({"--ui-shot", "title,options", "--ui-scale", "2,3", "--ui-out",
                         "/tmp/shots"});
    CHECK(parsed.has_value());
    CHECK(mc::render::uiCaptureImageCount(*parsed) == 4U);

    const auto titleAtTwo =
        mc::render::uiCaptureImagePath(*parsed, pageTarget(mc::ui::PageId::Title), 2);
    CHECK(titleAtTwo == std::filesystem::path{"/tmp/shots/title/scale-2.png"});
    CHECK(mc::render::uiCaptureImagePath(*parsed, pageTarget(mc::ui::PageId::Options), 3) ==
          std::filesystem::path{"/tmp/shots/options/scale-3.png"});
    // Auto 档要有自己的名字，否则它会和 "scale-0" 撞在一起看不出是哪一档。
    CHECK(mc::render::uiCaptureImagePath(*parsed, pageTarget(mc::ui::PageId::Title), 0) ==
          std::filesystem::path{"/tmp/shots/title/scale-auto.png"});

    // 同一条命令行解析两遍，得到完全相同的参数与路径——确定性从这里就开始。
    const auto again = parse({"--ui-shot", "title,options", "--ui-scale", "2,3", "--ui-out",
                              "/tmp/shots"});
    CHECK(again.has_value() && *again == *parsed);
    CHECK(mc::render::uiCaptureImagePath(*again, pageTarget(mc::ui::PageId::Title), 2) ==
          titleAtTwo);

    // 每一张图的路径互不相同：页与档都进了路径，所以四张图落在四个位置。
    std::vector<std::filesystem::path> written;
    for (const auto& target : parsed->targets) {
        for (const int scale : parsed->guiScales) {
            written.push_back(mc::render::uiCaptureImagePath(*parsed, target, scale));
        }
    }
    CHECK(written.size() == mc::render::uiCaptureImageCount(*parsed));
    for (std::size_t i = 0; i < written.size(); ++i) {
        for (std::size_t j = i + 1U; j < written.size(); ++j) {
            CHECK(written[i] != written[j]);
        }
    }
}

// --- 7. determinism knobs --------------------------------------------------
//
// ★ 先说清这条测试为什么必须存在，别把它当成凑数的源码扫描：
//
// "同一条命令行跑两遍逐字节相同"是这条通道的验收条件，而它在本容器里**通不过**
// 大部分 knob 的 sabotage —— 实测：把光标那一钉删掉，两遍的图仍然逐字节相同
// （Xvfb + 隐藏窗口下 GLFW 读回的光标位置本来就恒定；即使用 XWarpPointer 在两遍
// 之间把指针挪到"单人游戏"按钮上，图还是一样）。UI 时钟同理：拍摄循环自己从不推进它，
// 一个全新进程里它本来就是 0。
//
// 也就是说，两遍比对能证明"这条通道现在是确定的"，但**证不了"每个 knob 都还在"**。
// 而 knob 防的是别的情形：可见窗口、别的平台、或者将来从主循环之后再驱动一次拍摄——
// 那时删掉的那一钉会让图片开始漂移，而两遍比对届时才发现就太晚了。
//
// 所以这里用两层：一条对光标那个点的**性质断言**（它必须落在所有控件之外，否则
// 钉了也白钉），加一条列出每个 knob 的源码护栏（少钉一个就红）。
void testDeterminismKnobs() {
    // 性质：钉住的那个点在任何画布、任何缩放档下都不落在任何一个主菜单控件上。
    // 这才是"钉光标"要达到的效果——不是"钉住就行"，而是"钉住之后没有控件悬停"。
    const mc::ui::UiPoint pinned{mc::render::kUiCaptureCursorX, mc::render::kUiCaptureCursorY};
    for (const auto canvas : {std::pair{1280.0F, 720.0F}, std::pair{854.0F, 480.0F},
                              std::pair{1281.0F, 721.0F}, std::pair{640.0F, 480.0F}}) {
        for (int guiScale = 0; guiScale <= 4; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            for (std::size_t index = 0; index < mc::ui::kTitleWidgetCount; ++index) {
                const auto rect = mc::ui::frontendButtonRect(layout, mc::ui::PageId::Title, index,
                                                             mc::ui::kTitleWidgetCount);
                CHECK(!rect.contains(pinned.x, pinned.y));
            }
        }
    }
}

// 源码护栏：拍摄路径必须钉住下面每一个逐帧变化的量。
// 少钉一个不改变任何函数的返回值，也（在本环境下）不改变图片——只能这么抓。
void testKnobsAreAllPinned() {
    std::ifstream input{MC_REBEDROCK_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("ui_capture_test: cannot open %s\n", MC_REBEDROCK_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto signature = source.find("void applyUiCaptureDeterminism()");
    if (signature == std::string::npos) {
        std::printf("ui_capture_test: applyUiCaptureDeterminism not found — if it was renamed, "
                    "move this guard with it rather than deleting it\n");
        ++failures;
        return;
    }
    const auto open = source.find('{', signature);
    std::string body;
    int depth = 0;
    for (std::size_t index = open; index < source.size() && open != std::string::npos; ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            --depth;
            if (depth == 0) {
                body = source.substr(open, index - open + 1U);
                break;
            }
        }
    }
    CHECK(!body.empty());

    // 每一条都写清它不钉会怎样，删掉哪一条这里就红哪一条。
    // UI 时钟：全景相机的偏航与俯仰、文本光标的闪烁相位都是它的函数
    CHECK(body.find("uiTimeSeconds = kUiCaptureClockSeconds") != std::string::npos);
    // 光标：按钮的悬停高亮与槽位提示框都读它。
    // ★ A1 之后它是**命令行的函数**（`--ui-cursor`，默认仍是画布外那个点），
    //   所以护栏钉的是"它被显式赋值、且赋的是拍摄参数里的那个点"——而**不是**
    //   钉住某一个字面量：钉字面量会把"可以拍悬停态"这件事一并禁掉。
    CHECK(body.find("pinnedCursor =") != std::string::npos);
    CHECK(body.find("uiCapture->cursorX") != std::string::npos);
    CHECK(body.find("kUiCaptureCursorX") != std::string::npos);
    // 而它绝不能来自运行期读数——那正是这一整条护栏要挡的东西。
    CHECK(body.find("glfwGetCursorPos") == std::string::npos);
    // 按下态：上一次输入留下的按下按钮会被画成按下的样子
    CHECK(body.find("pressedMenuButton = ui::WidgetId::None") != std::string::npos);
    // 天气与视角摇晃：两者都经 HUD 通道影响画面
    CHECK(body.find("setWeather(") != std::string::npos);
    CHECK(body.find("options.viewBobbing = false") != std::string::npos);
    // 插值权重：世界静止而它不是，且它喂给视图矩阵
    CHECK(body.find("renderInterpolationAlpha = 0.0F") != std::string::npos);
    // 提示条与聊天：两者都带时间戳，会随运行时刻淡出
    CHECK(body.find("toastQueue_.clear()") != std::string::npos);
    CHECK(body.find("chatHistory.clear()") != std::string::npos);

    // ★ 导出**从不写 options.properties**。
    //
    // 这条不是洁癖：UI-2 落地后确实发生过——一次 1280x720 的界面截图把 `window.width`
    // 从 640 改成了 1280（隐藏窗口的尺寸经 `noteWindowSizeChanged` 进了 options，退出时存盘），
    // 于是下一次拍摄读到的是上一次留下的窗口尺寸，视频设置页的 "Fullscreen Resolution"
    // 标签跟着变，两组基线不可比。拍摄是一次测量，不是一场游戏。
    const auto persist = source.find("void persistOptions() noexcept");
    CHECK(persist != std::string::npos);
    if (persist != std::string::npos) {
        const std::string persistBody = source.substr(persist, 1200U);
        const auto guard = persistBody.find("if (uiCapture.has_value() ||");
        CHECK(guard != std::string::npos);
        // 豁免必须在**写任何字段之前**：先改 options 再 return 一样会留下脏值
        const auto firstWrite = persistBody.find("options.guiScale =");
        CHECK(firstWrite != std::string::npos && guard < firstWrite);
    }

    // 各向异性 / 抗锯齿 / 垂直同步是**初始化期读一次**的，钉在 initialize() 的开头，
    // 不在这个函数里。它们决定采样器与管线，晚一步钉就没用了。
    const auto initialize = source.find("void initialize()");
    CHECK(initialize != std::string::npos);
    // TAA-1：这一段从"只管界面截图"扩成"两条离屏通道共用"——方块预览导出从前
    // 没有份，于是它的抗锯齿档取决于机器上的 options.properties
    const auto capturePin =
        source.find("if (uiCapture.has_value() || (testScene.has_value() && "
                    "testScene->exportPreview)) {",
                    initialize);
    CHECK(capturePin != std::string::npos);
    // ★ 判据不是「离函数开头多少个字符」，而是「在 glfwInit 之前」——那才是这三档
    // 还来得及生效的真正边界。用字符距离量的那一版会被一段长注释推翻，而注释长短
    // 与它钉的那件事毫无关系
    const auto glfwInitCall = source.find("glfwInit()", initialize);
    CHECK(glfwInitCall != std::string::npos && capturePin < glfwInitCall);
    const std::string prologue = source.substr(capturePin, glfwInitCall - capturePin);
    CHECK(prologue.find("options.anisotropy = 1") != std::string::npos);
    // TAA 之后这一档是三态枚举。截图通道要的仍是"最不改变边缘的那一档"——
    // MSAA 会动几何边，TAA 还会让画面取决于前面拍了几帧
    // RN-44：钉的是**与 options.properties 无关**，不是钉成一个常量。常量那一版的
    // 代价是 MSAA 与 TAA 两条路在离屏通道里根本拍不到。界面截图没有 testScene，
    // 于是仍旧落在 Off 那一支
    CHECK(prologue.find("options.antiAliasing = testScene.has_value() ? testScene->antiAliasing") !=
          std::string::npos);
    CHECK(prologue.find(": config::AntiAliasingMode::Off;") != std::string::npos);
    // 而它绝不能来自持久化的那份配置——那正是这一整条护栏要挡的东西
    CHECK(prologue.find("options.antiAliasing = options.") == std::string::npos);
    CHECK(prologue.find("options.vsync = false") != std::string::npos);
}

} // namespace

int main() {
    testAbsent();
    testDefaults();
    testPageNames();
    testContainerTargets();
    testContainerFixture();
    testFixtureSourceGuards();
    testScales();
    testSize();
    testCursorPin();
    testPaths();
    testDeterminismKnobs();
    testKnobsAreAllPinned();
    if (failures != 0) {
        std::printf("ui_capture_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
