// RN-11 的太阳阴影图：深度约定、texel snapping、投射者选择，加上着色器与渲染器
// 两侧的源码护栏。
//
// 阴影的观感 headless 验不了——测试构建从不创建管线，也没有 GPU。但这一轮的四条
// 改动里有三条是**纯几何**：矩阵落在哪个深度区间、纹素网格钉不钉得住、排序键含不含
// 视点。它们是可证的，而且都是先红后绿的。剩下那条（PCF 与偏置的观感）只能靠源码
// 护栏钉住形状，数值留给 mac。

#include "render/MeshData.hpp"
#include "render/SunShadowMap.hpp"
#include "render/EntityRenderDraws.hpp"
#include "world/DayNightCycle.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#ifndef MC_REBEDROCK_WORLD_RENDERER_SRC
#error "MC_REBEDROCK_WORLD_RENDERER_SRC must point at src/render/vulkan/WorldRenderer.hpp"
#endif

namespace shaderBias {
using std::min;
using std::max;
using std::clamp;
using std::sqrt;
using std::abs;
#include "../resources/shaders/src/include/sun_shadow_bias.glsl"
}

namespace shaderReceiver {
using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::mat4;
using glm::dot;
using glm::normalize;
using glm::mix;
using std::min;
using std::max;
using std::clamp;
using std::sqrt;
using std::abs;

struct Samples {
    std::array<float, 9> visibility{};
    std::array<vec3, 9> coordinates{};
    std::size_t count = 0;
};
using sampler2DShadow = Samples*;
float texture(sampler2DShadow sampler, vec3 coordinates) {
    const auto index = sampler->count++;
    sampler->coordinates.at(index) = coordinates;
    return sampler->visibility.at(index);
}

// Generated from the production GLSL, with only swizzles/float literal syntax
// adapted to C++. The real early return, projection, bias and PCF execute here.
#include "sun_shadow_glsl_cpp.hpp"
}

namespace {

using mc::render::Aabb;

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"sun_shadow_map_test line " + std::to_string(line) + ": " +
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

// 去掉 `//` 行注释，解释某条不变量的散文因此不会自己满足对它的检查
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

// 取一个函数体：从 `<签名>` 起到缩进回到同级的那个 `}`。护栏要断言的是「这个函数里
// 有/没有某个东西」，在整份文件上 grep 会被别处的同名调用满足
[[nodiscard]] std::string functionBody(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    REQUIRE(start != std::string::npos, "cannot find " + signature);
    const auto open = source.find('{', start);
    REQUIRE(open != std::string::npos, "no body for " + signature);
    int depth = 0;
    for (std::size_t index = open; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (--depth == 0) {
                return source.substr(open, index - open + 1);
            }
        }
    }
    REQUIRE(false, "unbalanced body for " + signature);
    return {};
}

[[nodiscard]] glm::vec3 projectToNdc(const glm::mat4& lightViewProj, const glm::vec3& world) {
    const glm::vec4 clip = lightViewProj * glm::vec4{world, 1.0F};
    return glm::vec3{clip} / clip.w;
}

// 正午附近的太阳，DayNightCycle 的 orbit = 0
const glm::vec3 kNoonSun = glm::normalize(glm::vec3{0.0F, 1.0F, 0.28F});

void checkShadowFacing() {
    const std::array suns{glm::vec3{0, 1, 0}, glm::vec3{3, 0, 0},
                          glm::vec3{0, 0, -5}, glm::vec3{1, 2, 3}};
    const std::array normals{
        glm::normalize(glm::vec3{1, 0.25F, 0}),
        glm::normalize(glm::vec3{1, -0.25F, 0}),
        glm::vec3{1, 0, 0}, glm::vec3{-1, 0, 0}, glm::vec3{0, 1, 0},
        glm::vec3{0, -1, 0}, glm::vec3{0, 0, 1}, glm::vec3{0, 0, -1},
        glm::normalize(glm::vec3{1, -0.00001F, 0}),
        glm::normalize(glm::vec3{1, 0.00001F, 0})};
    const std::array patterns{
        std::array<float, 9>{0, 0, 0, 0, 0, 0, 0, 0, 0},
        std::array<float, 9>{1, 1, 1, 1, 1, 1, 1, 1, 1},
        std::array<float, 9>{0, 0.25F, 0.5F, 0.75F, 1, 0.75F, 0.5F, 0.25F, 0}};
    for (const auto sun : suns) {
        for (const auto normal : normals) {
            const float incidence = glm::dot(normal, glm::normalize(sun));
            for (const auto& pattern : patterns) {
                shaderReceiver::Samples samples{pattern, {}, 0};
                const float factor = shaderReceiver::sunShadowFactor(
                    &samples, glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F}, normal, sun);
                const std::string context = " at N.L=" + std::to_string(incidence);
                if (incidence <= 0.0F) {
                    REQUIRE(factor == 1.0F && samples.count == 0,
                            "back-facing/grazing receiver must return 1 with zero PCF taps" +
                                context + "; factor=" + std::to_string(factor) +
                                ", taps=" + std::to_string(samples.count));
                } else {
                    float lit = 0.0F;
                    for (const float visibility : pattern) lit += visibility;
                    const float expected = glm::mix(0.35F, 1.0F, lit / 9.0F);
                    REQUIRE(samples.count == 9 && std::abs(factor - expected) < 0.000001F,
                            "sun-facing receiver must preserve nine-tap PCF visibility" +
                                context + "; factor=" + std::to_string(factor) +
                                ", taps=" + std::to_string(samples.count));
                    // Positive incidence must still reach the original projected coordinates,
                    // ZO reference, slope bias and per-tap receiver-plane correction.
                    for (std::size_t tap = 0; tap < 9; ++tap) {
                        const float x = static_cast<float>(tap % 3) - 1.0F;
                        const float y = static_cast<float>(tap / 3) - 1.0F;
                        const auto expectedUv = glm::vec2{0.5F} + glm::vec2{x, y} / 2048.0F;
                        const float expectedDepth = 0.5F +
                            (shaderBias::sunShadowTapOffsetBlocks(normal.x, normal.y, incidence, x, y) -
                             shaderBias::sunShadowBiasBlocks(incidence)) / 319.9F;
                        const auto coordinates = samples.coordinates[tap];
                        REQUIRE(glm::length(glm::vec2{coordinates} - expectedUv) < 0.000001F &&
                                std::abs(coordinates.z - expectedDepth) < 0.000001F,
                                "sun-facing receiver must preserve projected PCF coordinates/depth" + context);
                    }
                }
            }
        }
    }
    // The existing outside-frustum early return remains active for front faces.
    shaderReceiver::Samples outside;
    REQUIRE(shaderReceiver::sunShadowFactor(&outside, glm::mat4{1.0F}, glm::vec3{3, 0, 0.5F},
                glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}) == 1.0F && outside.count == 0,
            "sun-facing receiver outside the shadow map must remain fully lit without PCF");
}

