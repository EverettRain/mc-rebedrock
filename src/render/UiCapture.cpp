#include "render/UiCapture.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mc::render {
namespace {

struct TargetName final {
    UiCaptureTarget target;
    std::string_view name;
};

// 一张表，两个方向都读它，于是名字与目标不可能各说各话。
//
// 前 18 行是前端页面（容器为空）；后 7 行是容器界面——它们全都挂在 `PageId::Game`
// 之上，因为容器屏**不是一个 PageId**（见 UiCaptureTarget 的注释）。
constexpr std::array kTargetNames{
    TargetName{{ui::PageId::Title}, "title"},
    TargetName{{ui::PageId::WorldList}, "world-list"},
    TargetName{{ui::PageId::CreateWorld}, "create-world"},
    TargetName{{ui::PageId::EditWorld}, "edit-world"},
    TargetName{{ui::PageId::ConfirmDelete}, "confirm-delete"},
    TargetName{{ui::PageId::Loading}, "loading"},
    TargetName{{ui::PageId::Game}, "game"},
    TargetName{{ui::PageId::Pause}, "pause"},
    TargetName{{ui::PageId::Death}, "death"},
    TargetName{{ui::PageId::Options}, "options"},
    TargetName{{ui::PageId::VideoSettings}, "video-settings"},
    TargetName{{ui::PageId::Controls}, "controls"},
    TargetName{{ui::PageId::Language}, "language"},
    TargetName{{ui::PageId::AdvancedGraphics}, "advanced-graphics"},
    TargetName{{ui::PageId::KeyBinds}, "key-binds"},
    TargetName{{ui::PageId::Accessibility}, "accessibility"},
    TargetName{{ui::PageId::SoundSettings}, "sound-settings"},
    TargetName{{ui::PageId::ResourcePacks}, "resource-packs"},
    TargetName{{ui::PageId::FontSettings}, "font-settings"},
    TargetName{{ui::PageId::AdvancedGraphicsNotice}, "advanced-graphics-notice"},
    // A0-0：容器界面。★ 创造背包是 PlayerInventory 的**创造那一档**，不是第七块屏。
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::PlayerInventory, false},
               "inventory"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::PlayerInventory, true, false},
               "inventory-creative"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::PlayerInventory, true, true},
               "creative-catalog"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::CraftingTable}, "crafting-table"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::Furnace}, "furnace"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::Chest}, "chest"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::EnchantingTable},
               "enchanting-table"},
    TargetName{{ui::PageId::Game, gameplay::ContainerScreen::Anvil}, "anvil"},
};

