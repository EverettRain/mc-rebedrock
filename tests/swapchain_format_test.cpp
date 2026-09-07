// 交换链格式选择：帧末那次 vkCmdCopyImage 必须是**同格式**拷贝。
//
// 2026-09-07 的 macOS Release 诊断把黑帧与退出崩溃都定位到 MoltenVK v1.4.2 的
// 跨格式纹理视图缓存上（见 SwapchainFormat.hpp 的文件头）。触发条件不是某个 Vulkan
// 调用写错了——那次拷贝完全合法——而是交换链挑了 `_SRGB` 而场景图恒为 `_UNORM`，
// 于是实现被迫建一个重解释格式的视图。这里钉住的就是「不要让它被迫」。
//
// 这些断言是纯值的：格式选择与场景图格式都是 constexpr / inline 函数，不碰设备、
// 不建管线，所以 headless 可证。真机上「崩溃是否消失」不在这里，那是 macOS 门控。

#include "render/vulkan/SwapchainFormat.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        std::cerr << "swapchain_format_test:" << line << " FAILED: " << message << '\n';
        ++failures;
    }
}

#define REQUIRE(condition, message) require((condition), (message), __LINE__)

constexpr VkColorSpaceKHR kSrgb = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
// 任意一个非 sRGB 的色彩空间，用来证明第一条优先级也看 colorSpace 而不只看格式。
constexpr VkColorSpaceKHR kOther = VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;

// ---------------------------------------------------------------------------
// 1. 同格式优先：SRGB 排在列表前面也不该被选中。
//
// 这是本轮的核心。macOS 上 surface 同时列出 B8G8R8A8_SRGB 与 B8G8R8A8_UNORM，
// 而从前的第一条优先级写死 `format == VK_FORMAT_B8G8R8A8_SRGB`，于是必然挑中 SRGB。
void testSameFormatWins() {
    const std::vector<VkSurfaceFormatKHR> macOsLike{
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgb},
        {VK_FORMAT_B8G8R8A8_UNORM, kSrgb},
    };
    const auto chosen = mc::render::chooseSurfaceFormat(macOsLike);
    REQUIRE(chosen.format == VK_FORMAT_B8G8R8A8_UNORM,
            "SRGB 排在前面时仍须挑同格式的 UNORM");
    REQUIRE(chosen.colorSpace == kSrgb, "色彩空间必须仍是 SRGB_NONLINEAR");
}

// 2. 选出来的格式与场景图逐字节同格式 —— 这条是「为什么要挑它」本身。
void testChosenFormatMatchesSceneImage() {
    const std::vector<std::vector<VkSurfaceFormatKHR>> candidates{
        {{VK_FORMAT_B8G8R8A8_SRGB, kSrgb}, {VK_FORMAT_B8G8R8A8_UNORM, kSrgb}},
        {{VK_FORMAT_R8G8B8A8_SRGB, kSrgb}, {VK_FORMAT_R8G8B8A8_UNORM, kSrgb}},
        {{VK_FORMAT_B8G8R8A8_UNORM, kSrgb}},
        {{VK_FORMAT_R8G8B8A8_UNORM, kSrgb}},
    };
    for (const auto& formats : candidates) {
        const auto chosen = mc::render::chooseSurfaceFormat(formats);
        REQUIRE(mc::render::presentCopyIsSameFormat(chosen.format),
                "列表里有同格式可选时，选出的格式必须与场景图相同");
        REQUIRE(chosen.format == mc::render::sceneImageFormat(chosen.format),
                "presentCopyIsSameFormat 与 sceneImageFormat 必须一致");
    }
}