// ---------------------------------------------------------------------------
// 1. 深度约定：光锥中心的 z_ndc 必须落在 Vulkan 的裁剪区间 [0, 1] 里
//
// 这是本轮的第一条断言。改动前 glm::ortho 派发到 orthoRH_NO（全仓没有定义
// GLM_FORCE_DEPTH_ZERO_TO_ONE），深度落在 [-1,1]；Vulkan 裁的是 0 <= z_clip <= w，
// depthClampEnable 又是 VK_FALSE。于是 z_ndc < 0 的几何被硬件裁掉，也就是光源空间
// 深度小于 (near+far)/2 = 160.05 格的一切——视点脚下的地面（d ≈ 96）从来没有写进过
// 阴影图，只有视点下方 64 格开外的地形才进得去。这条断言改动前得到 -0.4004。
// ---------------------------------------------------------------------------
void checkDepthConvention() {
    const glm::vec3 eye{12.5F, 70.0F, -8.25F};
    const glm::mat4 lightViewProj = mc::render::sunShadowLightViewProj(kNoonSun, eye);

    const float centerDepth = projectToNdc(lightViewProj, eye).z;
    REQUIRE(centerDepth >= 0.0F && centerDepth <= 1.0F,
            "the light frustum centre projects to z_ndc " + std::to_string(centerDepth) +
                ", outside Vulkan's [0, 1] clip range: the ortho matrix is using the OpenGL "
                "depth convention and the hardware clips everything nearer than 160 blocks");

    // 视点脚下到远低于脚下的一整段落差都必须进得去。128 格正交框的横截面覆盖 ±64 格，
    // 深度方向的可用区间是 0.1..320 格，视点本身在 96 格处，所以脚下 ±60 格都该通过
    for (const float drop : {0.0F, 5.0F, 32.0F, 60.0F}) {
        const glm::vec3 point = eye - glm::vec3{0.0F, drop, 0.0F};
        const glm::vec3 ndc = projectToNdc(lightViewProj, point);
        REQUIRE(ndc.z >= 0.0F && ndc.z <= 1.0F,
                "terrain " + std::to_string(drop) + " blocks below the eye projects to z_ndc " +
                    std::to_string(ndc.z) + ", which the rasteriser clips away");
        REQUIRE(std::abs(ndc.x) <= 1.0F && std::abs(ndc.y) <= 1.0F,
                "terrain " + std::to_string(drop) + " blocks below the eye falls outside the "
                "ortho box laterally, which it should not");
    }

    // 深度必须随离光源变远而单调增大，而且量纲对得上：1.0 个 NDC 单位 = far - near 格
    const float shallow = projectToNdc(lightViewProj, eye).z;
    const float deep = projectToNdc(lightViewProj, eye - glm::vec3{0.0F, 60.0F, 0.0F}).z;
    REQUIRE(deep > shallow, "light-space depth must increase away from the sun");
    // 落差 60 格投影到光轴上是 60 * dot(sun, up) 格（不是除——光轴与竖直方向的夹角
    // 让同样的落差在光方向上走得**更短**）
    const float expected = (60.0F * glm::dot(kNoonSun, glm::vec3{0.0F, 1.0F, 0.0F})) /
                           mc::render::kSunShadowDepthRangeBlocks;
    REQUIRE(std::abs((deep - shallow) - expected) < 1e-3F,
            "1.0 of NDC depth must equal far - near blocks; got " +
                std::to_string(deep - shallow) + " for an expected " + std::to_string(expected));
}

// ---------------------------------------------------------------------------
// 2. texel snapping：固定的世界点在阴影图里的纹素坐标，随视点平移只能整纹素跳变
//
// 不做量化时，光源正交框逐帧跟着视点这个连续浮点量平移，每一帧的纹素落在不同的世界
// 位置上，被量化的阴影边界因此逐帧改变采样相位——玩家平移时阴影边沿地面爬行。
// ---------------------------------------------------------------------------
void checkTexelSnapping() {
    const glm::vec3 probe{3.5F, 64.0F, -11.25F};
    const glm::vec3 base{0.0F, 70.0F, 0.0F};
    const float resolution = static_cast<float>(mc::render::kSunShadowMapResolution);

    const glm::vec3 reference = projectToNdc(mc::render::sunShadowLightViewProj(kNoonSun, base), probe);
    const glm::vec2 referenceTexel = (glm::vec2{reference} * 0.5F + 0.5F) * resolution;

    // 步长刻意取纹素尺寸（0.0625 格）的无理数倍，量化前后不会碰巧对齐
    constexpr float kStep = 0.0179856F;
    bool sawMotion = false;
    for (int index = 1; index <= 200; ++index) {
        const glm::vec3 eye = base + glm::vec3{kStep * static_cast<float>(index),
                                               kStep * 0.5F * static_cast<float>(index),
                                               -kStep * 0.75F * static_cast<float>(index)};
        const glm::vec3 ndc = projectToNdc(mc::render::sunShadowLightViewProj(kNoonSun, eye), probe);
        const glm::vec2 texel = (glm::vec2{ndc} * 0.5F + 0.5F) * resolution;
        const glm::vec2 delta = texel - referenceTexel;
        for (const float component : {delta.x, delta.y}) {
            const float residual = std::abs(component - std::round(component));
            REQUIRE(residual < 1e-2F,
                    "step " + std::to_string(index) + ": a fixed world point moved " +
                        std::to_string(component) +
                        " texels in the shadow map, which is not a whole number of texels — the "
                        "light matrix is not snapped to the texel grid and shadow edges will "
                        "crawl as the camera moves");
        }
        if (std::abs(delta.x) > 0.5F || std::abs(delta.y) > 0.5F) {
            sawMotion = true;
        }
    }
    // 反面：如果框根本没跟着视点动，上面那条断言会白白通过
    REQUIRE(sawMotion, "the light frustum never moved across 200 camera steps, so the snapping "
                       "assertion above proved nothing");
}

