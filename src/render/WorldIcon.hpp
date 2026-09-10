#pragma once

// 世界图标：把一帧画面变成 `<world>/icon.png` 里那张 64x64 缩略图的那段算术。
//
// 原版在退出世界时自动截一张图存成世界图标（GameRenderer.takeAutoScreenshot），
// 本文件只搬那一步「帧缓冲像素 -> 64x64 RGBA8」的纯计算：不碰 Vulkan、不碰
// 文件系统、不碰任何全局状态，因此能被无头单测逐像素钉住。取像素是渲染层的事，
// 落盘是 SaveRepository::writeIcon 的事，中间这段算术只有这一份。

#include <cstdint>
#include <span>
#include <vector>

namespace mc::render {

// 世界图标的边长。原版写死在 GameRenderer.java:648 的 `new NativeImage(64, 64,
// false)`，服务端那边也用同一个数字校验外来图标
// （MinecraftServer.java:929 "Invalid world icon size ... but expected [64, 64]"）。
inline constexpr int kWorldIconSize = 64;

// 每像素字节数。图标是 RGBA8，与 SaveRepository::writeIcon 收的那份缓冲同格式
// ——writeIcon 会拿 width*height*4 去喂 PNG 编码器，两边对不上就是越界读，所以
// 这里产出的长度必须正好是它期望的长度。
inline constexpr int kWorldIconChannels = 4;

// 一张世界图标的字节数，写成表达式而不是又一个字面量。
inline constexpr std::size_t kWorldIconBytes =
    static_cast<std::size_t>(kWorldIconSize) * kWorldIconSize * kWorldIconChannels;

// 把一张 RGBA8 帧缓冲（`rgba`，逐行紧密排列、共 width*height*4 字节）缩成
// kWorldIconSize 见方的世界图标，返回 kWorldIconBytes 字节。
//
// 两步，与 26.1 同序：
//   1. 按短边居中裁成正方形。这一步的整数算术逐字照抄
//      client/net/minecraft/client/renderer/GameRenderer.java:636-646，
//      连除法的先后都没动（见 .cpp 里的引文）。
//   2. 把那个正方形窗口重采样到 64x64。
//
// ★ 第 2 步**不是**原版的位级复刻，别当它是。原版
//   NativeImage.resizeSubRectTo（NativeImage.java:434-452）把活交给
//   STBImageResize.nstbir_resize_uint8_linear，也就是 stb 的带滤波重采样
//   （缩小时默认 Mitchell 核），不是逐像素点采。本作这里用最近邻，采样映射与
//   仓库里既有的那份 render::resizedRegion（src/render/vulkan/AtlasLayerFit.hpp）
//   完全一致，是一条**登记在案的偏差**：图标是给人认世界用的缩略图，不是要与
//   原版逐字节比对的产物，而把 stb 的核搬进来只为一张 64x64 缩略图不划算。
//   哪天要求变成「与原版逐字节一致」，改的是这里，不是调用方。
//
// 以下情况返回空 vector（调用方据此跳过写图标，而不是写一张坏图）：
//   * width 或 height 不为正；
//   * rgba 的长度不正好是 width*height*4。
// 第二条不是防御性客套：少一个字节就是越界读，而越界读会产出一张看着完全正常的
// PNG。这里是唯一同时看得见「长度」和「声明的尺寸」的地方。
[[nodiscard]] std::vector<std::uint8_t> worldIconFromFrame(std::span<const std::uint8_t> rgba,
                                                           int width, int height);

} // namespace mc::render
