// RN-22 第二条：半透明地形通道恢复背面剔除。
//
// `VulkanRenderer.cpp` 的半透明管线曾经是 `cullMode = VK_CULL_MODE_NONE`，
// 为的是修 2026-08-26 的现场缺陷「染色玻璃与实心方块的接触面被剔除，看穿」。
// 这一轮把它删了。两条证据：
//
// 一、26.1 不这么做。`RenderPipelines.TRANSLUCENT_TERRAIN`（RenderPipelines.java:235）
//    继承 `TERRAIN_SNIPPET → GENERIC_BLOCKS_SNIPPET → FOG_SNIPPET`，全链没有一处
//    `withCull(false)`，而 `RenderPipeline.java:383` 是 `cull.orElse(true)`。
// 二、双面修不了它声称修的那件事。玻璃贴着石头时，玻璃朝石头那一面**根本没进网格**
//    （石头封住了它），双面渲染变不出一片不存在的 quad；该看见的是石头自己朝向玻璃的
//    那一面，那一面在 RN-8e 给玻璃标上 `.noOcclusion()` 之后确实在画。本文件的
//    `checkGlassAgainstStone` 就是把这句话钉死。
//
// 双面真正多画出来的，是每片半透明面**背对相机的那一份**。片元着色器读的是烘死的顶点
// 法线而不是 `gl_FrontFacing`（grass_block.frag:86/101），所以那份背面拿的是它朝外
// 法线的 cardinalShade——于是内层方块的背面被画在最前、看着像正面而边框是内侧的颜色。
// 那正是这一轮要修的现场缺陷。
//
// 但删掉双面有一个真实的代价，而且它是本文件存在的主要理由：**流体的水面绕序朝上**，
// 人在水下抬头看的是它的背面。26.1 靠的是再发一片反向绕序的 quad
// （`FluidRenderer.addFace(..., addBackFace)`，FluidRenderer.java:192），不是关掉整个
// 通道的剔除。`checkWaterSurfaceIsVisibleFromBelow` 先在没有那片 quad 时红，才是这条
// 修改的证据。
//
// 判定「这片 quad 会不会被 BACK_BIT 剔掉」一律看**索引绕序**算出的几何法线，不看烘死的
// 顶点法线：水面背面那一片两者是相反的，那正是它的全部作用，用烘死的法线判会把它误判成
// 被剔除。

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"translucent_face_culling_test line " + std::to_string(line) +
                                 ": " + message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

// 一片 quad，按渲染器真正消费它的方式读出来：四个顶点由索引组给出，几何法线由绕序给出。
struct Quad final {
    glm::vec3 centre{};
    // 由索引绕序算出的法线。BACK_BIT 剔的就是它背对相机的那些。
    glm::vec3 windingNormal{};
    // 烘进顶点的法线，着色器拿它算 cardinalShade。
    glm::vec3 shadingNormal{};
};

// 把一层网格拆成 quad。索引每 6 个一组（`0,1,2,2,3,0` 或 AO 翻对角线后的
// `0,1,3,1,2,3`），组内四个不同的顶点就是这片 quad 的四角。
[[nodiscard]] std::vector<Quad> quadsOf(const mc::render::MeshData& mesh) {
    std::vector<Quad> quads;
    for (std::size_t group = 0; group + 5 < mesh.indices.size(); group += 6) {
        std::vector<std::uint32_t> distinct;
        for (std::size_t k = 0; k < 6; ++k) {
            const std::uint32_t index = mesh.indices[group + k];
            bool seen = false;
            for (const auto existing : distinct) seen = seen || existing == index;
            if (!seen) distinct.push_back(index);
        }
        if (distinct.size() != 4) continue;
        glm::vec3 centre{0.0F};
        for (const auto index : distinct) {
            centre += mc::render::decodeLocalPosition(mesh.vertices[index]);
        }
        const glm::vec3 first = mc::render::decodeLocalPosition(mesh.vertices[mesh.indices[group]]);
        const glm::vec3 second =
            mc::render::decodeLocalPosition(mesh.vertices[mesh.indices[group + 1U]]);
        const glm::vec3 third =
            mc::render::decodeLocalPosition(mesh.vertices[mesh.indices[group + 2U]]);
        quads.push_back({centre * 0.25F,
                         glm::normalize(glm::cross(second - first, third - first)),
                         mc::render::decodeNormal(mesh.vertices[mesh.indices[group]])});
    }
    return quads;
}

