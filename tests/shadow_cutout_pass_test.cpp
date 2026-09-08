// 太阳阴影的投射者是「不透明 **加** 镂空」两层，不是只有不透明那一层。
//
// 现场（用户实机）：「楼梯方块无太阳阴影」。
//
// 根因不在阴影算法里，在它挑投射者的那一句。`recordShadow` 只画 `mesh.opaque`，
// 而本作的 Cutout 桶装的远不止草和树叶——楼梯、墙、栅栏、门、活板门、压力板全在
// 里面（它们的材质其实没有一个透明像素，那是渲染分桶的另一笔账）。于是一段楼梯在
// 太阳底下不投任何影子，紧挨着它的台阶（Opaque 桶）却投，两者并排时格外扎眼。
//
// 26.1 把这两层归成同一组：`ChunkSectionLayerGroup.OPAQUE(SOLID, CUTOUT)`
// （ChunkSectionLayerGroup.java:8）。挡光与否跟「要不要 alpha 测试」是两回事。
//
// 这条测试读源码，因为它钉的东西 headless 跑不到：阴影通道要一台设备、一个
// swapchain 和一张离屏深度图。出图能看见结果（已验），但出图不进 ctest。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif
#ifndef MC_REBEDROCK_WORLD_RENDERER_SRC
#error "MC_REBEDROCK_WORLD_RENDERER_SRC must point at src/render/vulkan/WorldRenderer.hpp"
#endif
#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAILED: cannot read " << path.string() << "\n";
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 注释里提到一个名字不算调用，也不算声明。这两个文件的注释都很长。
[[nodiscard]] std::string stripComments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) ++i;
            i = i + 2 < source.size() ? i + 2 : source.size();
        } else {
            out.push_back(source[i]);
            ++i;
        }
    }
    return out;
}

[[nodiscard]] std::set<int> locations(const std::string& source, const std::string& direction) {
    std::set<int> found;
    const std::regex pattern{R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*(?:flat\s+)?)" +
                             direction + R"(\s)"};
    const auto clean = stripComments(source);
    for (auto it = std::sregex_iterator{clean.begin(), clean.end(), pattern};
         it != std::sregex_iterator{}; ++it) {
        found.insert(std::stoi((*it)[1].str()));
    }
    return found;
}

// `marker` 之前最后一次给 `field` 赋的那个数。
[[nodiscard]] int lastAssignmentBefore(const std::string& source, std::string_view field,
                                       std::string_view marker) {
    const auto end = source.find(marker);
    if (end == std::string::npos) {
        std::cerr << "FAILED: 源码里找不到 `" << marker << "`\n";
        ++failures;
        return -1;
    }
    const auto needle = std::string{field} + " = ";
    const auto at = source.rfind(needle, end);
    if (at == std::string::npos) {
        std::cerr << "FAILED: `" << marker << "` 之前没有给 " << field << " 赋过值\n";
        ++failures;
        return -1;
    }
    return std::stoi(source.substr(at + needle.size(), 4));
}

[[nodiscard]] float alphaThreshold(const std::string& source, std::string_view what) {
    static const std::regex pattern{R"(\.a\s*<\s*([0-9.]+))"};
    const auto clean = stripComments(source);
    std::smatch match;
    if (!std::regex_search(clean, match, pattern)) {
        std::cerr << "FAILED: " << what << " 里找不到 alpha 阈值\n";
        ++failures;
        return -1.0F;
    }
    return std::stof(match[1].str());
}

} // namespace

int main() {
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    const std::string worldRenderer = stripComments(readFile(MC_REBEDROCK_WORLD_RENDERER_SRC));
    const std::string renderer = stripComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    if (worldRenderer.empty() || renderer.empty()) {
        return 1;
    }

    // ---- ① 两层都被画进阴影图 ----------------------------------------
    require(worldRenderer.find("&GpuMesh::opaque") != std::string::npos,
            "阴影通道必须画不透明地形");
    require(worldRenderer.find("&GpuMesh::cutout") != std::string::npos,
            "阴影通道必须**也**画镂空地形——只画 opaque 时楼梯/栅栏/门/树叶不投影");

    // ---- ② 只有镂空几何的 section 不能在挑候选那一步就被扔掉 ----------
    //
    // 这一句单独钉：①的两次绘制即使都在，候选过滤若仍只看 opaque，一个整段
    // 只有楼梯的 section 连进都进不来，第二次绘制对它是空转。
    require(worldRenderer.find("mesh.opaque.indexCount == 0U && mesh.cutout.indexCount == 0U") !=
                std::string::npos,
            "候选过滤必须两层都空才跳过（曾经只看 mesh.opaque）");

    // ---- ③ 镂空那条管线的布局必须带描述符集 ---------------------------
    //
    // 它要采样 binding 1 的方块图集。布局不声明 set 而着色器采样它，在 MoltenVK 上
    // 不是校验层警告而是 SPIR-V→MSL 转换失败：vkCreateGraphicsPipelines 直接返回
    // VK_ERROR_INITIALIZATION_FAILED，表现为一扇黑窗户。
    require(lastAssignmentBefore(renderer, "layoutInfo.setLayoutCount",
                                 "&worldPipelines_.shadowCutoutPipelineLayout") == 1,
            "镂空阴影管线的布局必须声明描述符集（它采样 binding 1 的方块图集）");
    // 而不透明那条不能跟着一起付：它一个描述符都不读。这条同时钉住「两条布局
    // 确实是两条」——共用一条的话上面那句会把不透明管线也拖进来。
    require(lastAssignmentBefore(renderer, "layoutInfo.setLayoutCount",
                                 "&worldPipelines_.shadowPipelineLayout") == 0,
            "不透明阴影管线的布局不该声明描述符集——它不采样任何东西");

    // ---- ④ 顶点着色器写出去的每一个 location，两条片元都要接住 --------
    //
    // 两条管线共用 shadow.vert（位置算术只有一份，两份漂移影子就会与投影它的方块
    // 错开）。深度那条不需要图集坐标，但**必须声明**它们：否则校验层对每一条不透明
    // 地形管线报一次 OutputNotConsumed。
    const auto vertexOutputs = locations(readFile(shaderDir / "shadow.vert"), "out");
    require(!vertexOutputs.empty(), "shadow.vert 必须有输出，否则这条断言是空的");
    for (const auto* frag : {"shadow.frag", "shadow_cutout.frag"}) {
        const auto inputs = locations(readFile(shaderDir / frag), "in");
        std::set<int> missing;
        std::set_difference(vertexOutputs.begin(), vertexOutputs.end(), inputs.begin(),
                            inputs.end(), std::inserter(missing, missing.end()));
        for (const int location : missing) {
            std::cerr << "FAILED: " << frag << " 没有声明 shadow.vert 写出的 location "
                      << location << "\n";
            ++failures;
        }
    }

    // ---- ⑤ 影子的轮廓与看得见的轮廓是同一条线 -------------------------
    const float shadowAlpha = alphaThreshold(readFile(shaderDir / "shadow_cutout.frag"),
                                             "shadow_cutout.frag");
    const float visibleAlpha = alphaThreshold(readFile(shaderDir / "block_cutout.frag"),
                                              "block_cutout.frag");
    require(shadowAlpha == visibleAlpha,
            "镂空阴影的 alpha 阈值必须与主通道相同——不同就会在树叶边缘留下一圈"
            "投了影却看不见的像素");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "shadow_cutout_pass ok (vert 输出 " << vertexOutputs.size() << " 个 location，"
              << "alpha 阈值 " << shadowAlpha << ")\n";
    return 0;
}
