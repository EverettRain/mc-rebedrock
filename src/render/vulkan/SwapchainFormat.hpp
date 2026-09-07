#pragma once

// 交换链与场景图的格式契约。
//
// 本 build 把整帧画进一张离屏场景图（世界那趟与界面那趟都写它），帧末用
// `vkCmdCopyImage` 把它逐字节搬进交换链图像。逐字节搬运不做任何转换，所以两张图的
// 字节布局必须对得上——这一点 `sceneImageFormat` 一直做到了：它按交换链的通道序
// 选 BGRA 还是 RGBA。
//
// 但**通道序相同不等于格式相同**。`chooseSurfaceFormat` 从前第一优先挑
// `B8G8R8A8_SRGB`，而场景图恒为 `_UNORM`，于是那次拷贝是一次**跨格式**拷贝——
// Vulkan 允许（两者 size-compatible），代价是实现必须为其中一张图建一个重解释格式的
// 纹理视图。MoltenVK v1.4.2 在这条路径上有两个缺陷（2026-09-07 的 macOS Release 诊断
// 逐条反汇编确认，见 export/diagnostics/2026-09-07-release/analysis.md）：
//
//   1. `MVKImagePlane::getMTLTexture(format)` 以格式为键缓存 `_mtlTextureViews`，而交换链
//      的 `releaseMetalDrawable()` 只释放 drawable、不让这份缓存失效。换到新 drawable 之后
//      仍可能写向旧 drawable 的视图，呈现的却是当前 drawable —— 闪烁、旧帧、黑帧。
//   2. `MVKSwapchainImage::destroy()` 先 `detachSwapchain()` 把 `_device` 置空，随后基类析构
//      清理 plane 时 `releaseMTLTexture()` 又无条件经这个指针访问 live resource set —— 退出
//      时 `EXC_BAD_ACCESS`。触发条件正是"交换链图像存在缓存的纹理视图"。
//
// 两个缺陷同一个入口：**只要不建那个视图就都不会发生**。所以格式选择的第一优先改成
// 与场景图逐字节同格式的 `_UNORM`，让那次拷贝变成同格式拷贝。
//
// 这对显示结果是中性的：present 时字节如何被解释由 **colorSpace**（`SRGB_NONLINEAR`）
// 决定，不由 format 决定；而交换链图像在本 build 里既不被当渲染目标写（它是被 copy 进去的）
// 也不被采样，所以 format 的 `_SRGB` / `_UNORM` 之分对它没有任何作用。整帧仍在 sRGB
// 编码值上合成（见 `sceneUnormFormat` 与 gui pass 的注释），这条不变。

#include <vulkan/vulkan.h>

#include <span>
#include <stdexcept>

namespace mc::render {

// 界面那趟把编码值直接合成进场景图，帧末又逐字节搬出去，所以只接受 8 位 RGBA/BGRA。
[[nodiscard]] constexpr bool isSupportedSurfaceFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
}

// 场景图的格式：跟随交换链的**通道序**，但永远是 UNORM——着色器写的就是最终要显示的
// sRGB 编码值，中间不能再经过任何传输函数。
[[nodiscard]] constexpr VkFormat sceneImageFormat(VkFormat swapchainFormat) {
    return swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                   swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB
               ? VK_FORMAT_R8G8B8A8_UNORM
               : VK_FORMAT_B8G8R8A8_UNORM;
}

// 帧末那次 `vkCmdCopyImage` 是不是同格式拷贝。false 表示实现要建重解释格式的纹理视图，
// 那正是 MoltenVK 两个缺陷的入口（见文件头）。调用方据此决定是照常跑还是先说清楚。
[[nodiscard]] constexpr bool presentCopyIsSameFormat(VkFormat swapchainFormat) {
    return swapchainFormat == sceneImageFormat(swapchainFormat);
}

// 交换链格式选择。优先级的第一条是本文件头说的那件事：挑一个与场景图**完全同格式**的，
// 让帧末那次逐字节拷贝不必跨格式。
//
// 退路是有意保留而不是 fatal 的：跨格式拷贝在 Vulkan 里合法，原生驱动与 lavapipe 都正确
// 执行，只有 MoltenVK v1.4.2 那条路径有缺陷。一个只列出 `_SRGB` 的 surface 上，阻断启动
// 比接受一次跨格式拷贝更糟——但调用方必须把它说出来（`presentCopyIsSameFormat`）。
[[nodiscard]] inline VkSurfaceFormatKHR
chooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats) {
    // 1. 与场景图逐字节同格式，且是标准 sRGB 色彩空间。
    for (const auto& format : formats) {
        if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            isSupportedSurfaceFormat(format.format) && presentCopyIsSameFormat(format.format)) {
            return format;
        }
    }
    // 2. 退而求其次：色彩空间对，格式受支持但要跨格式拷贝。
    for (const auto& format : formats) {
        if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            isSupportedSurfaceFormat(format.format)) {
            return format;
        }
    }
    // 3. 连色彩空间都挑不到时，只要字节布局能对上就先跑起来。
    if (!formats.empty() && isSupportedSurfaceFormat(formats.front().format)) {
        return formats.front();
    }
    throw std::runtime_error(
        "No 8-bit RGBA/BGRA surface format: the GUI pass composites into a byte-compatible "
        "scene image and the frame is copied out verbatim");
}

} // namespace mc::render
