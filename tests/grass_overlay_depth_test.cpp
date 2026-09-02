#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif
#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

// A grass block's side is two coplanar quads: the opaque dirt base, then the
// tinted grass overlay in the cutout mesh (vanilla's grass_block.json draws its
// sides from two elements, and a pre-composited texture cannot be tinted without
// turning the dirt green). Coplanar means the overlay only survives the depth
// test if the cutout pipeline compares LESS_OR_EQUAL and both pipelines compute
// bit-identical clip coordinates for the shared corners.
//
// The overlay used to be pushed 0.001 blocks along the face normal instead. The
// depth difference a buffer can resolve at distance d falls off as d^2: on
// D32_SFLOAT with a 0.1 near plane, 0.001 blocks is under one float ULP past
// roughly 40 blocks, so the two quads quantise to the same depth, LESS rejects
// the overlay, and every distant slope's grass edge flickers back to the green
// baked into grass_block_side.png as the camera moves.
//
// None of that is reachable headlessly — a test build never creates a pipeline —
// so this reads the two invariants out of the source, the way
// shader_descriptor_bindings_test reads binding numbers out of both sides. The
// geometry half (overlay corners bit-identical to the base) is asserted in
// biome_colors_test's checkGrassSideOverlay.

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"grass_overlay_depth_test line " + std::to_string(line) + ": " +
                                 message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(static_cast<bool>(input), "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// Strips `//` line comments, so the prose explaining why LESS_OR_EQUAL is needed
// does not itself satisfy the check.
[[nodiscard]] std::string stripLineComments(const std::string& source) {
    std::string result;
    result.reserve(source.size());
    std::istringstream lines{source};
    std::string line;
    while (std::getline(lines, line)) {
        const auto comment = line.find("//");
        result.append(comment == std::string::npos ? line : line.substr(0, comment));
        result.push_back('\n');
    }
    return result;
}

// The cutout pipeline must be created while depthCompareOp is LESS_OR_EQUAL.
// Read the renderer's pipeline-building code in order and track the last value
// assigned to depthCompareOp before vkCreateGraphicsPipelines writes
// cutoutPipeline — that is what the driver actually receives.
void checkCutoutPipelineComparesLessOrEqual() {
    const std::string source =
        stripLineComments(readFile(std::filesystem::path{MC_REBEDROCK_RENDERER_SRC}));

    const std::regex creation{R"(&worldPipelines_\.cutoutPipeline)"};
    std::smatch cutoutMatch;
    REQUIRE(std::regex_search(source, cutoutMatch, creation),
            "no vkCreateGraphicsPipelines call for cutoutPipeline found in the renderer");
    const std::string before = source.substr(0, static_cast<std::size_t>(cutoutMatch.position()));

    const std::regex assignment{R"(depthStencil\.depthCompareOp\s*=\s*(VK_COMPARE_OP_\w+))"};
    std::string last;
    for (auto it = std::sregex_iterator{before.begin(), before.end(), assignment};
         it != std::sregex_iterator{}; ++it) {
        last = (*it)[1].str();
    }
    REQUIRE(!last.empty(), "depthCompareOp is never assigned before the cutout pipeline is built");
    REQUIRE(last == "VK_COMPARE_OP_LESS_OR_EQUAL",
            "the cutout pipeline is built with depthCompareOp " + last +
                ", but the grass side overlay is coplanar with the opaque quad under it and only "
                "passes a LESS_OR_EQUAL test. Do not restore a geometric offset instead: a fixed "
                "world-space nudge falls below one depth ULP at range.");
}

// The opaque and cutout pipelines share grass_block.vert. Without `invariant
// gl_Position` a driver is free to compute the same expression differently per
// pipeline, and one ULP of disagreement puts the flicker straight back.
void checkVertexShaderPinsPositionInvariance() {
    const std::filesystem::path shader =
        std::filesystem::path{MC_REBEDROCK_SHADER_SRC_DIR} / "grass_block.vert";
    const std::string source = stripLineComments(readFile(shader));
    const std::regex invariant{R"(\binvariant\s+gl_Position\s*;)"};
    REQUIRE(std::regex_search(source, invariant),
            "grass_block.vert must declare `invariant gl_Position;` — the opaque and cutout "
            "pipelines share it and must agree on the depth of the grass block's side corners");
}

} // namespace

int main() {
    checkCutoutPipelineComparesLessOrEqual();
    checkVertexShaderPinsPositionInvariance();
    return 0;
}