// 一条轴向视线上的 quad：法线沿该轴、且四边形的横截面盖住视线的横向位置。
// `axis` 取 0/1/2，视线沿该轴正向从 `origin` 出发。
[[nodiscard]] std::vector<Quad> quadsOnRay(const std::vector<Quad>& quads, int axis,
                                           const glm::vec3& origin) {
    std::vector<Quad> hits;
    for (const Quad& quad : quads) {
        if (std::abs(quad.windingNormal[axis]) < 0.9F) continue;
        bool covered = true;
        for (int other = 0; other < 3; ++other) {
            if (other == axis) continue;
            // 一格见方的面，中心与视线的横向距离必须在半格内
            covered = covered && std::abs(quad.centre[other] - origin[other]) < 0.5F;
        }
        if (!covered) continue;
        if (quad.centre[axis] < origin[axis]) continue;
        hits.push_back(quad);
    }
    return hits;
}

// BACK_BIT 之后还留在屏幕上的那些：几何法线朝向相机。
[[nodiscard]] std::size_t frontFacing(const std::vector<Quad>& quads, const glm::vec3& camera) {
    std::size_t count = 0;
    for (const Quad& quad : quads) {
        if (glm::dot(quad.windingNormal, quad.centre - camera) < 0.0F) ++count;
    }
    return count;
}

[[nodiscard]] World worldWith(std::vector<std::pair<glm::ivec3, BlockState>> cells) {
    World world;
    Chunk chunk;
    for (const auto& [position, state] : cells) {
        chunk.setState(position.x, position.y, position.z, state);
        if (state.block() == Block::Water) {
            chunk.setFluidLevel(position.x, position.y, position.z, 0U);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

constexpr int kY = mc::world::kMinY + 1;

// --- 一、看穿：透过半透明面，该看见的东西必须还在 -----------------------------
//
// 判据是「一条视线穿过 N 段连续同种半透明方块，就必须留下 N 片朝向相机的半透明面」。
// 这与 vanilla 的合并规则一致：同种相邻的内部面由 `skipsRenderingAgainstSelf` 删掉，
// 所以一段连续同色玻璃只留一片，两种不同颜色叠放留两片。

void checkHollowGlassBox() {
    // 3x3x3 空心红色染色玻璃盒，中心一格空气。相机在 -Z 外侧，视线穿过盒中心。
    std::vector<std::pair<glm::ivec3, BlockState>> cells;
    for (int dx = 0; dx < 3; ++dx) {
        for (int dy = 0; dy < 3; ++dy) {
            for (int dz = 0; dz < 3; ++dz) {
                if (dx == 1 && dy == 1 && dz == 1) continue;
                cells.push_back({{6 + dx, kY + dy, 6 + dz}, BlockState{Block::RedStainedGlass}});
            }
        }
    }
    const auto mesh = mc::world::ChunkMesher::buildSection(worldWith(std::move(cells)), {0, 0}, 0);
    const glm::vec3 camera{7.5F, 2.5F, -4.0F};
    const auto ray = quadsOnRay(quadsOf(mesh.translucentMesh), 2, camera);

    // 近墙外面、近墙内面、远墙内面、远墙外面 —— 四片都在网格里。
    REQUIRE(ray.size() == 4, "空心玻璃盒的中心视线上应有 4 片半透明面，实得 " +
                                 std::to_string(ray.size()));
    // 剔除之后剩两片：近墙的外表面与远墙的内表面。那是 vanilla 看到的两层玻璃。
    // 被剔掉的两片正是双面补丁多画的那两片，也正是「内侧背面被画在最前」的来源。
    const std::size_t visible = frontFacing(ray, camera);
    REQUIRE(visible == 2, "背面剔除后应剩 2 片玻璃（近墙外表面 + 远墙内表面），实得 " +
                              std::to_string(visible) + "。少于 2 就是看穿。");
}

void checkTwoColoursLayered() {
    // 红、蓝各一格前后叠放：颜色不同，中间那面不受同种跳过规则约束，两面都在。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY, 8}, BlockState{Block::RedStainedGlass}},
                   {{8, kY, 9}, BlockState{Block::BlueStainedGlass}}}),
        {0, 0}, 0);
    const glm::vec3 camera{8.5F, 1.5F, -4.0F};
    const std::size_t visible = frontFacing(quadsOnRay(quadsOf(mesh.translucentMesh), 2, camera),
                                            camera);
    REQUIRE(visible == 2, "红蓝染色玻璃叠放应看到 2 层，实得 " + std::to_string(visible));
}