// ---------------------------------------------------------------------------
// 3. 投射者选择：排序键不含视点，因此相机平移不改变入选集合
//
// 从前的键是 section 中心到相机的距离平方，相机一动整张表重排，512 的截断线在表上
// 滑动，整块 16x16 的 section 成批进出——地面上因此出现以区块为粒度、随移动扫过的
// 亮斑（用户实机报的「类似移动体积云的阴影」）。
// ---------------------------------------------------------------------------
// 候选必须**整体落在两个光锥之内**，这条断言才只在量排序。否则框边的进出会混进来，
// 而那是 128 格正交框的账（C(c) CSM，本轮不做），不是排序键的账。
// 位置用黄金比例序列铺开：确定性、无 RNG、且不会像整数网格那样与纹素量化共振。
[[nodiscard]] std::vector<Aabb> buildCandidateSections(const glm::vec3& around) {
    // 形状照着真实地形：横向铺开、竖向薄。这一点对断言的力度是决定性的——球状聚在
    // 视点周围的夹具里，「离相机距离」的排名约等于半径，相机一动排名几乎不变，缺陷
    // 会从断言底下溜过去（实测只有 4/512 个位置变化）。换成板状之后同样的破坏动 24 个。
    // 数量取 2000 而不是刚过 512：截断线因此落在分布的稠密处，而不是稀疏的尾巴上，
    // 这也更接近文档量级核对里「视点飞高时候选涨到数千」的那个情形。
    std::vector<Aabb> bounds;
    constexpr std::size_t kCount = 2000;
    constexpr float kSection = 16.0F;
    for (std::size_t index = 0; index < kCount; ++index) {
        const auto fraction = [index](float step) {
            const float value = static_cast<float>(index) * step;
            return value - std::floor(value);
        };
        const glm::vec3 minimum{
            around.x - 40.0F + fraction(0.6180339887F) * 80.0F,
            around.y - 56.0F + fraction(0.7548776662F) * 56.0F,
            around.z - 12.0F + fraction(0.5698402910F) * 24.0F,
        };
        bounds.push_back(Aabb{minimum, minimum + glm::vec3{kSection}});
    }
    return bounds;
}

// 八个角全部落在 NDC 立方体内 = 这个盒完整地在光锥里
[[nodiscard]] bool fullyInsideFrustum(const glm::mat4& lightViewProj, const Aabb& box) {
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{
            (corner & 1) != 0 ? box.maximum.x : box.minimum.x,
            (corner & 2) != 0 ? box.maximum.y : box.minimum.y,
            (corner & 4) != 0 ? box.maximum.z : box.minimum.z,
        };
        const glm::vec3 ndc = projectToNdc(lightViewProj, point);
        if (std::abs(ndc.x) > 1.0F || std::abs(ndc.y) > 1.0F || ndc.z < 0.0F || ndc.z > 1.0F) {
            return false;
        }
    }
    return true;
}

void checkCasterSelection() {
    const glm::vec3 eye{0.0F, 70.0F, 0.0F};
    const std::vector<Aabb> bounds = buildCandidateSections(eye);
    REQUIRE(bounds.size() > mc::render::kMaxSunShadowCasters,
            "the fixture must exceed the caster cap or it tests nothing");

    const glm::mat4 baseMatrix = mc::render::sunShadowLightViewProj(kNoonSun, eye);
    for (std::size_t index = 0; index < bounds.size(); ++index) {
        REQUIRE(fullyInsideFrustum(baseMatrix, bounds[index]),
                "fixture candidate " + std::to_string(index) + " is not inside the light frustum");
    }
    std::vector<std::size_t> selected;
    mc::render::selectSunShadowCasters(baseMatrix, kNoonSun, bounds, selected);
    REQUIRE(selected.size() == mc::render::kMaxSunShadowCasters,
            "expected the cap to bind, got " + std::to_string(selected.size()) + " casters");

    // 沿光方向最靠前的那批必须入选：把候选按光源空间深度排一遍，最靠前的 32 个
    // 一个都不能落选
    std::vector<std::size_t> byDepth(bounds.size());
    for (std::size_t index = 0; index < bounds.size(); ++index) {
        byDepth[index] = index;
    }
    std::sort(byDepth.begin(), byDepth.end(), [&](std::size_t first, std::size_t second) {
        return mc::render::sunShadowCasterDepth(kNoonSun, bounds[first]) <
               mc::render::sunShadowCasterDepth(kNoonSun, bounds[second]);
    });
    const std::set<std::size_t> chosen{selected.begin(), selected.end()};
    for (std::size_t rank = 0; rank < 32; ++rank) {
        REQUIRE(chosen.count(byDepth[rank]) == 1U,
                "the section ranked " + std::to_string(rank) +
                    " closest to the sun was dropped from the caster set");
    }

    // 本体：相机小幅平移不得改变入选集合
    for (const glm::vec3 nudge : {glm::vec3{0.37F, 0.0F, 0.0F}, glm::vec3{0.0F, 0.21F, -0.44F},
                                  glm::vec3{-0.9F, 0.5F, 0.9F}}) {
        const glm::mat4 movedMatrix = mc::render::sunShadowLightViewProj(kNoonSun, eye + nudge);
        // 前置条件：平移后的光锥仍然完整包住每一个候选。不成立就说明这个夹具在量的是
        // 框边进出而不是排序，断言会变成一条假阳性
        for (std::size_t index = 0; index < bounds.size(); ++index) {
            REQUIRE(fullyInsideFrustum(movedMatrix, bounds[index]),
                    "fixture candidate " + std::to_string(index) +
                        " left the light frustum when the camera moved; the fixture must keep "
                        "every candidate interior so this only measures the ordering");
        }
        std::vector<std::size_t> moved;
        mc::render::selectSunShadowCasters(movedMatrix, kNoonSun, bounds, moved);
        const std::set<std::size_t> movedSet{moved.begin(), moved.end()};
        REQUIRE(movedSet == chosen,
                "moving the camera by less than a block changed the caster set (" +
                    std::to_string(chosen.size()) + " vs " + std::to_string(movedSet.size()) +
                    " entries, sets differ) — the sort key still depends on the camera, so whole "
                    "sections pop in and out and their shadows sweep across the ground");
    }

    // 排序键本身：包围盒沿光方向的近点，越靠近光源越小，且与视点无关
    const Aabb near{{0.0F, 100.0F, 0.0F}, {16.0F, 116.0F, 16.0F}};
    const Aabb far{{0.0F, 20.0F, 0.0F}, {16.0F, 36.0F, 16.0F}};
    REQUIRE(mc::render::sunShadowCasterDepth(kNoonSun, near) <
                mc::render::sunShadowCasterDepth(kNoonSun, far),
            "a section nearer the sun must sort before one further from it");
}

