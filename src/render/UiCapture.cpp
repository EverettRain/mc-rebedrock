#include "render/UiCapture.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mc::render {
namespace {

struct PageName final {
    ui::PageId page;
    std::string_view name;
};

// 一张表，两个方向都读它，于是名字与 PageId 不可能各说各话。
constexpr std::array kPageNames{
    PageName{ui::PageId::Title, "title"},
    PageName{ui::PageId::WorldList, "world-list"},
    PageName{ui::PageId::CreateWorld, "create-world"},
    PageName{ui::PageId::EditWorld, "edit-world"},
    PageName{ui::PageId::ConfirmDelete, "confirm-delete"},
    PageName{ui::PageId::Loading, "loading"},
    PageName{ui::PageId::Game, "game"},
    PageName{ui::PageId::Pause, "pause"},
    PageName{ui::PageId::Death, "death"},
    PageName{ui::PageId::Options, "options"},
    PageName{ui::PageId::VideoSettings, "video-settings"},
    PageName{ui::PageId::Controls, "controls"},
    PageName{ui::PageId::Language, "language"},
    PageName{ui::PageId::Experimental, "experimental"},
};

// 表必须覆盖 PageId 的每一个取值，否则 --ui-shot 会对某个真实存在的屏幕说"不认识"。
static_assert(kPageNames.size() == static_cast<std::size_t>(ui::PageId::Experimental) + 1U,
              "the capture page-name table must cover every PageId");

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

std::string_view uiCapturePageName(ui::PageId page) {
    for (const PageName& entry : kPageNames) {
        if (entry.page == page) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<ui::PageId> uiCapturePageFromName(std::string_view name) {
    for (const PageName& entry : kPageNames) {
        if (entry.name == name) {
            return entry.page;
        }
    }
    return std::nullopt;
}

bool uiCapturePageNeedsWorld(ui::PageId page) {
    return page == ui::PageId::Game || page == ui::PageId::Pause ||
           page == ui::PageId::Death || page == ui::PageId::Loading;
}

std::filesystem::path uiCaptureImagePath(const UiCaptureOptions& options, ui::PageId page,
                                         int guiScale) {
    const std::string leaf =
        guiScale == 0 ? std::string{"scale-auto.png"}
                      : "scale-" + std::to_string(guiScale) + ".png";
    return options.root / std::string{uiCapturePageName(page)} / leaf;
}

std::size_t uiCaptureImageCount(const UiCaptureOptions& options) {
    return options.pages.size() * options.guiScales.size();
}

std::optional<UiCaptureOptions> parseUiCaptureArguments(
    std::span<const std::string_view> arguments) {
    std::optional<UiCaptureOptions> result;
    bool requestedPages = false;
    bool requestedScales = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--ui-shot") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--ui-shot requires one or more page names");
            }
            if (!result.has_value()) {
                result = UiCaptureOptions{};
            }
            requestedPages = true;
            for (const std::string_view name : splitOnCommas(arguments[index])) {
                const auto page = uiCapturePageFromName(name);
                if (!page.has_value()) {
                    throw std::invalid_argument("--ui-shot does not know the page name: " +
                                                std::string{name});
                }
                // 需要世界的页面拍出来不是命令行的函数，两次运行也不会一样。
                // 与其给出一张看着像成功的错图，不如现在就说不行。
                if (uiCapturePageNeedsWorld(*page)) {
                    throw std::invalid_argument(
                        "--ui-shot cannot capture '" + std::string{name} +
                        "': it needs an open world, which is not a function of the command line");
                }
                if (std::find(result->pages.begin(), result->pages.end(), *page) !=
                    result->pages.end()) {
                    throw std::invalid_argument("--ui-shot names '" + std::string{name} +
                                                "' twice");
                }
                result->pages.push_back(*page);
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

    // --ui-scale / --ui-size / --ui-out 单独出现是没有意义的：它们描述一次拍摄的
    // 参数，而拍什么由 --ui-shot 说了算。给了参数却没给页面，报错而不是默默不拍。
    if (result.has_value() && !requestedPages) {
        throw std::invalid_argument("--ui-scale, --ui-size and --ui-out require --ui-shot");
    }
    if (result.has_value() && result->guiScales.empty()) {
        throw std::invalid_argument("--ui-scale requires at least one GUI scale");
    }
    return result;
}

} // namespace mc::render