void checkSameColourMerges() {
    // 同色两格：内部面被 `skipsRenderingAgainstSelf` 删掉，只剩一层。
    // 这一条是「不许多画」的那半边：删掉双面之后仍然只有一层，不是两层。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY, 8}, BlockState{Block::RedStainedGlass}},
                   {{8, kY, 9}, BlockState{Block::RedStainedGlass}}}),
        {0, 0}, 0);
    const glm::vec3 camera{8.5F, 1.5F, -4.0F};
    const std::size_t visible = frontFacing(quadsOnRay(quadsOf(mesh.translucentMesh), 2, camera),
                                            camera);
    REQUIRE(visible == 1, "同色染色玻璃连成一段只应看到 1 层，实得 " + std::to_string(visible));
}

void checkGlassAgainstStone() {
    // 2026-08-26 那条现场缺陷的原始形状。双面补丁当初就是为它加的。
    //
    // 玻璃朝石头那一面根本没进网格，双面渲染无从补起；真正该看见的是石头朝向玻璃的
    // 那一面，它在不透明层里。RN-8e 给玻璃标 `.noOcclusion()` 之前，石头这一面会不会
    // 被剔掉是另一回事——今天它在，所以看不穿，与半透明通道的剔除模式无关。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY, 8}, BlockState{Block::RedStainedGlass}},
                   {{8, kY, 9}, BlockState{Block::Stone}}}),
        {0, 0}, 0);
    const glm::vec3 camera{8.5F, 1.5F, -4.0F};

    const auto translucent = quadsOnRay(quadsOf(mesh.translucentMesh), 2, camera);
    REQUIRE(frontFacing(translucent, camera) == 1, "玻璃贴石头时应看到 1 层玻璃");
    // 玻璃朝石头的那一面（z = 9）被石头封住，本来就不该在网格里。
    for (const Quad& quad : translucent) {
        REQUIRE(std::abs(quad.centre.z - 9.0F) > 0.01F,
                "玻璃朝石头那一面不该进网格，石头封住了它");
    }
    // 石头朝玻璃的那一面必须在，否则才是真的看穿。
    bool stoneFacesGlass = false;
    for (const Quad& quad : quadsOf(mesh.mesh)) {
        stoneFacesGlass = stoneFacesGlass ||
                          (std::abs(quad.centre.z - 9.0F) < 0.01F && quad.windingNormal.z < -0.9F);
    }
    REQUIRE(stoneFacesGlass,
            "石头朝向玻璃的那一面不在不透明网格里 —— 那才是「透过染色玻璃看穿」的真根因，"
            "半透明通道的剔除模式与它无关");
}

// --- 二、水面：删掉双面之后，水下抬头还看得见 ---------------------------------

void checkWaterSurfaceIsVisibleFromBelow() {
    // 三格深水柱，上方空气。相机在最下面一格，抬头看。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY + 0, 8}, BlockState{Block::Water}},
                   {{8, kY + 1, 8}, BlockState{Block::Water}},
                   {{8, kY + 2, 8}, BlockState{Block::Water}}}),
        {0, 0}, 0);
    const glm::vec3 camera{8.5F, 1.5F, 8.5F};
    const auto ray = quadsOnRay(quadsOf(mesh.translucentMesh), 1, camera);

    // 水面两片：绕序朝上的正面，与反向绕序的背面。
    REQUIRE(ray.size() == 2, "水柱顶上应有 2 片水平面（正面 + 反向绕序的背面），实得 " +
                                 std::to_string(ray.size()));
    const std::size_t visible = frontFacing(ray, camera);
    REQUIRE(visible == 1,
            "水下抬头应看得见 1 片水面，实得 " + std::to_string(visible) +
                "。0 就是水面消失了——半透明通道现在做背面剔除，水面的背面必须由网格自己"
                "发一片反向绕序的 quad 补上（FluidRenderer.java:192），不能靠关掉剔除。");

    // 两片必须共面、同法线、反绕序：反向那片若绕序没反，它一样会被剔掉；
    // 若法线也跟着反了，水面的下表面会被 cardinalShade 按朝下的面压暗，而 vanilla
    // 复用的是 topColor。
    REQUIRE(std::abs(ray[0].centre.y - ray[1].centre.y) < 0.001F, "水面两片必须共面");
    REQUIRE(ray[0].shadingNormal.y > 0.9F && ray[1].shadingNormal.y > 0.9F,
            "水面两片的着色法线都必须朝上（vanilla 的背面复用 topColor）");
    REQUIRE(ray[0].windingNormal.y * ray[1].windingNormal.y < 0.0F,
            "水面两片的绕序必须相反，否则反向那片一样被剔掉");
}

