#pragma once

#include "assets/ResourceProvider.hpp"
#include "config/GameOptions.hpp"
#include "render/MeshData.hpp"
#include "render/TestScene.hpp"

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
        const assets::ResourceProvider& resourceProvider,
        world::ChunkStreamer& chunkStreamer,
        config::GameOptions options,
        std::filesystem::path optionsPath,
        std::filesystem::path saveRoot,
        std::optional<TestSceneOptions> testScene = std::nullopt);
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
