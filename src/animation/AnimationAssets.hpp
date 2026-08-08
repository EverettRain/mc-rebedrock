#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/SkeletalModel.hpp"

#include <filesystem>

namespace mc::animation {

// A geometry paired with the animation clips authored for it: the runtime
// bundle the game hands to an Animator. Blocks, the player, NPCs and mobs are
// all represented by this same structure.
struct AnimatedModel final {
    SkeletalModel model;
    AnimationLibrary animations;
};

// Reads and parses a single Bedrock geometry file.
[[nodiscard]] SkeletalModel loadModelFile(const std::filesystem::path& path,
                                          std::string_view identifier = {});

// Reads and parses a single Bedrock animation file into a library.
[[nodiscard]] AnimationLibrary loadAnimationFile(const std::filesystem::path& path);

// Reads a geometry file and one or more animation files, merging every clip
// into the bundle's library.
[[nodiscard]] AnimatedModel loadAnimatedModel(
    const std::filesystem::path& geometryPath,
    const std::vector<std::filesystem::path>& animationPaths,
    std::string_view identifier = {});

} // namespace mc::animation