// 表必须覆盖 PageId 的每一个取值，否则 --ui-shot 会对某个真实存在的屏幕说"不认识"。
// ★ 用 `Count` 哨兵，**不是**"当时的最后一个枚举值"。从前这里写的是
//   `PageId::Accessibility + 1`，于是 UI-6e 在 Accessibility 之后加一页时，
//   这条断言仍然比较同一个数字、静默通过——表少一行，而 `--ui-shot` 会对一个真实
//   存在的屏幕说"不认识"。护栏本身失效了却不会有人知道，这是最坏的一种。
//
// ★ A0-0 起断言的是**逐个枚举值都在表里**，不再是"表有多少行"。行数断言在
//   加一行容器目标的同时漏掉一页时**照样通过**（两个错误互相抵消），而这里的
//   两条循环会指着那个缺的取值不放。
constexpr bool everyPageHasATarget() {
    for (std::size_t raw = 0; raw < static_cast<std::size_t>(ui::PageId::Count); ++raw) {
        const auto page = static_cast<ui::PageId>(raw);
        bool found = false;
        for (const TargetName& entry : kTargetNames) {
            if (entry.target.page == page && !entry.target.container.has_value()) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 每一块容器界面也要能点名拍。它与上面那条是两个问题：容器屏全都挂在 PageId::Game
// 上，所以"每个 PageId 都有目标"对容器一个字都没说。
constexpr bool everyContainerHasATarget() {
    for (std::size_t raw = 0; raw < static_cast<std::size_t>(gameplay::ContainerScreen::Count);
         ++raw) {
        const auto screen = static_cast<gameplay::ContainerScreen>(raw);
        bool found = false;
        for (const TargetName& entry : kTargetNames) {
            if (entry.target.container == screen) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 名字不能重复：两行同名时 uiCaptureTargetFromName 只会交出先出现的那一个，
// 而另一个目标就此永远拍不到——一条静默的覆盖漏洞。
constexpr bool everyNameIsUnique() {
    for (std::size_t i = 0; i < kTargetNames.size(); ++i) {
        for (std::size_t j = i + 1U; j < kTargetNames.size(); ++j) {
            if (kTargetNames[i].name == kTargetNames[j].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(everyPageHasATarget(), "the capture target table must cover every PageId");
static_assert(everyContainerHasATarget(),
              "the capture target table must cover every ContainerScreen");
static_assert(everyNameIsUnique(), "capture target names must be unique");

[[nodiscard]] std::vector<std::string_view> splitOnCommas(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start = 0U;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
        parts.push_back(value.substr(start, end - start));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    return parts;
}

[[nodiscard]] float parseCoordinate(std::string_view value, std::string_view what) {
    int parsed = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("--ui-cursor " + std::string{what} + " must be an integer");
    }
    return static_cast<float>(parsed);
}

[[nodiscard]] std::uint32_t parseExtent(std::string_view value, std::string_view what) {
    std::uint32_t parsed = 0U;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("--ui-size " + std::string{what} + " must be an integer");
    }
    return parsed;
}

} // namespace

std::string_view uiCaptureTargetName(const UiCaptureTarget& target) {
    for (const TargetName& entry : kTargetNames) {
        if (entry.target == target) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<UiCaptureTarget> uiCaptureTargetFromName(std::string_view name) {
    for (const TargetName& entry : kTargetNames) {
        if (entry.name == name) {
            return entry.target;
        }
    }
    return std::nullopt;
}

bool uiCapturePageNeedsWorld(ui::PageId page) {
    return page == ui::PageId::Game || page == ui::PageId::Pause ||
           page == ui::PageId::Death || page == ui::PageId::Loading;
}

bool uiCapturePageShowsWorld(ui::PageId page) {
    // loading 是差集：它要夹具在场，但那一屏画的是全景加进度行，世界画面还看不见。
    return uiCapturePageNeedsWorld(page) && page != ui::PageId::Loading;
}

bool uiCapturePageIsPaused(ui::PageId page) {
    // 游戏内 HUD 是唯一一个"世界在跑"的页面；其余三个都是盖在世界上的界面。
    return page != ui::PageId::Game;
}

std::filesystem::path uiCaptureImagePath(const UiCaptureOptions& options,
                                         const UiCaptureTarget& target, int guiScale) {
    const std::string leaf =
        guiScale == 0 ? std::string{"scale-auto.png"}
                      : "scale-" + std::to_string(guiScale) + ".png";
    return options.root / std::string{uiCaptureTargetName(target)} / leaf;
}

std::size_t uiCaptureImageCount(const UiCaptureOptions& options) {
    return options.targets.size() * options.guiScales.size();
}

std::optional<UiCaptureOptions> parseUiCaptureArguments(
    std::span<const std::string_view> arguments) {
    std::optional<UiCaptureOptions> result;
    bool requestedTargets = false;
    bool requestedScales = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--ui-shot") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-shot requires one or more page names");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            requestedTargets = true;
            for (const std::string_view name : splitOnCommas(arguments[index])) {
                const auto target = uiCaptureTargetFromName(name);
                if (!target.has_value()) {
                    throw std::invalid_argument("--ui-shot does not know the screen name: " +
                                                std::string{name});
                }
                // UI-6-0：需要世界的页面**现在可以拍了**。
                //
                // 从前这里直接拒绝，理由是"世界内容不是命令行的函数"。那句话对的是
                // *真实*世界——随机种子、异步区块流送、每帧推进的模拟。夹具不是：
                // 它是 `--test-scene` 那条路径已经在用的固定单方块场景（固定方块、
                // 固定光照、固定日时、不启动模拟线程），方块预览的八角图已经用它
                // 逐字节复现过很多轮。渲染器按 uiCapturePageNeedsWorld 决定这一页
                // 要不要打开那个夹具，见 VulkanRenderer::Impl::runUiCapture。
                if (std::find(result->targets.begin(), result->targets.end(), *target) !=
                    result->targets.end()) {
                    throw std::invalid_argument("--ui-shot names '" + std::string{name} +
                                                "' twice");
                }
                result->targets.push_back(*target);
            }
        } else if (arguments[index] == "--ui-scale") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-scale requires one or more GUI scales");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            // 第一次 --ui-scale 顶掉默认的两档，之后的追加——否则默认档会混进
            // 用户点名的档里，拍出他没要的图。
            if (!requestedScales) {
                result->guiScales.clear();
                requestedScales = true;
            }
            for (const std::string_view text : splitOnCommas(arguments[index])) {
                int scale = 0;
                const auto [end, error] =
                    std::from_chars(text.data(), text.data() + text.size(), scale);
                if (error != std::errc{} || end != text.data() + text.size() || scale < 0 ||
                    scale > kMaxUiCaptureGuiScale) {
                    throw std::invalid_argument(
                        "--ui-scale takes 0 (Auto) through " +
                        std::to_string(kMaxUiCaptureGuiScale) + ", got: " + std::string{text});
                }
                if (std::find(result->guiScales.begin(), result->guiScales.end(), scale) !=
                    result->guiScales.end()) {
                    throw std::invalid_argument("--ui-scale names " + std::to_string(scale) +
                                                " twice");
                }
                result->guiScales.push_back(scale);
            }
        } else if (arguments[index] == "--ui-size") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-size requires <width>x<height>");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            const std::string_view value = arguments[index];
            const std::size_t separator = value.find('x');
            if (separator == std::string_view::npos) {
                throw std::invalid_argument("--ui-size requires <width>x<height>, got: " +
                                            std::string{value});
            }
            result->width = parseExtent(value.substr(0U, separator), "width");
            result->height = parseExtent(value.substr(separator + 1U), "height");
            if (result->width < kMinUiCaptureWidth || result->height < kMinUiCaptureHeight ||
                result->width > kMaxUiCaptureExtent || result->height > kMaxUiCaptureExtent) {
                throw std::invalid_argument(
                    "--ui-size must be at least " + std::to_string(kMinUiCaptureWidth) + "x" +
                    std::to_string(kMinUiCaptureHeight) + " and at most " +
                    std::to_string(kMaxUiCaptureExtent) + " on each axis");
            }
        } else if (arguments[index] == "--ui-cursor") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-cursor requires <x>,<y>");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            const std::string_view value = arguments[index];
            const std::size_t comma = value.find(',');
            if (comma == std::string_view::npos) {
                throw std::invalid_argument("--ui-cursor requires <x>,<y>, got: " +
                                            std::string{value});
            }
            // 逗号分隔而不是 `x`：坐标可以是负数，而负号会把 `x` 那种写法读歪。
            result->cursorX = parseCoordinate(value.substr(0U, comma), "x");
            result->cursorY = parseCoordinate(value.substr(comma + 1U), "y");
        } else if (arguments[index] == "--ui-tab") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-tab requires an index");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            const std::string_view value = arguments[index];
            std::uint32_t parsed = 0U;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size()) {
                throw std::invalid_argument("--ui-tab takes a non-negative integer, got: " +
                                            std::string{value});
            }
            result->tabIndex = parsed;
        } else if (arguments[index] == "--ui-focus") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-focus requires a step count");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            const std::string_view value = arguments[index];
            std::uint32_t parsed = 0U;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size()) {
                throw std::invalid_argument("--ui-focus takes a non-negative integer, got: " +
                                            std::string{value});
            }
            result->focusSteps = parsed;
        } else if (arguments[index] == "--ui-carry") {
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            result->carryStack = true;
        } else if (arguments[index] == "--ui-out") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-out requires a directory");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            result->root = std::filesystem::path{std::string{arguments[index]}};
        }
    }

    // --ui-scale / --ui-size / --ui-out / --ui-cursor 单独出现是没有意义的：它们描述一次拍摄的
    // 参数，而拍什么由 --ui-shot 说了算。给了参数却没给页面，报错而不是默默不拍。
    if (result.has_value() && !requestedTargets) {
        throw std::invalid_argument("--ui-scale, --ui-size and --ui-out require --ui-shot");
    }
    if (result.has_value() && result->guiScales.empty()) {
        throw std::invalid_argument("--ui-scale requires at least one GUI scale");
    }
    return result;
}

} // namespace mc::render