void checkSubmergedWaterHasNoSurface() {
    // 顶上盖住的水没有顶面，也就不该有那片背面 —— 反向 quad 是跟着顶面走的，
    // 不是每格水都发一片。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY + 0, 8}, BlockState{Block::Water}},
                   {{8, kY + 1, 8}, BlockState{Block::Water}},
                   {{8, kY + 2, 8}, BlockState{Block::Stone}}}),
        {0, 0}, 0);
    // 相机在最下面那格水**里面**（局部 y = 1.5），所以脚下那片水底面在它身后，
    // 视线朝上扫到的只可能是水面。
    const glm::vec3 camera{8.5F, 1.5F, 8.5F};
    const auto ray = quadsOnRay(quadsOf(mesh.translucentMesh), 1, camera);
    REQUIRE(ray.empty(), "被石头盖住的水柱不该有任何水平面，实得 " + std::to_string(ray.size()));
}

// --- 三、绕序与着色法线的关系 -------------------------------------------------

void checkWindingMatchesShadingNormal() {
    // 除了水面那片刻意反绕的背面，每片 quad 的绕序法线都必须与烘死的着色法线一致。
    // 两者一旦脱钩，`cardinalShade` 就会按一个面朝反方向的法线着色 —— 那正是双面渲染
    // 下「边框是内侧背面的颜色」的机制。
    std::vector<std::pair<glm::ivec3, BlockState>> cells{
        {{8, kY, 8}, BlockState{Block::RedStainedGlass}},
        {{8, kY, 9}, BlockState{Block::Glass}},
        {{8, kY, 10}, BlockState{Block::Ice}},
        {{10, kY, 8}, BlockState{Block::Stone}},
    };
    const auto mesh = mc::world::ChunkMesher::buildSection(worldWith(std::move(cells)), {0, 0}, 0);
    for (const auto* layer : {&mesh.mesh, &mesh.cutoutMesh, &mesh.translucentMesh}) {
        for (const Quad& quad : quadsOf(*layer)) {
            REQUIRE(glm::dot(quad.windingNormal, quad.shadingNormal) > 0.9F,
                    "有一片 quad 的绕序与它烘死的着色法线不一致");
        }
    }
}

// --- 四、管线：半透明与不透明的剔除模式必须一致 -------------------------------

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(static_cast<bool>(input), "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// 去掉 `//` 行注释，免得上面那大段解释「这里曾经是 VK_CULL_MODE_NONE」的散文
// 自己把检查满足了。与 grass_overlay_depth_test 同一手法。
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

// 建管线的代码是顺序执行的一段：`rasterization.cullMode` 最后一次赋值决定驱动收到
// 什么。分别取「建不透明管线之前」与「建半透明管线之前」的最后一次赋值，两者必须相同。
// 头文件里没有这个值可读，测试构建也从不创建管线，所以照 grass_overlay_depth_test 的
// 先例从源码里读。
void checkTranslucentPipelineCullsBackFaces() {
    const std::string source =
        stripLineComments(readFile(std::filesystem::path{MC_REBEDROCK_RENDERER_SRC}));

    const auto cullModeBefore = [&source](const std::string& marker) {
        const std::regex creation{marker};
        std::smatch match;
        REQUIRE(std::regex_search(source, match, creation),
                "找不到创建 " + marker + " 的调用");
        const std::string before = source.substr(0, static_cast<std::size_t>(match.position()));
        const std::regex assignment{R"(rasterization\.cullMode\s*=\s*(VK_CULL_MODE_\w+))"};
        std::string last;
        for (auto it = std::sregex_iterator{before.begin(), before.end(), assignment};
             it != std::sregex_iterator{}; ++it) {
            last = (*it)[1].str();
        }
        REQUIRE(!last.empty(), marker + " 之前从未给 rasterization.cullMode 赋值");
        return last;
    };

    const std::string opaque = cullModeBefore(R"(&worldPipelines_\.graphicsPipeline)");
    const std::string translucent = cullModeBefore(R"(&worldPipelines_\.translucentPipeline)");
    REQUIRE(opaque == "VK_CULL_MODE_BACK_BIT",
            "不透明地形管线的 cullMode 是 " + opaque + "，本测试的前提是它做背面剔除");
    REQUIRE(translucent == opaque,
            "半透明管线的 cullMode 是 " + translucent + "，与不透明管线的 " + opaque +
                " 不一致。26.1 的 TRANSLUCENT_TERRAIN 继承 cull.orElse(true)，"
                "半透明地形**是**背面剔除的；水面从下方看得见靠的是网格自己发一片反向"
                "绕序的 quad，不是关掉整个通道的剔除。");
}

} // namespace

int main() {
    try {
        checkHollowGlassBox();
        checkTwoColoursLayered();
        checkSameColourMerges();
        checkGlassAgainstStone();
        checkWaterSurfaceIsVisibleFromBelow();
        checkSubmergedWaterHasNoSurface();
        checkWindingMatchesShadingNormal();
        checkTranslucentPipelineCullsBackFaces();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