// ---------------------------------------------------------------------------
// 4. 光源的 up 永不退化：太阳整天严格落在轨道平面里
//
// RN-24 把 up 从世界的 (0,1,0) 换成轨道平面法线之后，这条从「太阳不过天顶」变成更强的
// 「太阳恒与 up 正交」：|cross(-sun, up)| 从最小 0.2696 变成恒等 1。谁把轨道改成非平面
// （比如给 z 分量换个与 y 不同相位的式子），这里先炸，而不是在 mac 上炸成一屏 NaN，
// 也不是悄悄把 RN-24 的零自旋还回去。
// ---------------------------------------------------------------------------
void checkSunNeverVertical() {
    const glm::vec3 up = glm::normalize(mc::world::DayNightCycle::kSunOrbitNormal);
    float worstAlignment = 0.0F;
    float weakestCross = 1.0F;
    for (int tick = 0; tick < 24'000; ++tick) {
        const glm::vec3 sun =
            glm::normalize(mc::world::DayNightCycle::stateAtTick(static_cast<double>(tick)).sunDirection);
        worstAlignment = std::max(worstAlignment, std::abs(glm::dot(sun, up)));
        weakestCross = std::min(weakestCross, glm::length(glm::cross(-sun, up)));
        const glm::mat4 matrix = mc::render::sunShadowLightViewProj(sun, glm::vec3{0.0F, 70.0F, 0.0F});
        REQUIRE(std::isfinite(matrix[0][0]) && std::isfinite(matrix[3][2]),
                "the light matrix went non-finite at tick " + std::to_string(tick));
    }
    REQUIRE(worstAlignment < 1e-5F,
            "the sun leaves the orbit plane its normal was derived from (worst |dot(sun, up)| = " +
                std::to_string(worstAlignment) +
                "): kSunOrbitNormal no longer matches the direction formula in DayNightCycle.cpp, "
                "so the light basis is rolling again and sunShadowLightViewProj is one step from "
                "a degenerate lookAt");
    REQUIRE(weakestCross > 1.0F - 1e-5F,
            "|cross(-sun, up)| fell to " + std::to_string(weakestCross) +
                " during the day; lookAt normalises that cross product, so anything below 1 is "
                "amplified error in the light frame's right axis");
}

// ---------------------------------------------------------------------------
// 5. 光源基零自旋：整天里光源空间的 y 轴恒等于轨道法线（RN-24）
//
// 自旋（绕光轴的转动）是唯一会在阴影图**平面内**转动纹素网格的分量，物理上不改变任何
// 一片阴影，却让固定世界点的采样相位逐 tick 重排。用世界 (0,1,0) 当 up 时它在正午达到
// 太阳自身转速的 3.71 倍；用轨道法线当 up 时它恒为 0，因为 u = cross(s, -sun) 在
// sun ⊥ up 时恒等于 up 自己。
//
// 从矩阵里把基取回来：lightProj 的前两行只是 1/halfExtent 的缩放，所以
// s = halfExtent * (M[0][0], M[1][0], M[2][0])，u 同理取第二行。
// ---------------------------------------------------------------------------
[[nodiscard]] glm::vec3 lightBasisRow(const glm::mat4& matrix, int row) {
    return mc::render::kSunShadowOrthoHalfExtent *
           glm::vec3{matrix[0][row], matrix[1][row], matrix[2][row]};
}

void checkLightBasisHasNoRoll() {
    const glm::vec3 up = glm::normalize(mc::world::DayNightCycle::kSunOrbitNormal);
    const glm::vec3 eye{12.5F, 70.0F, -8.25F};
    float worstUpDrift = 0.0F;
    glm::vec3 firstRight{0.0F};
    float widestRightSwing = 0.0F;
    for (int tick = 0; tick < 24'000; tick += 7) {
        const glm::vec3 sun =
            glm::normalize(mc::world::DayNightCycle::stateAtTick(static_cast<double>(tick)).sunDirection);
        const glm::mat4 matrix = mc::render::sunShadowLightViewProj(sun, eye);
        const glm::vec3 right = glm::normalize(lightBasisRow(matrix, 0));
        const glm::vec3 basisUp = glm::normalize(lightBasisRow(matrix, 1));
        worstUpDrift = std::max(worstUpDrift, glm::length(basisUp - up));
        if (tick == 0) {
            firstRight = right;
        }
        widestRightSwing = std::max(widestRightSwing, glm::length(right - firstRight));
    }
    REQUIRE(worstUpDrift < 1e-4F,
            "the light basis' up axis drifted " + std::to_string(worstUpDrift) +
                " from the sun's orbit normal during the day: the basis is spinning about the "
                "light axis, which rotates the shadow map's texel grid inside its own plane and "
                "re-phases every sample without moving a single shadow");
    // 反面：如果整个基根本不动，上面那条会白白通过。right 轴必须跟着太阳转满一整圈。
    REQUIRE(widestRightSwing > 1.9F,
            "the light basis' right axis never left its starting direction (widest swing " +
                std::to_string(widestRightSwing) +
                "), so the no-roll assertion above proved nothing");
}

// ---------------------------------------------------------------------------
// 6. 太阳角度量化：一个角度步之内，光源矩阵逐位不变（RN-24）
//
// 这是本轮的主断言。太阳一转，snap() 清不掉的那个分数相位就重新随机化，阴影边的锯齿
// 因此逐 tick 重排；把角度量化到 kSunShadowAngleStepTicks 之后，步内 R 逐位不变，静止
// 视点下整个矩阵逐位相同——和「时间暂停」是同一份画面。
// ---------------------------------------------------------------------------
[[nodiscard]] glm::vec3 sunAtShadowTick(double dayTimeTicks) {
    return glm::normalize(mc::world::DayNightCycle::stateAtTick(
        mc::render::sunShadowSunTick(dayTimeTicks)).sunDirection);
}

