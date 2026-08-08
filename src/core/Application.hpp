#pragma once

#include <filesystem>
#include <optional>

#include "render/TestScene.hpp"

namespace mc {

class Application final {
  public:
    Application(
        std::filesystem::path resourceRoot,
        std::filesystem::path shaderRoot,
        std::filesystem::path configRoot,
        std::optional<render::TestSceneOptions> testScene = std::nullopt);

    [[nodiscard]] int run();

  private:
    std::filesystem::path resourceRoot_;
    std::filesystem::path shaderRoot_;
    std::filesystem::path configRoot_;
    std::optional<render::TestSceneOptions> testScene_;
};

} // namespace mc
