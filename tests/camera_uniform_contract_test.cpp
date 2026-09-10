// RN-20f-0b — 相机 UBO 的契约：C++ 那个 struct 与 GLSL 那份声明必须逐字段一致
//
// 这份声明从前手抄在 13 个着色器里，每份各自截断到自己用得到的长度。std140 下截断
// 是合法的——只要前缀逐字段一致。代价全在改动上：在中间插一个字段而漏改任何一份，
// 就是**静默的 UBO 错位**：不报错、不崩溃，只是某个 vec4 从此读到隔壁那一个。
// `block_cutout.frag` 的抬头记着这样一次事故，RN-19 §5-5 记着另一次。
//
// 冻成一份之后「13 份互相漂移」不存在了，但**新的失效模式**顶上来了：C++ 那个
// `struct CameraUniform` 与这一份 GLSL 声明之间仍然没有任何编译期联系。加一个 C++
// 字段而忘了改 GLSL（或反过来），画面上是某个数读串了，而 ctest 全绿。
//
// 所以这个测试比对的是**两边的字段序列**：名字、类型、数组长度、顺序，一项不差。
// 它不检查偏移——std140 的偏移由类型与顺序唯一决定，序列一致偏移就一致。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif
#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAIL: cannot read " << path.string() << '\n';
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 一个字段就是「类型 + 名字 + 数组长度」。std140 的偏移只由这三样与顺序决定。
struct Field final {
    std::string type;
    std::string name;
    int count = 1;   // 1 = 不是数组

    [[nodiscard]] bool operator==(const Field& other) const {
        return type == other.type && name == other.name && count == other.count;
    }
    [[nodiscard]] std::string str() const {
        return type + " " + name + (count == 1 ? "" : "[" + std::to_string(count) + "]");
    }
};

// 数组长度可以写成常量名。这里只认那两个真正用到的，写死一张小表而不是去解析 C++
// ——一个半吊子的表达式求值器会在下一个常量上骗人，而「多一个常量就要在这里加一行」
// 是一条会红的规矩，不是一条会漂的规矩。
[[nodiscard]] int resolveCount(const std::string& text) {
    if (text.empty()) {
        return 1;
    }
    if (text.find("kMaxBlockAnimations") != std::string::npos) {
        return 16;
    }
    if (text.find("kSunShadowCascadeCount") != std::string::npos) {
        return 2;
    }
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return -1;   // 解析不了 ⇒ 让比对失败，而不是悄悄当成 1
    }
}

// ---- C++ 侧：struct CameraUniform 的字段序列 --------------------------------
[[nodiscard]] std::vector<Field> parseCxx(const std::string& source) {
    const auto begin = source.find("struct CameraUniform final {");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = source.find("\n};", begin);
    const std::string body = source.substr(begin, end - begin);

    std::vector<Field> fields;
    // `alignas(16) glm::mat4 model{1.0F};`
    // `alignas(16) std::array<glm::vec4, 8> pointLights{};`
    // `alignas(16) std::array<glm::mat4, render::kSunShadowCascadeCount> lightViewProj{...};`
    const std::regex plain{R"(alignas\(16\)\s+glm::(mat4|vec4)\s+(\w+))"};
    const std::regex array{
        R"(alignas\(16\)\s+std::array<glm::(mat4|vec4),\s*([^>]+)>\s+(\w+))"};
    // regex_iterator 不接受临时的 regex（它只存引用），所以这条要有名字
    const std::regex any{
        R"(alignas\(16\)\s+(?:glm::(?:mat4|vec4)\s+\w+|std::array<glm::(?:mat4|vec4),[^>]+>\s+\w+))"};
    for (std::sregex_iterator it{body.begin(), body.end(), any}, last{}; it != last; ++it) {
        const std::string line = it->str();
        std::smatch m;
        if (std::regex_search(line, m, array)) {
            fields.push_back({m[1].str(), m[3].str(), resolveCount(m[2].str())});
        } else if (std::regex_search(line, m, plain)) {
            fields.push_back({m[1].str(), m[2].str(), 1});
        }
    }
    return fields;
}

// ---- GLSL 侧：uniform block 的字段序列 --------------------------------------
[[nodiscard]] std::vector<Field> parseGlsl(const std::string& source) {
    const auto begin = source.find("uniform CameraUniform {");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = source.find("} camera;", begin);
    const std::string body = source.substr(begin, end - begin);

    std::vector<Field> fields;
    // `mat4 model;` / `vec4 pointLights[8];`，注释行天然不匹配
    const std::regex decl{R"(^\s{4}(mat4|vec4)\s+(\w+)(?:\[(\d+)\])?\s*;)"};
    std::istringstream lines{body};
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, decl)) {
            fields.push_back({m[1].str(), m[2].str(), m[3].matched ? std::stoi(m[3].str()) : 1});
        }
    }
    return fields;
}

