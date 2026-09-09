#pragma once

#include "assets/ResourcePackLibrary.hpp"
#include "config/GameOptions.hpp"
#include "render/MeshData.hpp"
#include "render/TestScene.hpp"
#include "render/UiCapture.hpp"

#include <filesystem>
#include <memory>

namespace mc::world {
class ChunkStreamer;
}

namespace mc::render {

class VulkanRenderer final {
  public:
    VulkanRenderer(
        std::filesystem::path shaderRoot,
        // 资源包库，而不是一个已经叠好的 provider：渲染器除了读资源，还要让前端
        // 的资源包选择界面列出包、启停、调序、提交。传 const ResourceProvider&
        // 时那些操作一个都够不着——PackManager 是 Application::run() 的局部变量，
        // 渲染器既看不见它，也换不掉手里这个 provider
        assets::ResourcePackLibrary& packLibrary,
        world::ChunkStreamer& chunkStreamer,
        config::GameOptions options,
        std::filesystem::path optionsPath,
        std::filesystem::path saveRoot,
        std::optional<TestSceneOptions> testScene = std::nullopt,
        // UI-2：界面截图通道。给了它就停在指定的前端页面上逐档拍图然后退出，
        // 与 testScene 互斥（两个"我在拍什么"的答案，main 会拒绝同时给）
        std::optional<UiCaptureOptions> uiCapture = std::nullopt);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    VulkanRenderer(VulkanRenderer&&) = delete;
    VulkanRenderer& operator=(VulkanRenderer&&) = delete;

    [[nodiscard]] int run();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mc::render