void checkSunAngleQuantization() {
    const double step = mc::render::kSunShadowAngleStepTicks;
    REQUIRE(step >= 2.0, "an angle step of one tick quantises nothing");

    // 6a. 步函数本身：步内恒定，跨步恰好跳一个步长，且边界落在步长的整数倍上
    for (double base : {0.0, 8.0, 5'992.0, 23'992.0, 120'000.0}) {
        const double expected = std::floor(base / step) * step;
        for (double offset = 0.0; offset < step; offset += 1.0) {
            REQUIRE(mc::render::sunShadowSunTick(base + offset) == expected,
                    "sunShadowSunTick(" + std::to_string(base + offset) + ") = " +
                        std::to_string(mc::render::sunShadowSunTick(base + offset)) +
                        ", expected " + std::to_string(expected) +
                        ": the shadow's sun is not held constant across the angle step");
        }
        REQUIRE(mc::render::sunShadowSunTick(base + step) == expected + step,
                "the angle step must advance by exactly one step at its boundary");
    }

    // 6b. 步内逐位相同。用固定视点，比的是矩阵的全部 16 个浮点数，不是一个容差。
    const glm::vec3 eye{1'024.5F, 70.0F, -2'048.5F};
    for (const double base : {3'000.0, 5'992.0, 11'000.0}) {
        const glm::mat4 reference = mc::render::sunShadowLightViewProj(sunAtShadowTick(base), eye);
        for (double offset = 1.0; offset < step; offset += 1.0) {
            const glm::mat4 inStep =
                mc::render::sunShadowLightViewProj(sunAtShadowTick(base + offset), eye);
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    REQUIRE(inStep[column][row] == reference[column][row],
                            "tick " + std::to_string(base + offset) + " element [" +
                                std::to_string(column) + "][" + std::to_string(row) +
                                "] = " + std::to_string(inStep[column][row]) + " but tick " +
                                std::to_string(base) + " gave " +
                                std::to_string(reference[column][row]) +
                                ": the light matrix changed inside one angle step, so the shadow's "
                                "sampling phase is still being re-rolled every tick");
                }
            }
        }
        // 反面：跨到下一步必须真的变，否则上面那条对一个恒定矩阵也成立
        const glm::mat4 nextStep =
            mc::render::sunShadowLightViewProj(sunAtShadowTick(base + step), eye);
        bool moved = false;
        for (int column = 0; column < 4 && !moved; ++column) {
            for (int row = 0; row < 4 && !moved; ++row) {
                moved = nextStep[column][row] != reference[column][row];
            }
        }
        REQUIRE(moved, "the light matrix is identical across an angle step boundary at tick " +
                           std::to_string(base) + ", so the bit-for-bit assertion above proved "
                           "nothing — the sun is not advancing at all");
    }

    // 6c. RN-11 的护栏在量化之后仍然成立：步内视点平移只产生整纹素的变化。
    // 量化保住的是「太阳不动」，它不能替相机平移背书，那条仍归 texel snapping。
    const glm::vec3 probe{1'020.0F, 64.0F, -2'052.0F};
    const float resolution = static_cast<float>(mc::render::kSunShadowMapResolution);
    constexpr float kStep = 0.0179856F;
    const glm::vec3 sun = sunAtShadowTick(3'000.0);
    const glm::vec2 referenceTexel =
        (glm::vec2{projectToNdc(mc::render::sunShadowLightViewProj(sun, eye), probe)} * 0.5F + 0.5F) *
        resolution;
    bool sawMotion = false;
    for (int index = 1; index <= 200; ++index) {
        const auto scale = static_cast<float>(index);
        const glm::vec3 moved =
            eye + glm::vec3{kStep * scale, kStep * 0.5F * scale, -kStep * 0.75F * scale};
        // 同一个角度步里的另一个 tick：太阳必须还是同一个，平移才是唯一的变量
        const glm::vec3 sameStepSun = sunAtShadowTick(3'000.0 + static_cast<double>(index % 8));
        const glm::vec2 texel =
            (glm::vec2{projectToNdc(mc::render::sunShadowLightViewProj(sameStepSun, moved), probe)} *
                 0.5F + 0.5F) * resolution;
        const glm::vec2 delta = texel - referenceTexel;
        for (const float component : {delta.x, delta.y}) {
            const float residual = std::abs(component - std::round(component));
            REQUIRE(residual < 2e-2F,
                    "step " + std::to_string(index) + ": a fixed world point moved " +
                        std::to_string(component) +
                        " texels while the camera translated inside one angle step, which is not a "
                        "whole number of texels — texel snapping no longer holds and shadow edges "
                        "will crawl as the camera moves");
        }
        if (std::abs(delta.x) > 0.5F || std::abs(delta.y) > 0.5F) {
            sawMotion = true;
        }
    }
    REQUIRE(sawMotion, "the light frustum never moved across 200 camera steps inside one angle "
                       "step, so the snapping assertion above proved nothing");
}

// ---------------------------------------------------------------------------
// 5. 源码护栏：GPU 上才看得见的三件事，在源码层面钉住
// ---------------------------------------------------------------------------
void checkRendererSourceGuards() {
    const std::string renderer = stripLineComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    const std::string world = stripLineComments(readFile(MC_REBEDROCK_WORLD_RENDERER_SRC));

    // 采样器：独立的 compare 采样器，binding 8 挂的是它而不是调试叠加层那个
    const std::string sampler = functionBody(renderer, "void createShadowCompareSampler()");
    REQUIRE(sampler.find("compareEnable = VK_TRUE") != std::string::npos,
            "the terrain's shadow sampler must enable depth comparison, or sampler2DShadow in "
            "the shaders reads garbage");
    REQUIRE(sampler.find("VK_COMPARE_OP_LESS_OR_EQUAL") != std::string::npos,
            "the shadow compare op must be LESS_OR_EQUAL to match the shaders' 'lit' sense");
    REQUIRE(sampler.find("VK_FILTER_LINEAR") != std::string::npos,
            "the shadow compare sampler must filter LINEAR, so each PCF tap is itself a "
            "hardware 2x2 comparison");
    REQUIRE(renderer.find("shadowImageInfo.sampler = shadowCompareSampler") != std::string::npos,
            "descriptor binding 8 must carry the dedicated compare sampler, not the debug "
            "overlay's NEAREST one");

    // 光锥跟着**渲染视点**走，不是相机对象的位置
    const std::string matrix = functionBody(world, "void updateShadowMatrix()");
    REQUIRE(matrix.find("renderEyeState()") != std::string::npos,
            "updateShadowMatrix must centre the light frustum on the render eye; in third "
            "person the render eye is pulled 4 blocks back and the camera object is not");
    REQUIRE(matrix.find("camera.position()") == std::string::npos,
            "updateShadowMatrix still reads camera.position(), so the light frustum sits on the "
            "player rather than on what is being rendered");

    // RN-24：阴影的太阳必须走量化过的 tick。这条只能在源码层钉——sunShadowSunTick 本身
    // 的行为由几何断言覆盖，但「调用方到底有没有用它」在 headless 下没有别的观察点，
    // 而漏掉这一处的后果正是本轮要修的那个缺陷，且不会有任何几何断言变红。
    REQUIRE(matrix.find("sunShadowSunTick(") != std::string::npos,
            "updateShadowMatrix must feed stateAtTick the quantised sun tick "
            "(render::sunShadowSunTick); reading dayTimeTicks straight puts the light basis back "
            "on a 20 Hz rotation and the shadow map's sampling phase is re-rolled every tick");
    REQUIRE(matrix.find("stateAtTick(\n            clientMirror.world().dayTimeTicks)") ==
                std::string::npos &&
            matrix.find("stateAtTick(clientMirror.world().dayTimeTicks)") == std::string::npos,
            "updateShadowMatrix still passes the raw dayTimeTicks to stateAtTick");

    // 投射者排序里不得再出现视点
    const std::string record = functionBody(world, "void recordShadow(FrameContext& frame)");
    REQUIRE(record.find("camera.position()") == std::string::npos,
            "recordShadow still reads the camera position — the caster ordering must not depend "
            "on it, or sections pop in and out as the player walks");

    // 分辨率不得再有第二个字面量
    REQUIRE(renderer.find("kSunShadowMapResolution, kSunShadowMapResolution") != std::string::npos,
            "the shadow map's resolution must come from SunShadowMap.hpp, not a literal");

    // RN-11b 把两条交换链 VUID 登记为欠账：帧末 copySceneToSwapchain 用 vkCmdCopyImage
    // 写进交换链图像，并先把它 barrier 成 TRANSFER_DST_OPTIMAL，而 createSwapchain
    // 只请求了 COLOR_ATTACHMENT。MoltenVK 宽松，macOS 上看不出来；lavapipe 每帧都报。
    // 这里是那次收口的护栏：usage 必须真的带上 TRANSFER_DST，而且必须**先问过**
    // supportedUsageFlags——COLOR_ATTACHMENT 是 spec 保证的唯一一位，别的都得问。
    const std::string swapchain = functionBody(renderer, "void createSwapchain()");
    REQUIRE(swapchain.find("VK_IMAGE_USAGE_TRANSFER_DST_BIT") != std::string::npos,
            "the swapchain images are a vkCmdCopyImage destination, so they must be created "
            "with TRANSFER_DST usage");
    REQUIRE(swapchain.find("supportedUsageFlags") != std::string::npos,
            "TRANSFER_DST is not a guaranteed surface usage: createSwapchain must consult "
            "supportedUsageFlags before requesting it");
    REQUIRE(swapchain.find("supportedUsageFlags") < swapchain.find("info.imageUsage ="),
            "the capability check must run before the usage is requested, not after");
}

void checkShaderSourceGuards() {
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    // 三个采样者，一个都不能漏：binding 8 挂着 compare 采样器之后，用非 shadow 的
    // sampler2D 采它是未定义用法，MoltenVK 上是 SPIR-V→MSL 转换失败＝黑窗
    for (const char* name : {"grass_block.frag", "block_cutout.frag", "item_entity.frag"}) {
        const std::string source = stripLineComments(readFile(shaderDir / name));
        REQUIRE(source.find("layout(binding = 8) uniform sampler2DShadow shadowDepth;") !=
                    std::string::npos,
                std::string{name} +
                    " must declare binding 8 as sampler2DShadow; a plain sampler2D on a compare "
                    "sampler fails SPIR-V to MSL conversion on MoltenVK");
        REQUIRE(source.find("sampler2D shadowDepth") == std::string::npos,
                std::string{name} + " still declares binding 8 as a non-shadow sampler2D");
        REQUIRE(source.find("sunShadowFactor(") != std::string::npos,
                std::string{name} + " must go through the shared sunShadowFactor(), not its own "
                                    "hand-copied tap");
        REQUIRE(source.find("0.002") == std::string::npos,
                std::string{name} + " still carries the old constant 0.002 depth bias");
    }

    const std::string include = stripLineComments(readFile(shaderDir / "include/sun_shadow.glsl"));
    // z 不得再被重映射：投影是 orthoRH_ZO，深度已经在 [0,1] 里，再 * 0.5 + 0.5 会把它
    // 压进 [0.5,1]。这是换深度约定时最容易漏的一半
    REQUIRE(include.find("projected * 0.5 + 0.5") == std::string::npos,
            "sun_shadow.glsl still remaps all three components; under orthoRH_ZO the depth is "
            "already in [0,1] and remapping squeezes it into [0.5,1]");
    REQUIRE(include.find("projected.xy * 0.5 + 0.5") != std::string::npos,
            "sun_shadow.glsl must still remap xy from [-1,1] to [0,1]");
    // PCF 是 3x3
    REQUIRE(include.find("for (int y = -1; y <= 1; ++y)") != std::string::npos &&
                include.find("for (int x = -1; x <= 1; ++x)") != std::string::npos,
            "sun_shadow.glsl must sample a 3x3 PCF grid");
    // 着色器里的分辨率与 C++ 常量必须一致，否则 PCF 的步长不是一个纹素
    const std::string expected =
        "const float kSunShadowMapResolution = " +
        std::to_string(static_cast<int>(mc::render::kSunShadowMapResolution)) + ".0;";
    REQUIRE(include.find(expected) != std::string::npos,
            "sun_shadow.glsl's shadow map resolution must match SunShadowMap.hpp; expected \"" +
                expected + "\"");
    // 深度换算也必须一致
    std::ostringstream range;
    range << "const float kSunShadowDepthRangeBlocks = " << mc::render::kSunShadowDepthRangeBlocks
          << ";";
    REQUIRE(include.find(range.str()) != std::string::npos,
            "sun_shadow.glsl's depth range must match SunShadowMap.hpp; expected \"" +
                range.str() + "\"");
}

void checkEntityCasters() {
    using namespace mc::render;
    EntityRenderDraws scene;
    const auto cube = [](glm::vec3 center, glm::vec3 size) {
        ItemPush push{};
        push.data.x = kItemModeWorldMatrixCuboid;
        push.dimensions = glm::vec4{size, 0.0F};
        push.viewModelTransform = glm::translate(glm::mat4{1}, center);
        return push;
    };
    scene.begin(ShadowEntityKind::Player);
    scene.append(cube({0, 70.75F, 0}, {0.5F, 1.5F, 0.25F}), 36);
    scene.append(cube({0, 71.75F, 0}, {0.5F, 0.5F, 0.5F}), 36);
    scene.begin(ShadowEntityKind::Item);
    auto item = cube({2, 70.3F, 0}, {1, 1, 1});
    item.data.x = kItemModeGeneratedItem;
    item.viewModelTransform = glm::scale(item.viewModelTransform, glm::vec3{0.3F});
    scene.append(item, 6156);
    scene.begin(ShadowEntityKind::Creature);
    scene.append(cube({-2, 70.5F, 0}, {0.5F, 1, 0.5F}), 36);
    scene.begin(ShadowEntityKind::FallingBlock);
    ItemPush falling{};
    falling.data.x = kItemModeBlockCube;
    falling.positionSize = {4, 72, 0, 1};
    scene.append(falling, 36);
    scene.begin(ShadowEntityKind::Decal);
    scene.append({}, 36);
    scene.begin(ShadowEntityKind::Orb);
    scene.append({}, 6);
    scene.begin(ShadowEntityKind::Creature); // 无几何的实体不占名额
    scene.begin(ShadowEntityKind::Item); // 框外
    scene.append(cube({1000, 70, 0}, {1, 1, 1}), 36);
    const auto matrix = sunShadowLightViewProj(kNoonSun, {0, 70, 0});
    std::vector<std::size_t> entities;
    selectSunShadowEntityCasters(matrix, kNoonSun, scene.casters, entities);
    REQUIRE(entities == std::vector<std::size_t>({0, 1, 2, 3}),
            "player, dropped item, creature and falling block must enter entity caster list; decals/orbs/empty/outside must not");
    REQUIRE(scene.casters[0].drawCount == 2 && scene.casters[1].firstDraw == 2,
            "one player must own all body draws and consume only one entity slot");
    REQUIRE(glm::length(scene.casters[0].bounds.minimum - glm::vec3{-.25F, 70, -.25F}) < 1e-5F &&
            glm::length(scene.casters[0].bounds.maximum - glm::vec3{.25F, 72, .25F}) < 1e-5F,
            "entity bounds must enclose the union of animated body parts");
    REQUIRE(std::abs(scene.casters[1].bounds.maximum.z - 0.009375F) < 1e-6F,
            "generated item bounds must use actual extruded sprite thickness and world scale");
    REQUIRE(scene.casters[3].bounds.minimum == glm::vec3(3.5F, 71.5F, -0.5F),
            "falling block scalar-size fallback must enclose its full cube");

    // 旋转 + 镜像 + 非等比缩放：用八个角作独立 oracle，不重复绝对矩阵公式。
    const auto transform = glm::scale(glm::rotate(glm::translate(glm::mat4{1}, {3, 72, 1}),
        0.71F, glm::normalize(glm::vec3{1, 2, 3})), glm::vec3{-0.2F, 0.3F, 0.1F});
    const glm::vec3 size{4, 8, 6};
    glm::vec3 lo{1e6F}, hi{-1e6F};
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner{(i & 1) ? .5F : -.5F, (i & 2) ? .5F : -.5F, (i & 4) ? .5F : -.5F};
        const glm::vec3 point{transform * glm::vec4{corner * size, 1}};
        lo = glm::min(lo, point); hi = glm::max(hi, point);
    }
    const auto bounds = sunShadowTransformedBounds(transform, size);
    REQUIRE(glm::length(bounds.minimum - lo) < 1e-5F && glm::length(bounds.maximum - hi) < 1e-5F,
            "rotated/mirrored render bounds must match all eight transformed corners");

    std::vector<Aabb> terrain;
    for (int i = 0; i < 900; ++i) {
        const glm::vec3 pos{static_cast<float>(i % 30) - 15, 70, static_cast<float>(i / 30) - 15};
        terrain.push_back({pos, pos + glm::vec3{1}});
    }
    std::vector<std::size_t> before, after;
    selectSunShadowCasters(matrix, kNoonSun, terrain, before);
    for (int i = 0; i < 700; ++i) {
        scene.begin(ShadowEntityKind::Creature);
        scene.append(cube({0, 80, 0}, {1, 2, 1}), 36); // 比玩家更靠近太阳
    }
    selectSunShadowEntityCasters(matrix, kNoonSun, scene.casters, entities);
    selectSunShadowSceneCasters(matrix, kNoonSun, terrain, scene.casters, after, entities);
    REQUIRE(before.size() == 512 && after == before,
            "entity casters must not consume any of the 512 terrain slots or change terrain selection");
    REQUIRE(entities.size() == 512 && std::ranges::find(entities, 0U) != entities.end(),
            "independent entity budget must retain 512 whole entities including the local player");
    std::vector<std::size_t> moved;
    selectSunShadowEntityCasters(sunShadowLightViewProj(kNoonSun, {.01F, 70, 0}), kNoonSun,
                                scene.casters, moved);
    REQUIRE(moved == entities, "entity selection must be stable under small camera translation");
    REQUIRE(!drawEntityShadowDecal(true) && drawEntityShadowDecal(false),
            "sun shadows must suppress circular decals and restore them when disabled");
}

void checkBias() {
    const std::array<float, 7> angles{0, 30, 45, 63, 70, 85, 90};
    const std::array<float, 7> expected{.005F, .022320508F, .035F, .06387832F, .08F, .08F, .08F};
    for (std::size_t i = 0; i < angles.size(); ++i) {
        const float bias = shaderBias::sunShadowBiasBlocks(std::cos(glm::radians(angles[i])));
        REQUIRE(std::isfinite(bias) && bias >= .005F && bias <= .080001F,
                "shader bias outside [0.005, 0.08] blocks at " + std::to_string(angles[i]) +
                " degrees: " + std::to_string(bias));
        REQUIRE(std::abs(bias - expected[i]) < 2e-6F,
                "shader slope bias golden value mismatch at " + std::to_string(angles[i]) +
                " degrees: " + std::to_string(bias));
    }
    REQUIRE(shaderBias::kSunShadowTexelSizeBlocks == mc::render::kSunShadowTexelSize,
            "receiver plane tap spacing must match SunShadowMap texel geometry");
    // 独立几何 oracle：沿光源 right/up 平移一纹素，再沿深度轴移动回 y=70 的平面。
    for (const double tick : {1500.0, 3000.0, 6000.0, 9000.0}) {
        const auto sun = mc::world::DayNightCycle::stateAtTick(tick).sunDirection;
        const auto matrix = mc::render::sunShadowLightViewProj(sun, {0, 70, 0});
        const auto right = glm::normalize(glm::vec3{matrix[0][0], matrix[1][0], matrix[2][0]});
        const auto up = glm::normalize(glm::vec3{matrix[0][1], matrix[1][1], matrix[2][1]});
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            const float offset = shaderBias::sunShadowTapOffsetBlocks(right.y, up.y, sun.y,
                static_cast<float>(x), static_cast<float>(y));
            const glm::vec3 moved = glm::vec3{0, 70, 0} +
                (right * static_cast<float>(x) + up * static_cast<float>(y)) * mc::render::kSunShadowTexelSize -
                sun * offset;
            REQUIRE(std::abs(moved.y - 70.0F) < 1e-5F,
                    "PCF tap reference must remain on the receiver plane (offset sign/scale)");
        }
    }
    REQUIRE(shaderBias::sunShadowBiasBlocks(-1) == .08F &&
            shaderBias::sunShadowBiasBlocks(2) == .005F, "bias clamp must handle backfaces/roundoff");
}

