#include "core/Application.hpp"

#include "config/GameOptions.hpp"
#include "render/vulkan/VulkanRenderer.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/WorldConstants.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mc {
namespace {

[[nodiscard]] int developmentLoadRadius(int configuredRadius) {
    constexpr int kMaximumRadius = 36;
    const char* value = std::getenv("MC_REBEDROCK_VIEW_DISTANCE");
    if (value == nullptr) {
        return configuredRadius;
    }

    const std::string_view text{value};
    int radius = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), radius);
    if (error != std::errc{} || end != text.data() + text.size() ||
        radius < 0 || radius > kMaximumRadius) {
        throw std::invalid_argument(
            "MC_REBEDROCK_VIEW_DISTANCE must be an integer between 0 and 36");
    }
    return radius;
}

} // namespace

Application::Application(
    std::filesystem::path resourceRoot,
    std::filesystem::path shaderRoot,
    std::filesystem::path configRoot,
    std::optional<render::TestSceneOptions> testScene)
    : resourceRoot_(std::move(resourceRoot)),
      shaderRoot_(std::move(shaderRoot)),
      configRoot_(std::move(configRoot)), testScene_(testScene) {}

int Application::run() {
    const auto vanillaRoot = resourceRoot_ / "vanilla" / "1.16.1";

    std::cout << "MC Rebedrock Vulkan milestone\n";
    std::cout << "Chunk: " << world::kChunkWidth << 'x' << world::kChunkDepth << 'x'
              << world::kWorldHeight << "\n";
    std::cout << "Sections per chunk: " << world::kSectionCount << "\n";
    std::cout << "Sea level: " << world::kSeaLevel << "\n";
    std::cout << "Resources: " << vanillaRoot << "\n";

    if (!std::filesystem::is_directory(vanillaRoot)) {
        std::cerr << "Warning: extracted Minecraft 1.16.1 resources were not found.\n";
    }

    const auto optionsPath = configRoot_ / "options.properties";
    config::GameOptions options = config::GameOptions::load(optionsPath);
    const int loadRadius = testScene_.has_value()
        ? 0 : developmentLoadRadius(options.viewDistance);
    const int unloadRadius = loadRadius;
    world::ChunkStreamer chunkStreamer{
        0x5EEDULL,
        loadRadius,
        unloadRadius,
    };
    const int diameter = loadRadius * 2 + 1;
    std::cout << "Streaming window: " << diameter << 'x' << diameter
              << " chunks (load radius " << loadRadius
              << ", unload radius " << unloadRadius << ")\n";

    render::VulkanRenderer renderer{
        shaderRoot_,
        vanillaRoot / "textures" / "minecraft" / "block",
        vanillaRoot / "audio" / "minecraft" / "sounds",
        chunkStreamer,
        options,
        optionsPath,
        configRoot_.parent_path() / "saves",
        testScene_};
    renderer.run();
    chunkStreamer.stop();

    return 0;
}

} // namespace mc
