#pragma once

// 渲染器内核（VulkanRenderer.cpp）与 HUD 绘制子系统（HudRenderer.hpp）共用的 HUD/前端类型与常量
// 放在 mc::render 而不是某个 .cpp 的匿名命名空间里，两边才能指同一份定义

#include "gameplay/ScreenHandler.hpp"

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace mc::render {

// guiTextures 数组的层号，与 createGuiTexture() 保持同步
// 11 是 misc/vignette.png，12 是烘焙好的 Screen.renderBackground 暗角渐变
inline constexpr float kVignetteGuiLayer = 11.0F;
inline constexpr float kScreenDimGuiLayer = 12.0F;
inline constexpr float kMenuListBackgroundGuiLayer = 13.0F;
// 标题界面的六张全景面，拼成 logo 背后的那个世界；标题轮播把它们当幻灯片循环
inline constexpr std::size_t kPanoramaFaces = 6U;
// 26.1 的十个内容页签加上"背包"伪页签，七个在上排、四个在下排
// 对应 tab_top_1..7 与 tab_bottom_1..4
inline constexpr std::size_t kCreativeTabCount = 11U;

// "当前开着哪个界面"是玩法事实，槽位路由要据此分派
// 所以枚举跟 ScreenHandler 放在一起，渲染器只是引用它
using ContainerScreen = gameplay::ContainerScreen;

struct HudPush final {
    glm::vec4 rect;
    glm::vec4 color;
    glm::vec4 uvRect;
    glm::vec4 data;
};

// 标题全景立方体：x = 偏航、y = 俯仰（弧度）、z = tan(fov/2)、w = 宽高比
// blur.x 是只作用于背景的模糊半径，单位为帧缓冲像素（26.1 默认 5），其余分量保留
struct PanoramaPush final {
    glm::vec4 rotationFov;
    glm::vec4 blur;
};

struct ItemPush final {
    glm::vec4 positionSize;
    glm::vec4 textureLayersRotation;
    glm::vec4 data;
    // 非等比长方体的可选 xyz 尺寸；零向量表示沿用 positionSize.w 里的标量尺寸
    glm::vec4 dimensions;
    glm::mat4 viewModelTransform{1.0F};
};

static_assert(sizeof(ItemPush) <= 128U, "Item push constants must fit Vulkan's guaranteed minimum");

} // namespace mc::render