void checkEntityWiring() {
    const auto world = stripLineComments(readFile(MC_REBEDROCK_WORLD_RENDERER_SRC));
    const auto renderer = stripLineComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    const auto record = functionBody(world, "void recordShadow(FrameContext& frame)");
    REQUIRE(record.find("selectSunShadowSceneCasters(") != std::string::npos &&
            record.find("pipelines.entityShadowPipeline") != std::string::npos &&
            record.find("sizeof(draw.push), &draw.push") != std::string::npos &&
            record.find("draw.vertexCount, 1, draw.firstVertex") != std::string::npos,
            "shadow pass must select and submit the collected entity geometry");
    REQUIRE(record.find("vkCmdBeginRenderPass") == std::string::npos &&
            record.find("vkCmdEndRenderPass") == std::string::npos &&
            record.find("vkCmdPipelineBarrier") == std::string::npos,
            "shadow body must not own graph renderpass or layout transitions");
    const auto frame = functionBody(world, "recordCommandBuffer(FrameContext& frame");
    for (const auto name : {"collectEntityShadowDecals();", "collectItemEntities();",
                            "collectWorldEntities();", "collectWorldPlayer();"})
        REQUIRE(frame.find(name) < frame.find("frameGraph.execute"),
                std::string{"same-frame entity preparation missing before graph: "} + name);
    const auto player = functionBody(world, "void collectWorldPlayer()");
    REQUIRE(player.find("shadowDisabled && cameraPerspective == CameraPerspective::FirstPerson") != std::string::npos &&
            player.find("entityDraws_.begin(ShadowEntityKind::Player)") != std::string::npos &&
            player.find("entityDraws_.append(makeWorldCuboidPush(") != std::string::npos,
            "first-person local player must contribute its body when sun shadows are enabled");
    const auto items = functionBody(world, "void collectItemEntities()");
    REQUIRE(items.find("entityDraws_.begin(ShadowEntityKind::Item)") != std::string::npos &&
            items.find("entityDraws_.begin(ShadowEntityKind::FallingBlock)") != std::string::npos &&
            items.find("entityDraws_.append(push, kGeneratedItemVertexCount") != std::string::npos,
            "item snapshot must feed both dropped items and falling blocks into caster collection");
    // RN-23：贴花已从 collectItemEntities 里那段「只给掉落物、画一个固定圆盘」搬到
    // 自己的收集器，并覆盖全部五类实体。RN-11b 的开关判断随之搬家但不改语义：太阳
    // 阴影开着时统一不画，而不是按单只实体是否入选真实投影临时恢复。
    const auto decals = functionBody(world, "void collectEntityShadowDecals()");
    REQUIRE(decals.find("drawEntityShadowDecal(!shadowDisabled)") != std::string::npos,
            "production decal collection must obey the sun shadow switch");
    REQUIRE(decals.find("options.entityShadows") != std::string::npos,
            "the decal must obey the player's Entity Shadows setting as well");
    REQUIRE(items.find("kItemModeEntityShadow") == std::string::npos,
            "the item collector must no longer grow its own private shadow blob");
    REQUIRE(decals.find("clientMirror.player()") != std::string::npos &&
                decals.find("clientMirror.entities()") != std::string::npos &&
                decals.find("frame.snapshot.items()") != std::string::npos &&
                decals.find("frame.snapshot.experienceOrbs()") != std::string::npos &&
                decals.find("frame.snapshot.fallingBlocks()") != std::string::npos,
            "every entity kind that has a shadow radius in 26.1 must reach the decal collector");
    REQUIRE(decals.find("cameraPerspective != CameraPerspective::FirstPerson") != std::string::npos,
            "the local player's decal follows LevelRenderer's first-person rule: in first person "
            "the entity never enters the render list, so it has no shadow either");
    REQUIRE(decals.find("SkyLight::skyDarken(") != std::string::npos,
            "night must dim the decal through the one skyDarken transcription, not a second copy");
    const auto mobs = functionBody(world, "void collectWorldEntities()");
    REQUIRE(mobs.find("entityDraws_.begin(ShadowEntityKind::Creature)") != std::string::npos &&
            mobs.find("entityDraws_.append(makeBoxUvCuboidPush(") != std::string::npos,
            "creature snapshot must submit posed box-UV geometry");
    const auto pipeline = functionBody(renderer, "void createShadowResources()");
    for (const auto token : {"entity_shadow.frag.spv", "item_entity.vert.spv", "shadowVariant = VK_TRUE",
             "vertexBindingDescriptionCount = 0", "vertexAttributeDescriptionCount = 0",
             "rasterization.cullMode = VK_CULL_MODE_NONE", "push.size = sizeof(ItemPush)"})
        REQUIRE(pipeline.find(token) != std::string::npos,
                std::string{"entity depth pipeline contract missing: "} + token);
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    const auto vertex = stripLineComments(readFile(shaderDir / "item_entity.vert"));
    REQUIRE(vertex.find("layout(constant_id = 0) const bool sunShadowPass = false") != std::string::npos,
            "world item vertex shader must default to color projection");
    std::size_t count = 0, offset = 0;
    while ((offset = vertex.find("? camera.lightViewProj * vec4(worldPosition, 1.0)", offset)) != std::string::npos) {
        ++count; ++offset;
    }
    REQUIRE(count == 2, "both generated sprites and cuboids must project through the current light matrix");
    const auto fragment = stripLineComments(readFile(shaderDir / "entity_shadow.frag"));
    REQUIRE(fragment.find("if (alpha < 0.1) discard") != std::string::npos &&
            fragment.find("if (fragmentOpacity < 0.01) discard") != std::string::npos &&
            fragment.find("binding = 8") == std::string::npos,
            "entity shadow must discard transparent texels/hidden edges and never sample its depth attachment");
    const auto sampling = stripLineComments(readFile(shaderDir / "include/sun_shadow.glsl"));
    REQUIRE(sampling.find("#include \"sun_shadow_bias.glsl\"") != std::string::npos &&
            sampling.find("sunShadowBiasBlocks(dot(normal, normalize(sunDirection)))") != std::string::npos,
            "production shadow sampling must use the exact GLSL bias function compiled by the headless test");
    REQUIRE(sampling.find("float tapReference = reference + sunShadowTapOffsetBlocks(") != std::string::npos &&
            sampling.find("tapReference));") != std::string::npos,
            "PCF must compare each tap against its receiver-plane depth");
}

} // namespace

int main() {
    try {
        checkShadowFacing();
        checkEntityCasters();
        checkBias();
        checkEntityWiring();
        checkDepthConvention();
        checkTexelSnapping();
        checkCasterSelection();
        checkSunNeverVertical();
        checkLightBasisHasNoRoll();
        checkSunAngleQuantization();
        checkRendererSourceGuards();
        checkShaderSourceGuards();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
