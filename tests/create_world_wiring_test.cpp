// 「创建世界」界面那三项功能的接线测试：世界种子输入、创建时选难度、文件夹名预览。
//
// 三项的后端本来就齐（createWorld 一直带 seed 形参、存档一直有 difficulty 字段、
// create() 一直会 slug 化），缺的全是前端到后端那一段。所以这里断言的也正是那一段：
//
//   1. ui::parseWorldSeed —— 种子框里的字符串是什么意思（空 / 十进制 / 哈希）
//   2. ui::newWorldRequest —— 表单读进 createWorld 的那五个实参，尤其是难度不再是常量
//   3. SaveRepository::slugForDisplayName —— 预览用的文件夹名与 create() 真建出来的一致
//   4. ui::buildPage —— 创建页上真的多了那个难度按钮，且点它步进的是表单的暂存值
//   5. GameRuntime::createWorld —— 难度确实落到磁盘上，而不是只停在内存里

#include "runtime/GameRuntime.hpp"

#include "gameplay/Difficulty.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "persistence/SaveRepository.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/WidgetId.hpp"
#include "world/ChunkStreamer.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

using namespace mc;

namespace {

// createWorld 一行世界数据都不 tick，所以宿主的每个反应都是空操作。
// 纯虚函数必须逐个实现（它们服务的是模拟循环，不是世界创建）
struct NullHost final : public gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// Java 的 int 哈希在 Java 里会被符号扩展成 long 再当种子用，本作的 u64 种子同样
// 走这条位型。写成函数是为了让每条期望值都显式带上这次扩展
[[nodiscard]] std::uint64_t asSeed(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

void testSeedParsing() {
    // 空 = 随机：返回 nullopt，由调用方掷。★ 不是"返回 0"——那会让每个留空的世界
    // 都长成同一张地图，而且是静默的
    assert(!ui::parseWorldSeed("").has_value());
    assert(!ui::parseWorldSeed("   ").has_value());
    assert(!ui::parseWorldSeed("\t\n ").has_value());

    // 纯十进制整数直接当种子，含负号与正号（Long.parseLong 认 '+'）
    assert(ui::parseWorldSeed("12345") == asSeed(12345));
    assert(ui::parseWorldSeed("0") == asSeed(0));
    assert(ui::parseWorldSeed("-1") == asSeed(-1));
    assert(ui::parseWorldSeed("+7") == asSeed(7));
    // 前后空白先 trim 掉，与 WorldOptions.parseSeed 一致
    assert(ui::parseWorldSeed("  42  ") == asSeed(42));
    // i64 的两端都必须原样通过，不能被截成别的世界
    assert(ui::parseWorldSeed("9223372036854775807") == asSeed(9223372036854775807LL));
    assert(ui::parseWorldSeed("-9223372036854775808") ==
           asSeed(-9223372036854775807LL - 1LL));

    // 其余取 Java 的 String.hashCode（期望值来自 Java 的定义，不是本实现的回填）
    assert(ui::parseWorldSeed("hello") == asSeed(99162322));
    assert(ui::parseWorldSeed("My Seed!") == asSeed(2062066780));
    // 哈希为负时要**符号扩展**到 64 位。写成裸 u32 的话高 32 位会是 0，
    // 那是另一个世界，而且只在名字恰好哈希为负时才现形
    assert(ui::parseWorldSeed("Minecraft") == asSeed(-1595926131));
    // 数字串但不是合法的 long：Java 那边同样抛 NumberFormatException 转哈希
    assert(ui::parseWorldSeed("99999999999999999999") == asSeed(1260560192));
    // 半个数字不算数字：Long.parseLong("12x") 抛异常，不是解析出 12
    assert(ui::parseWorldSeed("12x") == asSeed(48759));
    // 哈希按 UTF-16 码元算，不是 UTF-8 字节。中文名与 Java 版同种子同世界，
    // 这一条是 JC（存档/世界一致性）那根轴上的要求，不只是好看
    assert(ui::parseWorldSeed("世界") == asSeed(649718));
    // 代理对拆成两个码元参与哈希（U+1F642 -> D83D DE42）
    assert(ui::parseWorldSeed("🙂") == asSeed(1772965));
    // trim 之后再哈希：哈希的是 trim 后的串
    assert(ui::parseWorldSeed("  hello  ") == ui::parseWorldSeed("hello"));
}

// 表单读进 createWorld 的那五个实参
void testNewWorldRequest() {
    ui::MenuSystem menu;
    menu.createWorldName.value = "My World";
    menu.createWorldSeed.value = "12345";
    menu.createWorldGameMode = gameplay::GameMode::Creative;
    menu.createWorldAllowCommands = true;
    menu.createWorldDifficulty = gameplay::Difficulty::Hard;

    const auto request = ui::newWorldRequest(menu, /*randomSeed=*/0xDEADBEEFULL);
    assert(request.name == "My World");
    assert(request.seed == 12345ULL);
    assert(request.mode == gameplay::GameMode::Creative);
    assert(request.allowCommands);
    // ★ 难度必须来自表单。从前这里恒为 Normal（写死在 GameRuntime::createWorld 里），
    //   四个档位的界面选择整个丢掉，而游戏里看起来"就是普通难度"，没有任何报错
    assert(request.difficulty == gameplay::Difficulty::Hard);

    // 种子框留空才用调用方掷的那个随机数
    menu.createWorldSeed.value.clear();
    assert(ui::newWorldRequest(menu, 0xDEADBEEFULL).seed == 0xDEADBEEFULL);
    // 非空时**绝不**回落到随机数——这正是从前那个 bug 的形状
    menu.createWorldSeed.value = "hello";
    assert(ui::newWorldRequest(menu, 0xDEADBEEFULL).seed == asSeed(99162322));

    // 四个难度档都要能原样传下去，不是只有 Hard 走通
    for (std::uint8_t raw = 0; raw < gameplay::kDifficultyCount; ++raw) {
        menu.createWorldDifficulty = static_cast<gameplay::Difficulty>(raw);
        assert(ui::newWorldRequest(menu, 0U).difficulty == menu.createWorldDifficulty);
    }
}

// 预览用的 slug 与 create() 真正建出来的目录名必须逐字节一致
void testFolderPreview(const std::filesystem::path& root) {
    persistence::SaveRepository repository{root / "folder-preview"};

    // 现有 save_repository_test 钉的那一条：清洗掉 '=' 与首尾空白之后再 slug 化
    assert(persistence::SaveRepository::slugForDisplayName("  Test=World  ") == "testworld");
    // 非字母数字折成单个 '-'，尾部的 '-' 去掉，全大写转小写
    assert(persistence::SaveRepository::slugForDisplayName("My New World!!") == "my-new-world");
    assert(persistence::SaveRepository::slugForDisplayName("A  B") == "a-b");
    // 一个能用的字符都没有 -> "world"
    assert(persistence::SaveRepository::slugForDisplayName("!!!") == "world");
    // 空名字先被 sanitizeDisplayName 顶成 "New World"，所以走的**不是** "world" 那一支
    assert(persistence::SaveRepository::slugForDisplayName("") == "new-world");

    // 与 create() 对拍：目录还不存在时，两者必须给出同一个名字
    for (const std::string_view name : {"  Test=World  ", "My New World!!", "!!!", ""}) {
        const auto created = repository.create(std::string{name}, 0U);
        assert(created.summary.identifier ==
               persistence::SaveRepository::slugForDisplayName(name));
    }

    // 去重后缀留在 create() 里（它要摸磁盘），slug 函数不受影响：
    // 预览显示的永远是不带后缀的基名
    auto first = repository.create("Dup", 1U);
    assert(first.summary.identifier == "dup");
    repository.save(first);
    const auto second = repository.create("Dup", 2U);
    assert(second.summary.identifier == "dup-2");
    assert(persistence::SaveRepository::slugForDisplayName("Dup") == "dup");
}

// 创建页上真的多了难度按钮，点它步进的是表单的暂存值
void testCreateWorldPageWiring() {
    ui::MenuSystem menu;
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    cb.cycleCreateDifficulty = [&menu] {
        menu.createWorldDifficulty = gameplay::nextDifficulty(menu.createWorldDifficulty);
    };
    // 世界内选项页那个难度按钮走的是**另一个**回调，创建页绝不能误接到它上面：
    // 接错了的症状是在创建界面上改动的是已打开存档的难度（此时根本没有存档）
    bool inWorldDifficultyCycled = false;
    cb.cycleDifficulty = [&inWorldDifficultyCycled] { inWorldDifficultyCycled = true; };

    const auto page = ui::buildPage(ui::PageId::CreateWorld, ctx, cb, [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    });

    // 顺序照 26.1 的 GameTab：游戏模式、难度、允许作弊、创建、返回
    assert(page.size() == 5U);
    assert(page[0].debugId == static_cast<std::uint16_t>(ui::WidgetId::CreateGameMode));
    assert(page[1].debugId == static_cast<std::uint16_t>(ui::WidgetId::Difficulty));
    assert(page[2].debugId == static_cast<std::uint16_t>(ui::WidgetId::CreateAllowCommands));
    assert(page[3].debugId == static_cast<std::uint16_t>(ui::WidgetId::CreateConfirm));
    assert(page[4].debugId == static_cast<std::uint16_t>(ui::WidgetId::Back));

    assert(page[1].onActivate);
    page[1].onActivate();
    assert(menu.createWorldDifficulty == gameplay::Difficulty::Hard);  // Normal -> Hard
    page[1].onActivate();
    assert(menu.createWorldDifficulty == gameplay::Difficulty::Peaceful);  // 循环回头
    assert(!inWorldDifficultyCycled);
}

// 难度真的落到磁盘上：createWorld 写完就存盘，再从仓库读回来
void testDifficultyReachesTheSave(const std::filesystem::path& root) {
    const auto saveRoot = root / "difficulty";
    std::filesystem::create_directories(saveRoot);
    world::ChunkStreamer streamer{0U, 4, 4};
    NullHost host;
    runtime::GameRuntime runtime{host, streamer, saveRoot};

    const auto hard =
        runtime.createWorld("hard world", 7U, gameplay::GameMode::Survival,
                            /*allowCommands=*/false, gameplay::Difficulty::Hard);
    assert(hard.difficulty == gameplay::Difficulty::Hard);
    assert(hard.summary.seed == 7U);

    persistence::SaveRepository repository{saveRoot};
    const auto reloaded = repository.load(hard.summary.identifier);
    // ★ 内存里对了不算数：难度是存档字段，读回来还是 Hard 才说明它真的过了序列化
    assert(reloaded.difficulty == gameplay::Difficulty::Hard);
    assert(reloaded.summary.seed == 7U);

    // 缺省仍是 Normal（dedicated_server 那类不传难度的调用点靠的就是这个）
    const auto peaceful = runtime.createWorld("peaceful world", 9U, gameplay::GameMode::Survival,
                                              /*allowCommands=*/false,
                                              gameplay::Difficulty::Peaceful);
    assert(peaceful.difficulty == gameplay::Difficulty::Peaceful);
    const auto defaulted = runtime.createWorld("default world", 11U, gameplay::GameMode::Survival);
    assert(defaulted.difficulty == gameplay::Difficulty::Normal);
    // 两个世界各自记各自的难度，后建的不会把先建的覆盖掉
    assert(repository.load(peaceful.summary.identifier).difficulty ==
           gameplay::Difficulty::Peaceful);
    assert(repository.load(hard.summary.identifier).difficulty == gameplay::Difficulty::Hard);
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("mc-rebedrock-create-world-test-" + std::to_string(unique));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    testSeedParsing();
    testNewWorldRequest();
    testFolderPreview(root);
    testCreateWorldPageWiring();
    testDifficultyReachesTheSave(root);

    std::filesystem::remove_all(root);
    std::cout << "create_world_wiring: ok\n";
    return 0;
}
