// Every shader that lights world geometry must go through the one shared
// lightmap, and that lightmap must be 26.1's.
//
// The formula lives in resources/shaders/src/include/lightmap.glsl, transcribed
// from vanilla's assets/minecraft/shaders/core/lightmap.fsh. Nothing headless
// can execute GLSL, so this reads the source the way
// shader_descriptor_bindings_test reads binding numbers: it checks the constants
// are vanilla's numbers, and that no shader has grown a private copy of the
// lighting again.
//
// The private-copy half is not hypothetical. The terrain lighting was hand-
// copied into grass_block.frag and block_cutout.frag; the two drifting is what
// shipped a MoltenVK pipeline-creation failure once already, and by the time
// this was written the same block had been copied a fourth and fifth time into
// item_entity.frag and particle_instanced.frag. All five had:
//
//     illumination = max(skyIllumination, blockIllumination);
//
// where vanilla ADDS the two. max() means a torch can never add to daylight, so
// four torches around an enchanting table changed nothing on screen. They were
// also missing notGamma (the brightness curve, which lifts sky level 10 from
// 0.333 to 0.568) and the 1.4 block-light factor, and they capped sky light at
// 72% for any face not pointing at the sun. The world was dark for all four
// reasons at once.

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"terrain_lightmap_test line " + std::to_string(line) + ": " +
                                 message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

const std::filesystem::path kShaderDir{MC_REBEDROCK_SHADER_SRC_DIR};

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(static_cast<bool>(input), "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

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

// Every shader that lights world geometry from the mesh's sky/block levels.
constexpr std::array kWorldShaders{"grass_block.frag", "block_cutout.frag", "item_entity.frag",
                                   "particle_instanced.frag", "rain_sheet.frag"};

void checkEveryWorldShaderUsesTheSharedLightmap() {
    for (const auto* name : kWorldShaders) {
        const std::string source = stripLineComments(readFile(kShaderDir / name));
        REQUIRE(source.find("#include \"include/lightmap.glsl\"") != std::string::npos,
                std::string{name} +
                    " must include the shared lightmap rather than carry its own copy of the "
                    "lighting");
        REQUIRE(source.find("sampleLightmap(") != std::string::npos,
                std::string{name} + " includes the lightmap but never calls sampleLightmap");
        // The private copies all declared this helper. Its presence means the
        // shader is lighting itself again.
        REQUIRE(source.find("float lightBrightness(") == std::string::npos,
                std::string{name} +
                    " declares its own lightBrightness(); the brightness curve belongs to the "
                    "shared lightmap include, and a private copy is how the five terrain "
                    "shaders drifted apart before");
        // Vanilla adds sky and block light. Combining them with max() is the
        // defect that made torches invisible in daylight.
        REQUIRE(source.find("max(skyIllumination") == std::string::npos &&
                    source.find("max(skyTint") == std::string::npos,
                std::string{name} +
                    " combines sky and block light with max(); 26.1's lightmap.fsh ADDS them, "
                    "which is what lets a torch brighten a surface the sky already lights");
    }
}

// The constants, against the numbers 26.1 actually ships.
void checkLightmapConstantsMatchVanilla() {
    const std::string source =
        stripLineComments(readFile(kShaderDir / "include" / "lightmap.glsl"));

    const auto floatConstant = [&source](const std::string& name) {
        const std::regex pattern{"const\\s+float\\s+" + name + "\\s*=\\s*([0-9.]+)\\s*;"};
        std::smatch match;
        REQUIRE(std::regex_search(source, match, pattern),
                "lightmap.glsl must declare `const float " + name + "`");
        return std::stof(match[1].str());
    };

    // LightmapRenderStateExtractor: blockFactor = blockLightFlicker + 1.4F.
    REQUIRE(std::abs(floatConstant("kBlockLightFactor") - 1.4F) < 1.0e-5F,
            "block light is scaled by 1.4 in 26.1 (LightmapRenderStateExtractor.extract)");
    // Options.gamma default 0.5 — the "options.gamma.default" label's value.
    REQUIRE(std::abs(floatConstant("kBrightnessFactor") - 0.5F) < 1.0e-5F,
            "the brightness factor drives notGamma; vanilla's gamma option defaults to 0.5, and "
            "0 would switch the whole midtone lift off and put the darkness back");

    // The formula's three moves, in vanilla's order: ambient floor, then sky
    // ADDED, then block ADDED, then clamp, then notGamma.
    REQUIRE(source.find("color += kSkyLightColor") != std::string::npos,
            "sky light must be added to the accumulator");
    REQUIRE(source.find("color += blockLightColor") != std::string::npos,
            "block light must be added to the accumulator");
    REQUIRE(source.find("lightmapNotGamma") != std::string::npos,
            "notGamma is the brightness curve; without it everything below full daylight is "
            "roughly 40% too dark");

    // BLOCK_LIGHT_TINT default -10100 = 0xFFFFD84C -> (255, 216, 76)/255.
    const std::regex tint{
        "kBlockLightTint\\s*=\\s*vec3\\(\\s*([0-9.]+)\\s*,\\s*([0-9.]+)\\s*,\\s*([0-9.]+)\\s*\\)"};
    std::smatch tintMatch;
    REQUIRE(std::regex_search(source, tintMatch, tint), "lightmap.glsl must declare kBlockLightTint");
    const std::array expectedTint{255.0F / 255.0F, 216.0F / 255.0F, 76.0F / 255.0F};
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        const float actual = std::stof(tintMatch[static_cast<int>(channel) + 1].str());
        REQUIRE(std::abs(actual - expectedTint[channel]) < 2.0e-3F,
                "kBlockLightTint must be 0xFFD84C, EnvironmentAttributes.BLOCK_LIGHT_TINT's "
                "default (-10100); channel " + std::to_string(channel) + " is " +
                    std::to_string(actual));
    }

    // CardinalLighting.DEFAULT(0.5, 1.0, 0.8, 0.8, 0.6, 0.6): down, up,
    // north/south, west/east.
    const std::string shade = source.substr(source.find("float cardinalShade"));
    REQUIRE(shade.find("0.5") != std::string::npos && shade.find("1.0") != std::string::npos,
            "cardinalShade must carry CardinalLighting.DEFAULT's down 0.5 and up 1.0");
    REQUIRE(shade.find("mix(0.6, 0.8") != std::string::npos,
            "CardinalLighting.DEFAULT is west/east 0.6 and north/south 0.8; the old hand-rolled "
            "table used 0.68 for the X faces");
}

} // namespace

int main() {
    checkEveryWorldShaderUsesTheSharedLightmap();
    checkLightmapConstantsMatchVanilla();
    return 0;
}