// 3. 通道序仍然跟随交换链 —— 逐字节拷贝要求 BGRA 场景图不能搬进 RGBA 交换链。
void testChannelOrderFollowsSwapchain() {
    REQUIRE(mc::render::sceneImageFormat(VK_FORMAT_B8G8R8A8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM,
            "BGRA 交换链的场景图必须是 BGRA");
    REQUIRE(mc::render::sceneImageFormat(VK_FORMAT_B8G8R8A8_SRGB) == VK_FORMAT_B8G8R8A8_UNORM,
            "BGRA_SRGB 交换链的场景图必须是 BGRA_UNORM");
    REQUIRE(mc::render::sceneImageFormat(VK_FORMAT_R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM,
            "RGBA 交换链的场景图必须是 RGBA");
    REQUIRE(mc::render::sceneImageFormat(VK_FORMAT_R8G8B8A8_SRGB) == VK_FORMAT_R8G8B8A8_UNORM,
            "RGBA_SRGB 交换链的场景图必须是 RGBA_UNORM");
}

// 4. 退路：只列出 _SRGB 的 surface 上仍然要能起来，但那次拷贝是跨格式的，
//    且这件事必须**可被检测**（调用方据此打日志）。
void testSrgbOnlySurfaceStillWorksButIsDetectable() {
    const std::vector<VkSurfaceFormatKHR> srgbOnly{{VK_FORMAT_B8G8R8A8_SRGB, kSrgb}};
    const auto chosen = mc::render::chooseSurfaceFormat(srgbOnly);
    REQUIRE(chosen.format == VK_FORMAT_B8G8R8A8_SRGB, "只有 SRGB 时仍须选它而不是抛异常");
    REQUIRE(!mc::render::presentCopyIsSameFormat(chosen.format),
            "跨格式拷贝必须能被 presentCopyIsSameFormat 检出");
}

// 5. 第一条优先级同时看 colorSpace：一个 UNORM 但色彩空间不对的，不该越过
//    一个 colorSpace 正确的候选。
void testColorSpaceGatesTheFirstChoice() {
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_B8G8R8A8_UNORM, kOther},
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgb},
    };
    const auto chosen = mc::render::chooseSurfaceFormat(formats);
    REQUIRE(chosen.colorSpace == kSrgb, "色彩空间不对的 UNORM 不该被第一条优先级选中");
}

// 6. 一个字节布局都对不上的 surface 必须抛，而不是静默画错。
void testUnsupportedSurfaceThrows() {
    const std::vector<VkSurfaceFormatKHR> unsupported{{VK_FORMAT_R5G6B5_UNORM_PACK16, kSrgb}};
    bool threw = false;
    try {
        static_cast<void>(mc::render::chooseSurfaceFormat(unsupported));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    REQUIRE(threw, "非 8 位 RGBA/BGRA 的 surface 必须抛异常");
}

// 7. 源码护栏：格式选择只能有一份。
//
// 这条防的是「有人又在 VulkanRenderer::Impl 里写了第二份 chooseSurfaceFormat」——
// 那样两份会分头演化，而本仓在形状、LCG、终端光照算式上各栽过一次。函数被移走之后
// 编译期不会再有冲突，所以这里靠读源码钉住。
void testRendererHasNoSecondFormatChooser() {
    std::ifstream source{MC_REBEDROCK_RENDERER_SRC};
    REQUIRE(source.is_open(), "读不到 VulkanRenderer.cpp");
    if (!source.is_open()) {
        return;
    }
    std::stringstream buffer;
    buffer << source.rdbuf();
    const std::string text = buffer.str();
    REQUIRE(text.find("render/vulkan/SwapchainFormat.hpp") != std::string::npos,
            "VulkanRenderer.cpp 必须 include 格式契约的单一源");
    REQUIRE(text.find("VkSurfaceFormatKHR\n    chooseSurfaceFormat") == std::string::npos &&
                text.find("VkSurfaceFormatKHR chooseSurfaceFormat") == std::string::npos,
            "VulkanRenderer.cpp 不得再自己定义 chooseSurfaceFormat");
    REQUIRE(text.find("bool isSupportedSurfaceFormat") == std::string::npos,
            "VulkanRenderer.cpp 不得再自己定义 isSupportedSurfaceFormat");
    // 跨格式那件事必须仍被说出来；删掉这段日志会让 SRGB-only 的 surface 静默走进
    // 已知有缺陷的 MoltenVK 路径。
    REQUIRE(text.find("presentCopyIsSameFormat") != std::string::npos,
            "createSwapchain 必须检查并报告跨格式拷贝");
}

} // namespace

int main() {
    testSameFormatWins();
    testChosenFormatMatchesSceneImage();
    testChannelOrderFollowsSwapchain();
    testSrgbOnlySurfaceStillWorksButIsDetectable();
    testColorSpaceGatesTheFirstChoice();
    testUnsupportedSurfaceThrows();
    testRendererHasNoSecondFormatChooser();
    if (failures != 0) {
        std::cerr << "swapchain_format_test: " << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "swapchain_format_test ok\n";
    return EXIT_SUCCESS;
}