void testContractsMatch() {
    const std::filesystem::path root{MC_REBEDROCK_SOURCE_DIR};
    const std::filesystem::path shaders{MC_REBEDROCK_SHADER_SRC_DIR};
    const auto cxx = parseCxx(readFile(root / "src/render/vulkan/VulkanRenderer.cpp"));
    const auto glsl = parseGlsl(readFile(shaders / "include/camera_uniform.glsl"));

    check(!cxx.empty(), "解析得到 C++ 的 CameraUniform 字段");
    check(!glsl.empty(), "解析得到 GLSL 的 CameraUniform 字段");
    // 解析器自己要有下界：字段数骤降八成多半是正则没匹配上，而不是有人删了 15 个字段。
    // 少了这一条，一个失效的解析器会让整个测试变成两个空表相等。
    check(cxx.size() >= 15, "C++ 侧至少 15 个字段——少于这个数说明解析器坏了，不是契约变了");

    check(cxx.size() == glsl.size(),
          "两边字段数必须相同：C++ " + std::to_string(cxx.size()) + " vs GLSL " +
              std::to_string(glsl.size()));
    const std::size_t common = std::min(cxx.size(), glsl.size());
    for (std::size_t index = 0; index < common; ++index) {
        check(cxx[index] == glsl[index],
              "第 " + std::to_string(index) + " 个字段必须一致：C++ `" + cxx[index].str() +
                  "` vs GLSL `" + glsl[index].str() + "`");
    }
    // 数组长度解析不了会得到 -1，那种情况上面的比对可能碰巧两边都是 -1 而放过去
    for (const Field& field : cxx) {
        check(field.count > 0, "数组长度必须解析得出来：" + field.str());
    }
}

// ---- 没有人再手抄一份 -------------------------------------------------------
void testNobodyDeclaresItAgain() {
    const std::filesystem::path shaders{MC_REBEDROCK_SHADER_SRC_DIR};
    int consumers = 0;
    for (const auto& entry : std::filesystem::directory_iterator{shaders}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension().string();
        if (extension != ".vert" && extension != ".frag") {
            continue;
        }
        const std::string source = readFile(entry.path());
        const bool includes =
            source.find("#include \"include/camera_uniform.glsl\"") != std::string::npos;
        const bool declares = source.find("uniform CameraUniform {") != std::string::npos;
        check(!declares, entry.path().filename().string() +
                             " 不得自己再声明一遍 CameraUniform——那正是 13 份手抄的来源");
        if (includes) {
            ++consumers;
        }
    }
    // 今天恰好 13 个消费者。这个数字写在这里是为了让「少了一个」也变红：
    // 有人把 include 删掉换回手抄时，上面那条 `!declares` 会红；而有人整个删掉了
    // 对 camera 的使用（比如把某个着色器改成不吃相机），这一条会红——两种都该被看见。
    check(consumers == 13,
          "恰好 13 个着色器 include 这份契约，实测 " + std::to_string(consumers));
}

// ---- CMake 的 DEPENDS 不得落后于 include/ ----------------------------------
//
// `glslc` 不会告诉 CMake 它 include 了什么，所以那张表是手工维护的。漏一行的后果是
// **改了那个头文件什么都不重编译**——而本轮之前 `shadow_map.glsl` 与
// `vertex_normals.glsl` 就是漏着的。那正是「着色器没重编译却出了一张漂亮的图」
// 这类事故的温床（RN-55 那轮踩过一次，退出码 0、A/B 两组读数逐字节相同）。
void testShaderIncludeDependenciesAreComplete() {
    const std::filesystem::path root{MC_REBEDROCK_SOURCE_DIR};
    const std::string cmake = readFile(root / "CMakeLists.txt");
    const auto begin = cmake.find("set(MC_REBEDROCK_SHADER_INCLUDES");
    check(begin != std::string::npos, "找得到 MC_REBEDROCK_SHADER_INCLUDES");
    if (begin == std::string::npos) {
        return;
    }
    const std::string list = cmake.substr(begin, cmake.find(')', begin) - begin);

    int headers = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator{std::filesystem::path{MC_REBEDROCK_SHADER_SRC_DIR} /
                                             "include"}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".glsl") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        check(list.find("include/" + name) != std::string::npos,
              name + " 必须列进 MC_REBEDROCK_SHADER_INCLUDES，否则改它不会重编译任何着色器");
        ++headers;
    }
    check(headers >= 5, "include/ 下至少 5 个头文件——少于这个数说明目录读错了");
}

} // namespace

int main() {
    testContractsMatch();
    testNobodyDeclaresItAgain();
    testShaderIncludeDependenciesAreComplete();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "camera_uniform_contract_test ok\n";
    return 0;
}
