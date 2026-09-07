#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "render/TestScene.hpp"
#include "render/UiCapture.hpp"

namespace mc {

// 一次运行的编排者
// 构造时只记下三个根目录（资源、着色器、配置）和可选的测试场景
// 全部实际工作在 run() 里，返回值即进程退出码
class Application final {
  public:
    Application(
        std::filesystem::path resourceRoot,
        std::filesystem::path shaderRoot,
        std::filesystem::path configRoot,
        std::optional<render::TestSceneOptions> testScene = std::nullopt,
        std::vector<std::filesystem::path> commandLinePacks = {},
        // UI-2：界面截图通道。给了它就停在指定前端页面上逐档拍图然后退出
        std::optional<render::UiCaptureOptions> uiCapture = std::nullopt);

    [[nodiscard]] int run();

  private:
    std::filesystem::path resourceRoot_;
    std::filesystem::path shaderRoot_;
    std::filesystem::path configRoot_;
    std::optional<render::TestSceneOptions> testScene_;
    std::optional<render::UiCaptureOptions> uiCapture_;
    // `--pack` 点名的资源包，排在 <游戏根>/resourcepacks 扫描结果之后（优先级最高）
    std::vector<std::filesystem::path> commandLinePacks_;
};

} // namespace mc
