#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif
#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

// A shader that samples a descriptor binding the pipeline layout does not declare
// is not a link error and not a validation warning on every driver — MoltenVK
// fails the SPIR-V to MSL conversion and vkCreateGraphicsPipelines returns
// VK_ERROR_INITIALIZATION_FAILED, which is a black window rather than a message.
//
// Nothing headless catches it: a test build never creates a pipeline. This test
// is the substitute — it reads the binding numbers out of both sides and checks
// they agree. It exists because removing the two biome-colour lookup textures
// (BM-1) left one of the four terrain shaders still sampling them, and the crash
// only appeared on a Mac.

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"shader_descriptor_bindings_test line " + std::to_string(line) +
                                 ": " + message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

const std::filesystem::path kShaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
const std::filesystem::path kRendererSource{MC_REBEDROCK_RENDERER_SRC};

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(static_cast<bool>(input), "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// Strips `//` line comments, so a binding mentioned only in prose does not read
// as a declaration.
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

// Set 0 is the shared camera-and-textures set every pipeline binds. The scene set
// (set 1, the particle storage buffer) has its own layout and is declared with an
// explicit `set = 1`, so requiring the set to be absent keeps this to set 0.
[[nodiscard]] std::set<int> shaderBindings(const std::string& source) {
    std::set<int> bindings;
    static const std::regex pattern{R"(layout\s*\(\s*binding\s*=\s*(\d+)\s*\))"};
    const auto stripped = stripLineComments(source);
    for (auto it = std::sregex_iterator{stripped.begin(), stripped.end(), pattern};
         it != std::sregex_iterator{}; ++it) {
        bindings.insert(std::stoi((*it)[1].str()));
    }
    return bindings;
}

// The renderer declares set 0 as `<name>Binding.binding = N;` on a
// VkDescriptorSetLayoutBinding, and the array it hands to
// vkCreateDescriptorSetLayout lists exactly those. Parsing the assignments is
// what keeps this test honest when the layout changes.
[[nodiscard]] std::set<int> layoutBindings(const std::string& source) {
    std::set<int> bindings;
    static const std::regex pattern{R"((\w*Binding)\.binding\s*=\s*(\d+)\s*;)"};
    const auto stripped = stripLineComments(source);
    for (auto it = std::sregex_iterator{stripped.begin(), stripped.end(), pattern};
         it != std::sregex_iterator{}; ++it) {
        bindings.insert(std::stoi((*it)[2].str()));
    }
    // binding 0 is the uniform buffer and binding 1 the block atlas; both are
    // built from a shared descriptor rather than assigned a number in the same
    // shape, so they are named here instead of parsed.
    bindings.insert(0);
    bindings.insert(1);
    return bindings;
}

[[nodiscard]] std::string join(const std::set<int>& values) {
    std::string text;
    for (const auto value : values) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::to_string(value);
    }
    return text.empty() ? "(none)" : text;
}

} // namespace

int main() {
    const auto declared = layoutBindings(readFile(kRendererSource));
    REQUIRE(!declared.empty(), "parsed no descriptor bindings out of the renderer");

    std::set<int> used;
    std::map<int, std::string> firstUser;
    std::vector<std::filesystem::path> shaders;
    for (const auto& entry : std::filesystem::directory_iterator{kShaderDir}) {
        const auto extension = entry.path().extension().string();
        if (extension == ".vert" || extension == ".frag") {
            shaders.push_back(entry.path());
        }
    }
    std::sort(shaders.begin(), shaders.end());
    REQUIRE(shaders.size() >= 10U, "found suspiciously few shaders in " + kShaderDir.string());

    for (const auto& shader : shaders) {
        for (const auto binding : shaderBindings(readFile(shader))) {
            used.insert(binding);
            firstUser.emplace(binding, shader.filename().string());
            // The one that matters: a shader may not name a binding the layout
            // does not declare. MoltenVK turns this into a pipeline that cannot
            // be created, with SPIR-V to MSL conversion error: nullptr.
            REQUIRE(declared.count(binding) == 1U,
                    shader.filename().string() + " samples descriptor binding " +
                        std::to_string(binding) +
                        ", which the pipeline layout does not declare (layout has " +
                        join(declared) + ")");
        }
    }

    // And the other direction: a binding the layout declares that no shader reads
    // is a descriptor written every frame for nothing, and usually the leftover
    // of a removed feature.
    for (const auto binding : declared) {
        REQUIRE(used.count(binding) == 1U,
                "the pipeline layout declares descriptor binding " + std::to_string(binding) +
                    " but no shader reads it (shaders use " + join(used) + ")");
    }
    return 0;
}
