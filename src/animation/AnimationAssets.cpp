#include "animation/AnimationAssets.hpp"

#include "core/Json.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mc::animation {
namespace {

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("failed to open animation asset: " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

SkeletalModel loadModelFile(const std::filesystem::path& path, std::string_view identifier) {
    return SkeletalModel::loadGeometry(core::Json::parse(readFile(path)), identifier);
}

AnimationLibrary loadAnimationFile(const std::filesystem::path& path) {
    AnimationLibrary library;
    library.loadDocument(core::Json::parse(readFile(path)));
    return library;
}

AnimatedModel loadAnimatedModel(const std::filesystem::path& geometryPath,
                                const std::vector<std::filesystem::path>& animationPaths,
                                std::string_view identifier) {
    AnimatedModel bundle;
    bundle.model = loadModelFile(geometryPath, identifier);
    for (const auto& animationPath : animationPaths) {
        bundle.animations.loadDocument(core::Json::parse(readFile(animationPath)));
    }
    return bundle;
}

} // namespace mc::animation
