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
using glm::mix;
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
    // RN-35：级联之后比较采样的坐标是 vec4——xy = uv、**z = 层号**、w = 比较参考值。
    // 层号挤进第三个分量正是 sampler2DArrayShadow 的约定，深度因此搬到了 w
    std::array<vec4, 9> coordinates{};
    std::size_t count = 0;
    // RN-34：遮挡物搜索读的是同一张图的**非比较**视图。夹具把它做成一个可编程的深度
    // 场：默认 1.0（比任何接收点都远 = 没有遮挡物），测试按需覆盖。
    // RN-43：过渡带里两级各跑一次，所以遮挡搜索最多 8 次而不是 4 次
    std::array<float, 8> blockerDepth{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    std::array<vec3, 8> blockerCoordinates{};
    std::size_t blockerCount = 0;
};
using sampler2DArrayShadow = Samples*;
using sampler2DArray = Samples*;
float texture(sampler2DArrayShadow sampler, vec4 coordinates) {
    const auto index = sampler->count++;
    sampler->coordinates.at(index) = coordinates;
    return sampler->visibility.at(index);
}
vec4 texture(sampler2DArray sampler, vec3 coordinates) {
    const auto index = sampler->blockerCount++;
    sampler->blockerCoordinates.at(index) = coordinates;
    return vec4{sampler->blockerDepth.at(index), 0.0F, 0.0F, 0.0F};
}

// Generated from the production GLSL, with only swizzles/float literal syntax
// adapted to C++. The real early return, projection, bias and PCF execute here.
#include "sun_shadow_glsl_cpp.hpp"
}

namespace {

using mc::render::Aabb;

// RN-35：级联的接收端先试近段。既有的那些断言全都围绕**远段**的纹素（1/16 格）展开，
// 所以夹具给近段一个必然出框的矩阵——缩放 10 倍把 z=0.5 推到 5，超出 [0,1]，
// `sunShadowInsideCascade` 因此为假，接收端退到远段。
// 「近段命中时会怎样」由 checkCascadeSelection 单独钉。
const glm::mat4 kMissNearCascade = glm::scale(glm::mat4{1.0F}, glm::vec3{10.0F});

// lightingSettings.z：近段那一层这一帧有没有内容。玩家关掉级联时它是 0，接收端必须
// 直接用远段——checkCascadeSwitch 单独钉那一档
const float kNearCascadeOn = 1.0F;
const float kNearCascadeOff = 0.0F;

// weatherSettings.xy：降雨与雷暴的 0..1 渐变量。晴天两者都是 0，阴影因此与 RN-36
// 之前逐位相同——既有的每一条断言都靠这一点继续成立
const glm::vec2 kClearWeather{0.0F, 0.0F};

// RN-41：thinPlane。普通的实心面是 0，既有的每一条断言都在这一档上——薄片那一档
// （十字植物、作物）由 checkThinPlaneBias 单独钉
const float kSolidFace = 0.0F;
const float kThinPlane = 1.0F;

// RN-42：directWeight。水平地面在任何时刻都是 1，既有的每一条断言都在这一档上——
// 竖直面那一档由 checkDirectWeight 单独钉
const float kGroundFacing = 1.0F;
const float kSunOverhead = 1.0F;

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
                // 遮挡物深度 0 = 离光源最近，离接收点 (z=0.5) 160 格：半影因此
                // 吃满上限 0.5 纹素，下面的 tap 坐标断言与接触硬化之前逐位相同。
                samples.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
                const float factor = shaderReceiver::sunShadowFactor(
                    &samples, &samples, kMissNearCascade, glm::mat4{1.0F},
                    glm::vec3{0, 0, 0.5F}, normal, sun, kNearCascadeOn, kClearWeather, kSolidFace);
                const std::string context = " at N.L=" + std::to_string(incidence);
                if (incidence <= 0.0F) {
                    REQUIRE(factor == 1.0F && samples.count == 0,
                            "back-facing/grazing receiver must return 1 with zero PCF taps" +
                                context + "; factor=" + std::to_string(factor) +
                                ", taps=" + std::to_string(samples.count));
                } else {
                    float lit = 0.0F;
                    for (std::size_t tap = 0; tap < 4; ++tap) lit += pattern[tap];
                    // RN-38：返回的是**可见度**，四个 tap 的平均，不再把「影子里该有
                    // 多亮」烘在里面——那一份现在由 sunSkyFactor 从散射的份额算出
                    const float expected = lit / 4.0F;
                    REQUIRE(samples.count == 4 && std::abs(factor - expected) < 0.000001F,
                            "sun-facing receiver must preserve four-tap PCF visibility" +
                                context + "; factor=" + std::to_string(factor) +
                                ", taps=" + std::to_string(samples.count));
                    // RN-33：投影的**输入**现在是沿法线抬起来的那个点，不是 worldPosition
                    // 本身。这条断言因此同时钉住两件事：抬高确实加在了投影之前（加在之后
                    // 就又变成沿光线方向的位移，亮边原样回来），以及 tap 网格是 ±0.5 纹素。
                    const float lift = shaderBias::sunShadowNormalOffsetBlocks(incidence, shaderBias::kSunShadowFarTexelBlocks);
                    const glm::vec3 lifted = glm::vec3{0, 0, 0.5F} + normal * lift;
                    for (std::size_t tap = 0; tap < 4; ++tap) {
                        // (i - 0.5) * penumbraTexels * 2，而这里 penumbraTexels 饱和在 0.5
                        const float penumbra = shaderBias::sunShadowPenumbraTexels(0.5F * 319.9F, shaderBias::kSunShadowFarTexelBlocks);
                        const float x = (static_cast<float>(tap % 2) - 0.5F) * penumbra * 2.0F;
                        const float y = (static_cast<float>(tap / 2) - 0.5F) * penumbra * 2.0F;
                        const auto expectedUv = glm::vec2{lifted} * 0.5F + glm::vec2{0.5F} +
                                                glm::vec2{x, y} / 2048.0F;
                        const float expectedDepth = lifted.z +
                            (shaderBias::sunShadowTapOffsetBlocks(normal.x, normal.y, incidence, x, y,
                                                shaderBias::kSunShadowFarTexelBlocks) -
                             shaderBias::kSunShadowDepthBiasBlocks) / 319.9F;
                        const auto coordinates = samples.coordinates[tap];
                        REQUIRE(glm::length(glm::vec2{coordinates} - expectedUv) < 0.000001F &&
                                std::abs(coordinates.w - expectedDepth) < 0.000001F &&
                                coordinates.z == 1.0F,
                                "sun-facing receiver must preserve projected PCF coordinates/depth"
                                " and sample the far cascade's layer" + context);
                    }
                }
            }
        }
    }
    // The existing outside-frustum early return remains active for front faces.
    shaderReceiver::Samples outside;
    REQUIRE(shaderReceiver::sunShadowFactor(&outside, &outside, kMissNearCascade, glm::mat4{1.0F},
                glm::vec3{3, 0, 0.5F}, glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0},
                kNearCascadeOn, kClearWeather, kSolidFace) == 1.0F && outside.count == 0,
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
    const std::string record =
        functionBody(world, "void recordShadow(FrameContext& frame, std::size_t cascade)");
    REQUIRE(record.find("camera.position()") == std::string::npos,
            "recordShadow still reads the camera position — the caster ordering must not depend "
            "on it, or sections pop in and out as the player walks");

    // RN-35：把阴影图转成 SHADER_READ_ONLY 的边界屏障是**逐级一条**的。
    //
    // 这一条只有校验层抓得到（headless 没有 layout），所以退一步钉源码。两件事：
    //  * 每条只覆盖自己那一层，层号来自循环变量而不是字面量——写死 0 就是「第二层
    //    停在 DEPTH_STENCIL_ATTACHMENT_OPTIMAL 却被采样」，现场是 imageLayout-00344；
    //  * 屏障跟着它那一步的启用状态走。合成一条覆盖两层的屏障在两级都画时是对的，
    //    可级联一关，层 0 那一步被剪、它停在 SHADER_READ_ONLY，而屏障的 oldLayout
    //    仍写着 DEPTH_STENCIL_ATTACHMENT——对不上。这是 RN-20a 那条「屏障必须跟着
    //    它的步一起被剪」，粒度从整张图细到一层。
    const std::string tables = functionBody(renderer, "void buildFrameGraphTables(");
    REQUIRE(tables.find("shadowRead.subresourceRange.baseArrayLayer = "
                        "static_cast<std::uint32_t>(cascade);") != std::string::npos &&
                tables.find("shadowRead.subresourceRange.baseArrayLayer = 0") == std::string::npos,
            "each shadow-read boundary barrier must carry its own cascade's layer index");
    REQUIRE(tables.find("shadowRead.subresourceRange.layerCount = 1;") != std::string::npos,
            "one barrier per cascade layer: a single barrier spanning both cannot be pruned with "
            "the near step when cascades are switched off");
    REQUIRE(tables.find("const bool drawn = cascade == 0 ? sunShadowNearCascadeEnabled() : "
                        "!shadowDisabled;") != std::string::npos,
            "the per-cascade barrier must be pruned with the step that writes that layer");
    REQUIRE(tables.find("render::kSunShadowCascadeCount") != std::string::npos,
            "the frame-graph tables must derive their cascade count from SunShadowMap.hpp");

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
        // RN-35：阴影图是一张两层的数组，所以是 sampler2DArrayShadow。仍然必须是
        // **shadow** 那一族：用非 shadow 的采样器采一个开了 compare 的采样器是未定义
        // 用法，MoltenVK 上是 SPIR-V→MSL 转换失败＝黑窗
        REQUIRE(source.find("layout(binding = 8) uniform sampler2DArrayShadow shadowDepth;") !=
                    std::string::npos,
                std::string{name} +
                    " must declare binding 8 as sampler2DArrayShadow; a non-shadow sampler on a "
                    "compare sampler fails SPIR-V to MSL conversion on MoltenVK");
        // 带分号：binding 8 那一个叫 shadowDepth，RN-34 新增的 binding 10 叫
        // shadowDepthRaw，前缀相同。少了这个分号，新绑定点会被误判成旧缺陷。
        REQUIRE(source.find("sampler2D shadowDepth;") == std::string::npos &&
                    source.find("sampler2DArray shadowDepth;") == std::string::npos,
                std::string{name} + " still declares binding 8 as a non-shadow sampler");
        // RN-34：接触硬化要读回深度值本身，比较采样器做不到。三个采样者都必须有这第二
        // 个绑定点——漏一个，那个着色器就会拿一个未声明的采样器去编译（MoltenVK 上是
        // 黑窗），或者干脆退回固定半径。
        REQUIRE(source.find("layout(binding = 10) uniform sampler2DArray shadowDepthRaw;") !=
                    std::string::npos,
                std::string{name} + " must also bind the non-compare view of the shadow map "
                                    "(binding 10) that the blocker search reads");
        REQUIRE(source.find("sunShadowFactor(shadowDepth, shadowDepthRaw,") != std::string::npos,
                std::string{name} + " must pass both shadow views into sunShadowFactor");
        REQUIRE(source.find("sunShadowFactor(") != std::string::npos,
                std::string{name} + " must go through the shared sunShadowFactor(), not its own "
                                    "hand-copied tap");
        REQUIRE(source.find("0.002") == std::string::npos,
                std::string{name} + " still carries the old constant 0.002 depth bias");
        // RN-35：UBO 里的光源矩阵是**两个**。三个 .frag 与 item_entity.vert 的声明必须
        // 逐字节一致——漏一处就是一次静默的 std140 错位，而 block_cutout.frag 的抬头
        // 记着上一次同样的事故（lightViewProj 早了 64 字节）
        REQUIRE(source.find("mat4 lightViewProj[2];") != std::string::npos,
                std::string{name} + " must declare the cascade light matrices as an array of two");
    }

    const std::string include = stripLineComments(readFile(shaderDir / "include/sun_shadow.glsl"));
    // z 不得再被重映射：投影是 orthoRH_ZO，深度已经在 [0,1] 里，再 * 0.5 + 0.5 会把它
    // 压进 [0.5,1]。这是换深度约定时最容易漏的一半
    REQUIRE(include.find("projected * 0.5 + 0.5") == std::string::npos,
            "sun_shadow.glsl still remaps all three components; under orthoRH_ZO the depth is "
            "already in [0,1] and remapping squeezes it into [0.5,1]");
    REQUIRE(include.find("projected.xy * 0.5 + 0.5") != std::string::npos,
            "sun_shadow.glsl must still remap xy from [-1,1] to [0,1]");
    // RN-33：PCF 是 2x2，tap 位于 ±0.5 纹素。半影因此是 0.125 格而不是 0.25 格 ——
    // 那 0.25 格是**固定**的，不管接收点离挡光的方块多远都糊同样宽，方块脚下本该硬边的
    // 地方也照糊，那正是「贴近方块处漏光」的主因。
    REQUIRE(include.find("for (int y = 0; y < 2; ++y)") != std::string::npos &&
                include.find("for (int x = 0; x < 2; ++x)") != std::string::npos &&
                include.find("float tapX = (float(x) - 0.5) * penumbraTexels * 2.0;") !=
                    std::string::npos &&
                include.find("lit * 0.25") != std::string::npos,
            "sun_shadow.glsl must sample a 2x2 PCF grid at the blocker-scaled radius, averaged by 4");

    // ---- RN-34：接触硬化的两条结构性判据 --------------------------------
    //
    // ① 遮挡物搜索的半径必须**严格覆盖** PCF 的足迹。搜索半径小了，「没找到遮挡物」
    //    那条提前返回就会漏掉本该投影的边缘像素，影子边上出现一圈缺口——而那是一种
    //    只有在特定太阳角才看得见的缺陷，出图不一定拍得到。
    //    搜索是 ±1.5 纹素；PCF 的 tap 最远 ±0.5，加硬件双线性的 ±0.5，合计 ±1.0。
    REQUIRE(include.find("float searchTexel = 1.5 * texel;") != std::string::npos,
            "blocker search radius must be stated explicitly (and stay >= the PCF footprint)");
    REQUIRE(1.5F > shaderBias::kSunMaxPenumbraTexels + 0.5F,
            "blocker search radius must strictly cover the PCF footprint; otherwise the "
            "no-blocker early-out drops shadowed pixels at the silhouette");
    // ② 提前返回必须在**搜索之后、PCF 之前**。放在别处它要么没省下什么，要么会跳过
    //    本该做的比较。
    const auto searchAt = include.find("float searchTexel");
    const auto earlyAt = include.find("if (blockerCount == 0.0)");
    const auto pcfAt = include.find("for (int y = 0; y < 2; ++y)");
    REQUIRE(searchAt != std::string::npos && earlyAt != std::string::npos &&
                pcfAt != std::string::npos && searchAt < earlyAt && earlyAt < pcfAt,
            "the no-blocker early-out must sit between the blocker search and the PCF loop");
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

// RN-35：级联。两级、两张正交框、两个独立的吸附步长、接收端选一级。
//
// 这四件事互相独立地会坏，而且坏了都不崩：吸附步长写成同一个 ⇒ 近段跟着粗纹素跳；
// 纹素尺寸没跟着级别走 ⇒ 偏置抬高 8 倍、影子从脚下浮起来；选级判据写反 ⇒ 玩家附近
// 反而用远段。所以逐条钉。
void checkCascades() {
    using mc::render::kSunShadowCascadeCount;
    using mc::render::sunShadowTexelSize;
    REQUIRE(kSunShadowCascadeCount == 2, "本轮是两级；加级数要连着下面的黄金值一起改");

    // ---- 1. GLSL 与 C++ 的纹素尺寸必须逐位相同 ----------------------------
    //
    // 着色器那边是两个手写常量（GLSL 没有 constexpr 能算 2*halfExtent/resolution）。
    // 两处一漂移，接收端就按错误的纹素抬偏置——而画面上只表现为「影子有点飘」
    REQUIRE(shaderBias::sunShadowTexelBlocks(0) == sunShadowTexelSize(0) &&
                shaderBias::sunShadowTexelBlocks(1) == sunShadowTexelSize(1),
            "receiver texel sizes must match SunShadowMap's per-cascade geometry");
    REQUIRE(sunShadowTexelSize(1) == sunShadowTexelSize(0) * 8.0F,
            "near cascade must be exactly eight times finer; the receiver's constants assume it");

    // ---- 2. 每一级吸附到**自己**的纹素网格 --------------------------------
    //
    // 用同一个步长吸附两级，细的那一级就只在粗纹素的整数倍上落脚：相机在一个粗纹素内
    // 平移时近段整体不动，跨过时跳 8 个细纹素——那正是 RN-24 要消灭的爬行，换了个尺度。
    const auto sun = glm::normalize(mc::world::DayNightCycle::stateAtTick(3000.0).sunDirection);
    const glm::vec3 eye{12.3F, 70.0F, -45.7F};
    const auto far = mc::render::sunShadowLightViewProj(sun, eye, 1);
    const auto right = glm::normalize(glm::vec3{far[0][0], far[1][0], far[2][0]});
    const float nearTexel = sunShadowTexelSize(0);
    const float farTexel = sunShadowTexelSize(1);

    // 从矩阵里取回吸附后的光源空间中心。lightViewProj = ortho * translate(-center) * R，
    // 而 ortho 在 x 上的缩放是 1/halfExtent，所以 m[3][0] = -center.x / halfExtent。
    // 直接问「中心落在网格上吗」，比问「平移一点点画面变没变」稳：后者取决于起点
    // 离量化边界多远，是一条会看起点脸色的断言
    const auto snappedCenterX = [](const glm::mat4& matrix, std::size_t cascade) {
        return -matrix[3][0] * mc::render::kSunShadowOrthoHalfExtents[cascade];
    };
    const auto isMultiple = [](float value, float step) {
        const float quotient = value / step;
        return std::abs(quotient - std::round(quotient)) < 1e-3F;
    };
    bool nearOffFarGrid = false;
    for (const float step : {0.0F, 0.013F, 0.031F, 0.077F, 0.211F, 0.5F, 1.37F}) {
        const glm::vec3 moved = eye + right * step;
        const auto nearMatrix = mc::render::sunShadowLightViewProj(sun, moved, 0);
        const auto farMatrix = mc::render::sunShadowLightViewProj(sun, moved, 1);
        REQUIRE(isMultiple(snappedCenterX(nearMatrix, 0), nearTexel),
                "the near cascade's centre must land on its own texel grid");
        REQUIRE(isMultiple(snappedCenterX(farMatrix, 1), farTexel),
                "the far cascade's centre must land on its own texel grid");
        // ★ 两级各自吸附的真正证据：近段的中心**不必**落在远段的网格上。
        // 两级共用一个步长时这条永远为假——而画面上只表现为「近段跟着粗纹素跳」
        nearOffFarGrid =
            nearOffFarGrid || !isMultiple(snappedCenterX(nearMatrix, 0), farTexel);
    }
    REQUIRE(nearOffFarGrid,
            "the near cascade snapped to the far cascade's step for every probe: the two are "
            "sharing one snap step, and RN-24's stability was bought for the far one only");

    // ---- 3. 近段的框确实窄八倍 --------------------------------------------
    //
    // 同一个世界点在两级里的 NDC 相差正好 8 倍——那既是「16 格 vs 128 格」的另一种说法，
    // 也是「两级共用同一个旋转」的证据（差的只有一个标量）。
    // 用**两个** probe 的 NDC 之差，不是单个 probe 的绝对值：两级的中心各自吸附到
    // 各自的网格，绝对值里因此还含着一个与比例无关的平移
    const glm::mat4 nearMatrix = mc::render::sunShadowLightViewProj(sun, eye, 0);
    const glm::mat4 farMatrix = mc::render::sunShadowLightViewProj(sun, eye, 1);
    const glm::vec3 probeA = eye + right * 1.0F;
    const glm::vec3 probeB = eye + right * 4.0F;
    const float nearSpan = (nearMatrix * glm::vec4{probeB, 1.0F}).x -
                           (nearMatrix * glm::vec4{probeA, 1.0F}).x;
    const float farSpan =
        (farMatrix * glm::vec4{probeB, 1.0F}).x - (farMatrix * glm::vec4{probeA, 1.0F}).x;
    REQUIRE(std::abs(nearSpan - farSpan * 8.0F) < 1e-4F,
            "the near cascade's box must be exactly eight times narrower");
    // 深度轴两级共用：同一个点的 z 必须逐位相同，否则偏置那一套以「格」为单位的换算
    // 在两级里不再是同一件事
    REQUIRE(std::abs((nearMatrix * glm::vec4{probeA, 1.0F}).z -
                     (farMatrix * glm::vec4{probeA, 1.0F}).z) < 1e-6F,
            "both cascades must share one depth range, or the bias' block units stop meaning the "
            "same thing in the two");

    // ---- 4. 接收端选级 ----------------------------------------------------
    //
    // 近段命中时层号必须是 0、tap 间距必须按近段的纹素算。夹具给近段一个恒等矩阵
    // （点落在框内），远段给一个不会被走到的矩阵。
    shaderReceiver::Samples samples{};
    samples.visibility = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    samples.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
    const float factor = shaderReceiver::sunShadowFactor(
        &samples, &samples, glm::mat4{1.0F}, kMissNearCascade, glm::vec3{0, 0, 0.5F},
        glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather, kSolidFace);
    REQUIRE(samples.count == 4 && factor == 1.0F,
            "a receiver inside the near box must take the near cascade and still do four taps");
    for (std::size_t tap = 0; tap < samples.count; ++tap) {
        REQUIRE(samples.coordinates[tap].z == 0.0F,
                "every tap must read layer 0 when the near cascade is selected");
    }
    for (std::size_t tap = 0; tap < samples.blockerCount; ++tap) {
        REQUIRE(samples.blockerCoordinates[tap].z == 0.0F,
                "the blocker search must read the same layer the PCF taps do");
    }
    // ★ 近段的**法线抬升**必须按近段的纹素算。
    //
    // 这一条是补出来的：源码护栏（「texelBlocks = sunShadowTexelBlocks(cascade)」必须在）
    // 会被**第二处**赋值满足，于是把第一处改成写死 1 的注入一路绿灯通过——而那正是
    // 「近段按远段的纹素抬 8 倍」这个缺陷的样子，画面上是影子整片从脚下浮起来。
    // 所以这里不问源码长什么样，问的是投影**输入**落在哪：抬升量差 8 倍，uv 就差 8 倍。
    {
        const glm::vec3 grazingSun = glm::normalize(glm::vec3{1.0F, 1.0F, 0.0F});
        const glm::vec3 up{0.0F, 1.0F, 0.0F};
        const float incidence = glm::dot(up, grazingSun);
        shaderReceiver::Samples grazing{};
        grazing.visibility = {1, 1, 1, 1, 1, 1, 1, 1, 1};
        grazing.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
        static_cast<void>(shaderReceiver::sunShadowFactor(
            &grazing, &grazing, glm::mat4{1.0F}, kMissNearCascade, glm::vec3{0, 0, 0.5F}, up,
            grazingSun, kNearCascadeOn, kClearWeather, kSolidFace));
        REQUIRE(grazing.count == 4, "the grazing near-cascade probe must reach the PCF taps");
        const float nearLift = shaderBias::sunShadowNormalOffsetBlocks(
            incidence, shaderBias::kSunShadowNearTexelBlocks);
        const float farLift = shaderBias::sunShadowNormalOffsetBlocks(
            incidence, shaderBias::kSunShadowFarTexelBlocks);
        REQUIRE(std::abs(farLift - nearLift * 8.0F) < 1e-6F && nearLift > 0.0F,
                "fixture check: the two cascades' lifts must differ by eight, or the assertion "
                "below cannot tell them apart");
        // 抬升沿 +Y，恒等矩阵下它整个进 uv.y。tap 的横向偏移只动 x，所以 y 是干净的
        const float expectedY = (0.0F + nearLift) * 0.5F + 0.5F;
        const float wrongY = (0.0F + farLift) * 0.5F + 0.5F;
        for (std::size_t tap = 0; tap < grazing.count; ++tap) {
            const float actualY = grazing.coordinates[tap].y;
            REQUIRE(std::abs(actualY - expectedY) < 1.0F / 2048.0F,
                    "the near cascade must lift along the normal by ITS OWN texel, not the far "
                    "one's: expected uv.y " + std::to_string(expectedY) + ", got " +
                        std::to_string(actualY));
            REQUIRE(std::abs(actualY - wrongY) > 1.0F / 2048.0F,
                    "fixture check: the far cascade's lift must be distinguishable here");
        }
    }

    // ---- 5. 玩家把级联关掉 -------------------------------------------------
    //
    // 关掉时近段那一步在**编译期**被剪，层 0 里留着的是上一次的内容。接收端不跳过它，
    // 脚下就盖着一片陈旧的影子，而且它随玩家走动而不动——按钮能点、值能存、画面却
    // 「有效果但是错的」，比毫无效果更难查。
    {
        shaderReceiver::Samples off{};
        off.visibility = {1, 1, 1, 1, 1, 1, 1, 1, 1};
        off.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
        // 近段给恒等（点落在它框内），远段也给恒等：关掉之后必须**直接**用远段，
        // 而不是「先试近段命中了就用」
        static_cast<void>(shaderReceiver::sunShadowFactor(&off, &off, glm::mat4{1.0F},
                                                          glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F},
                                                          glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0},
                                                          kNearCascadeOff, kClearWeather, kSolidFace));
        REQUIRE(off.count == 4, "switching cascades off must still shadow, just from the far map");
        for (std::size_t tap = 0; tap < off.count; ++tap) {
            REQUIRE(off.coordinates[tap].z == 1.0F,
                    "with cascades off every tap must read layer 1: layer 0 holds whatever the "
                    "pruned near step left there");
        }
        for (std::size_t tap = 0; tap < off.blockerCount; ++tap) {
            REQUIRE(off.blockerCoordinates[tap].z == 1.0F,
                    "the blocker search must not read the stale near layer either");
        }
        // 同一个夹具、只翻这一档：开时读层 0。两条并排才说明这一档真的被读了
        shaderReceiver::Samples on{};
        on.visibility = {1, 1, 1, 1, 1, 1, 1, 1, 1};
        on.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
        static_cast<void>(shaderReceiver::sunShadowFactor(&on, &on, glm::mat4{1.0F},
                                                          glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F},
                                                          glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0},
                                                          kNearCascadeOn, kClearWeather, kSolidFace));
        REQUIRE(on.count == 4 && on.coordinates[0].z == 0.0F,
                "fixture check: with the same matrices, cascades on must select layer 0 — "
                "otherwise the assertion above passes for the wrong reason");
    }

    // ---- 6. 那一档必须真的接到渲染器上 ------------------------------------
    //
    // 漏掉这一步是最安静的一种：按钮能点、值能存进 options.properties、下次启动也读得
    // 回来，但帧图不重编译，近段那一步照跑。headless 看不到 Vulkan，所以钉源码。
    {
        const auto renderer = stripLineComments(readFile(MC_REBEDROCK_RENDERER_SRC));
        const auto applied = functionBody(renderer, "void applyOptionChanged(ui::WidgetId id)");
        const auto switchCase = applied.find("case ui::WidgetId::CascadedShadows:");
        REQUIRE(switchCase != std::string::npos,
                "toggling cascades must have a case in applyOptionChanged, or the button stores a "
                "value and changes nothing");
        REQUIRE(applied.find("rebuildFrameGraph();", switchCase) != std::string::npos,
                "the cascade toggle prunes a compile-time step, so it must recompile the graph");
        // 剪枝与接收端那一位必须来自**同一个**判据，否则会出现「这一步不画了但着色器
        // 还在读它」的半开状态
        REQUIRE(renderer.find("bool sunShadowNearCascadeEnabled() const") != std::string::npos &&
                    renderer.find("uniform.lightingSettings.z = sunShadowNearCascadeEnabled()") !=
                        std::string::npos,
                "the pruning predicate and the receiver's flag must be one function, not two");
    }

    // 近段的半影上限仍是 0.5 个**纹素**，而纹素细八倍 ⇒ 物理半影细八倍。
    // 这就是这个节点买到的东西，写成断言而不是散文
    REQUIRE(shaderBias::sunShadowPenumbraTexels(1e6F, shaderBias::kSunShadowNearTexelBlocks) ==
                    shaderBias::kSunMaxPenumbraTexels &&
                shaderBias::kSunShadowNearTexelBlocks * shaderBias::kSunMaxPenumbraTexels * 8.0F ==
                    shaderBias::kSunShadowFarTexelBlocks * shaderBias::kSunMaxPenumbraTexels,
            "the near cascade's maximum penumbra must be one eighth of the far cascade's in world "
            "units — that eight is the whole point of the node");
}

// RN-36：下雨时影子必须跟着变浅。
//
// 现场（用户实机）：雨天天空盒暗下来，影子却还是同样浓、同样锐，割裂感明显。
// 成因是三个量在着色器里是**乘性**的——天气减光乘在天光上，阴影可见度也乘在天光上，
// 于是影子相对周围的深度恒为 65%，晴天雨天一个样。物理上全阴天没有直射光，
// 也就没有明显的影子。
void checkWeatherResponse() {
    // ---- 1. 云量的黄金值 ---------------------------------------------------
    //
    // 权重 0.9 / 0.1：纯下雨留一丝残影，雷暴（vanilla 里必然同时在下雨）完全没有。
    REQUIRE(shaderBias::sunShadowOvercast(0.0F, 0.0F) == 0.0F, "晴天云量必须是 0");
    REQUIRE(std::abs(shaderBias::sunShadowOvercast(1.0F, 0.0F) - 0.9F) < 1e-6F,
            "纯下雨的云量是 0.9");
    REQUIRE(std::abs(shaderBias::sunShadowOvercast(1.0F, 1.0F) - 1.0F) < 1e-6F,
            "雨加雷暴的云量满值");
    // 两条 gradient 都是渐变量，所以云量在天气转换期间连续——不会在某一 tick 上跳
    float previous = -1.0F;
    for (int step = 0; step <= 10; ++step) {
        const float rain = static_cast<float>(step) / 10.0F;
        const float overcast = shaderBias::sunShadowOvercast(rain, 0.0F);
        REQUIRE(overcast > previous, "云量必须随降雨单调上升");
        previous = overcast;
    }
    // 越界的输入夹住而不是外推（渐变量来自插值，端点上可能差一个 ulp）
    REQUIRE(shaderBias::sunShadowOvercast(2.0F, 2.0F) == 1.0F &&
                shaderBias::sunShadowOvercast(-1.0F, -1.0F) == 0.0F,
            "云量必须夹在 [0,1]");

    // ---- 2. 天光的两项：直射被挡住、散射不受影响 ---------------------------
    //
    // RN-38：`sunShadowFactor` 现在回答的是**可见度**（1 = 太阳完全照到），
    // 「影子里该有多亮」由 sunSkyFactor 从散射的份额算出来，不再是一个烘在
    // 接收端里的 0.35。
    {
        const float lit = shaderBias::sunSkyFactor(1.0F, 1.0F, 1.0F, 0.0F, 0.0F, kGroundFacing, kSunOverhead);
        const float shadowed = shaderBias::sunSkyFactor(1.0F, 1.0F, 0.0F, 0.0F, 0.0F, kGroundFacing, kSunOverhead);
        REQUIRE(std::abs(lit - 1.0F) < 1e-6F,
                "晴天全亮必须是 1.0——受光面的亮度一个字都不该动");
        REQUIRE(std::abs(shadowed - shaderBias::kSkyAmbientFraction) < 1e-6F,
                "晴天全影剩下的正是天空散射那一份，不是一个硬编码的系数");
        // 影子比从前更暗（0.2 而不是 0.35），那是这个节点买到的东西
        REQUIRE(shadowed < 0.35F, "拆开之后影子必须比那个 0.35 的系数更暗");

        // 云把直射**转给**散射：全阴时阴影完全不起作用，而总亮度不变
        const float overcastLit = shaderBias::sunSkyFactor(1.0F, 1.0F, 1.0F, 1.0F, 1.0F, kGroundFacing, kSunOverhead);
        const float overcastShadowed = shaderBias::sunSkyFactor(1.0F, 1.0F, 0.0F, 1.0F, 1.0F, kGroundFacing, kSunOverhead);
        REQUIRE(std::abs(overcastLit - overcastShadowed) < 1e-6F,
                "全阴时受光与全影必须一样亮——没有直射就没有影子");
        REQUIRE(std::abs(overcastLit - 1.0F) < 1e-6F,
                "云只是把直射散开，不吸收：总量的下降归 weatherDimming 单独表达");
        // 纯下雨：直射还剩一成，影子的对比度因此也只剩一成
        const float rainLit = shaderBias::sunSkyFactor(1.0F, 1.0F, 1.0F, 1.0F, 0.0F, kGroundFacing, kSunOverhead);
        const float rainShadowed = shaderBias::sunSkyFactor(1.0F, 1.0F, 0.0F, 1.0F, 0.0F, kGroundFacing, kSunOverhead);
        const float clearContrast = lit - shadowed;
        REQUIRE(std::abs((rainLit - rainShadowed) - clearContrast * 0.1F) < 1e-6F,
                "纯下雨的影子对比度应当是晴天的十分之一");
        // 天气的总量下降是**另一件事**，它对两项一视同仁
        REQUIRE(std::abs(shaderBias::sunSkyFactor(1.0F, 0.5F, 0.0F, 0.0F, 0.0F, kGroundFacing, kSunOverhead) -
                         shadowed * 0.5F) < 1e-6F,
                "weatherDimming 只缩放总量，不改变直射与散射的比例");
    }

    // ---- 3. 接收端返回的是可见度 -------------------------------------------
    const auto run = [](glm::vec2 weather) {
        shaderReceiver::Samples samples{};
        samples.visibility = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        samples.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F};
        const float factor = shaderReceiver::sunShadowFactor(
            &samples, &samples, kMissNearCascade, glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F},
            glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, kNearCascadeOn, weather, kSolidFace);
        return std::pair{factor, samples.count};
    };
    const auto [clearFactor, clearTaps] = run(kClearWeather);
    REQUIRE(clearTaps == 4 && clearFactor == 0.0F,
            "四个 tap 全被挡住 ⇒ 可见度是 0，而不是某个「影子里的亮度」");
    // ★ 云厚到直射不剩什么时**一次采样都不做**。这是逐屏幕像素的一整套遮挡搜索加
    // PCF，暴雨里它产出的是一个看不见的差别
    const auto [stormFactor, stormTaps] = run(glm::vec2{1.0F, 1.0F});
    REQUIRE(stormFactor == 1.0F && stormTaps == 0,
            "雷暴里直射已经没了，整套采样必须省掉，而不是照跑一遍再乘一个 1");
    // 纯下雨还没到那个阈值，仍然要采样——省掉它就是让雨中的影子突然消失
    const auto [rainFactor, rainTaps] = run(glm::vec2{1.0F, 0.0F});
    REQUIRE(rainTaps == 4 && rainFactor == 0.0F,
            "纯下雨仍然采样：可见度与晴天相同，淡下去的是它在 sunSkyFactor 里的权重");

    // ---- 4. 三个采样者都要把天气传进去，色调都不能被阴影冲淡 ---------------
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    for (const char* name : {"grass_block.frag", "block_cutout.frag", "item_entity.frag"}) {
        const std::string source = stripLineComments(readFile(shaderDir / name));
        REQUIRE(source.find("camera.weatherSettings.xy") != std::string::npos,
                std::string{name} + " must pass the weather gradients into sunShadowFactor: "
                                    "one shader left out is one surface whose shadow ignores rain");
        // RN-38：色调的权重**不含阴影**。用含阴影的 skyFactor 会把影子里的色调冲淡
        // 成白——那正是影子看起来「偏灰」而不是天空的蓝的原因
        REQUIRE(source.find("float tintWeight = camera.sunDirection.w * camera.weatherSettings.z;") !=
                        std::string::npos &&
                    source.find("mix(vec3(1.0), skyTint, tintWeight)") != std::string::npos,
                std::string{name} + " must weight the sky tint by time and weather only, never by "
                                    "the shadow: a shadow is lit by the sky and should keep its "
                                    "colour");
        REQUIRE(source.find("mix(vec3(1.0), skyTint, skyFactor)") == std::string::npos,
                std::string{name} + " still tints by the shadowed sky factor");
    }
    {
        // RN-38：下落方块那条本来就是「环境 + 直射」的雏形（0.72 + 0.28），只是那两个
        // 数与地形那一套各写各的。「天光里有多少是散射」在整个仓库里只能有一个答案。
        // RN-42 更进一步：连**分配**都不再手抄，直接调用 sunSkyFactor，所以这里钉的
        // 不再是「用了那个常数」，而是「用了那个函数」
        const std::string source =
            stripLineComments(readFile(shaderDir / "item_entity.frag"));
        REQUIRE(source.find("sunSkyFactor(") != std::string::npos,
                "the falling block's sky split must come from the shared function");
        REQUIRE(source.find("0.72") == std::string::npos,
                "item_entity.frag still carries its own copy of the ambient share");
    }
}

void checkBias() {
    // RN-33：随入射角变化的那一项在**法线**轴上，不在深度轴上。抬高 = 一个纹素 x sin(角)。
    const std::array<float, 7> angles{0, 30, 45, 63, 70, 85, 90};
    const std::array<float, 7> expected{0.0F,       .03125F,    .044194174F, .055687908F,
                                        .058730789F, .062262169F, .0625F};
    for (std::size_t i = 0; i < angles.size(); ++i) {
        const float lift = shaderBias::sunShadowNormalOffsetBlocks(std::cos(glm::radians(angles[i])),
                                                shaderBias::kSunShadowFarTexelBlocks);
        REQUIRE(std::isfinite(lift) && lift >= 0.0F &&
                    lift <= shaderBias::kSunShadowFarTexelBlocks + 1e-6F,
                "normal offset outside [0, one texel] blocks at " + std::to_string(angles[i]) +
                " degrees: " + std::to_string(lift));
        REQUIRE(std::abs(lift - expected[i]) < 2e-6F,
                "normal offset golden value mismatch at " + std::to_string(angles[i]) +
                " degrees: " + std::to_string(lift));
    }
    // 深度轴只剩常数底值。它必须**小**：这一项是唯一还会把影子从投射者脚下推开的东西，
    // 亮边宽度 = 它 x cos(太阳仰角)。0.005 格是 1/200 格，看不见；旧的 0.08 看得见。
    REQUIRE(shaderBias::kSunShadowDepthBiasBlocks > 0.0F &&
                shaderBias::kSunShadowDepthBiasBlocks <= 0.01F,
            "depth bias must stay a quantisation floor; anything larger reopens peter-panning");
    REQUIRE(shaderBias::kSunShadowFarTexelBlocks == mc::render::kSunShadowTexelSize,
            "receiver plane tap spacing must match SunShadowMap texel geometry");
    // 独立几何 oracle：沿光源 right/up 平移一纹素，再沿深度轴移动回 y=70 的平面。
    for (const double tick : {1500.0, 3000.0, 6000.0, 9000.0}) {
        const auto sun = mc::world::DayNightCycle::stateAtTick(tick).sunDirection;
        const auto matrix = mc::render::sunShadowLightViewProj(sun, {0, 70, 0});
        const auto right = glm::normalize(glm::vec3{matrix[0][0], matrix[1][0], matrix[2][0]});
        const auto up = glm::normalize(glm::vec3{matrix[0][1], matrix[1][1], matrix[2][1]});
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            const float offset = shaderBias::sunShadowTapOffsetBlocks(right.y, up.y, sun.y,
                static_cast<float>(x), static_cast<float>(y), shaderBias::kSunShadowFarTexelBlocks);
            const glm::vec3 moved = glm::vec3{0, 70, 0} +
                (right * static_cast<float>(x) + up * static_cast<float>(y)) * mc::render::kSunShadowTexelSize -
                sun * offset;
            REQUIRE(std::abs(moved.y - 70.0F) < 1e-5F,
                    "PCF tap reference must remain on the receiver plane (offset sign/scale)");
        }
    }
    // 护栏：喂进 [0,1] 之外的 cos 时结果仍然有限且有界。(-1) 走到上界，(2) 走到 0。
    // 背面在 sunShadowFactor 开头就返回了，走不到这里，但这条 clamp 不能删。
    REQUIRE(shaderBias::sunShadowNormalOffsetBlocks(-1, shaderBias::kSunShadowFarTexelBlocks) == shaderBias::kSunShadowFarTexelBlocks &&
            shaderBias::sunShadowNormalOffsetBlocks(2, shaderBias::kSunShadowFarTexelBlocks) == 0.0F,
            "normal-offset clamp must handle backfaces/roundoff");
}

// RN-34：接触硬化。糊多宽由**遮挡物离接收面多远**决定，而不是一个定值。
//
// 现场：墙根一条约 0.15 格宽、亮度过量 31% 的软亮带（沿墙根方向平均 60 个样本量出来的，
// 单条剖面会被草纹理的棋盘噪声淹没）。成因是固定半径的 PCF —— 方块脚下的遮挡距离是 0，
// 那里物理上应当是硬边，却照样吃满整个足迹。亮带宽度 = 足迹半宽 / sin(太阳仰角)，
// 所以低太阳时格外宽。
//
// 夹具让接收点落在 z = 0.5、法线朝 +Y、太阳正上方（入射角 0 ⇒ 法线偏移为 0，
// 投影出来的 z 正好是 0.5），于是「遮挡物深度」可以直接换算成「离接收面多少格」。
// RN-41：竖直薄片（十字植物、作物）的自遮。
void checkThinPlaneBias() {
    // ---- 1. 偏置本身 -------------------------------------------------------
    // 正午（朝上法线的入射角余弦 = 1）要抬满一格：那正是一株草在阴影图里横跨的深度。
    REQUIRE(std::abs(shaderBias::sunShadowThinPlaneBiasBlocks(1.0F, 1.0F) - 1.0F) < 1e-6F,
            "a thin plane at noon must be lifted by its own height");
    // 日出日落光线几乎垂直于薄片，本来就不自遮 ⇒ 偏置必须归零，否则那一档白付
    // peter-panning
    REQUIRE(shaderBias::sunShadowThinPlaneBiasBlocks(1.0F, 0.0F) == 0.0F,
            "a grazing sun must not lift a thin plane at all");
    // ★ 实心面**一格都不能抬**。给方块顶面加这一格会吃掉一格以内的全部接触阴影
    for (const float incidence : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
        REQUIRE(shaderBias::sunShadowThinPlaneBiasBlocks(0.0F, incidence) == 0.0F,
                "a solid face must never take the thin-plane bias");
    }

    // ---- 2. 它真的接进了比较参考值 -----------------------------------------
    // 遮挡物摆在接收点前方 0.001 个 NDC 深度单位 = 0.32 格。实心面的偏置只有 0.005 格，
    // 所以它**找得到**这个遮挡物并跑满四次 PCF；薄片抬了一整格（0.0031 个深度单位），
    // 参考值越过遮挡物 ⇒ 一次采样都不做。
    //
    // 数遮挡物搜索的结果而不是最终亮度：斑马纹的成因正是「草把自己判成了遮挡物」。
    const auto run = [](float thinPlane) {
        shaderReceiver::Samples samples{};
        samples.visibility = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        samples.blockerDepth = {0.499F, 0.499F, 0.499F, 0.499F};
        const float factor = shaderReceiver::sunShadowFactor(
            &samples, &samples, kMissNearCascade, glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F},
            glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather, thinPlane);
        return std::pair{factor, samples.count};
    };
    const auto [solidFactor, solidTaps] = run(kSolidFace);
    REQUIRE(solidTaps == 4 && solidFactor == 0.0F,
            "a solid face 0.32 blocks under a blocker must still be shadowed");
    const auto [plantFactor, plantTaps] = run(kThinPlane);
    REQUIRE(plantTaps == 0 && plantFactor == 1.0F,
            "a thin plane must not shadow itself: the same blocker is inside its own height");

    // ---- 3. 两张法线表逐位一致 ---------------------------------------------
    // 薄片这一档靠**下标**传递（顶点格式没有空位），所以两张表一旦错位，植物会拿到
    // 另一个方向的法线，而症状是「草的亮度变了」，读源码看不出来。
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    const std::string vertex = stripLineComments(readFile(shaderDir / "grass_block.vert"));
    const auto from = vertex.find("vec3 kVertexNormals[");
    REQUIRE(from != std::string::npos, "grass_block.vert must still declare the normal table");
    const auto to = vertex.find(");", from);
    REQUIRE(to != std::string::npos, "the normal table must be terminated");
    std::vector<float> numbers;
    {
        const std::string body = vertex.substr(from, to - from);
        std::size_t cursor = body.find('(');
        while ((cursor = body.find("vec3(", cursor)) != std::string::npos) {
            cursor += 5;
            const auto end = body.find(')', cursor);
            std::string triple = body.substr(cursor, end - cursor);
            for (char& c : triple) {
                if (c == ',') c = ' ';
            }
            std::istringstream in{triple};
            float value = 0.0F;
            while (in >> value) numbers.push_back(value);
            cursor = end;
        }
    }
    REQUIRE(numbers.size() == mc::render::kVertexNormals.size() * 3U,
            "the shader normal table has " + std::to_string(numbers.size() / 3U) +
                " entries, C++ has " + std::to_string(mc::render::kVertexNormals.size()));
    for (std::size_t i = 0; i < mc::render::kVertexNormals.size(); ++i) {
        const glm::vec3& expected = mc::render::kVertexNormals[i];
        REQUIRE(std::abs(numbers[i * 3U + 0U] - expected.x) < 1e-6F &&
                    std::abs(numbers[i * 3U + 1U] - expected.y) < 1e-6F &&
                    std::abs(numbers[i * 3U + 2U] - expected.z) < 1e-6F,
                "normal table entry " + std::to_string(i) + " differs between C++ and the shader");
    }
    // 薄片那个下标必须是**表尾那一项**，且它的方向与「朝上」逐位相同：它表达的是
    // 「几何是竖直的」，不是一个新方向
    REQUIRE(mc::render::kThinPlaneNormalIndex + 1U == mc::render::kVertexNormals.size(),
            "the thin-plane index must be the last entry of the normal table");
    REQUIRE(mc::render::kVertexNormals[mc::render::kThinPlaneNormalIndex] ==
                glm::vec3(0.0F, 1.0F, 0.0F),
            "the thin-plane entry must still shade as an upward face");
    // 而 nearestNormalIndex 永远取不到它——否则每一个朝上的面都会变成薄片，
    // 整个世界的顶面都会丢掉一格以内的接触阴影
    for (const glm::vec3& normal : mc::render::kVertexNormals) {
        REQUIRE(mc::render::nearestNormalIndex(normal) != mc::render::kThinPlaneNormalIndex,
                "nearestNormalIndex must never return the thin-plane index on its own");
    }
    // 三个采样者都得把这一档传下去。漏一个的症状是「有的草有斑马纹、有的没有」
    for (const char* name : {"grass_block.frag", "block_cutout.frag", "item_entity.frag"}) {
        const std::string source = stripLineComments(readFile(shaderDir / name));
        const auto call = source.find("sunShadowFactor(");
        REQUIRE(call != std::string::npos, std::string{name} + " must still call sunShadowFactor");
        const auto argsEnd = source.find(");", call);
        const std::string args = source.substr(call, argsEnd - call);
        const bool terrain = std::string{name} != "item_entity.frag";
        REQUIRE(args.find(terrain ? "fragmentThinPlane" : "0.0") != std::string::npos,
                std::string{name} + " must pass the thin-plane flag to sunShadowFactor");
    }
}

// RN-42：直射项相对水平面的权重。
void checkDirectWeight() {
    // 太阳仰角的余弦（= 朝上法线的入射角余弦）。正午接近 1，清晨接近 0。
    const float noonSun = 0.95F;
    const float morningSun = 0.20F;

    // ---- 1. 水平地面在任何时刻都是 1 -------------------------------------
    // ★ 这一条是整个公式的锚：时段的明暗 vanilla 已经用 skyLightFactor 表达过一遍，
    //   这里再乘一次 sin(仰角) 就是把一天的曲线算两遍，清晨傍晚会平白暗一倍
    for (const float sunUp : {0.05F, 0.20F, 0.50F, 0.95F, 1.0F}) {
        REQUIRE(std::abs(shaderBias::sunDirectWeight(sunUp, sunUp) - 1.0F) < 1e-6F,
                "a horizontal surface must keep full direct weight at every hour");
    }

    // ---- 2. 正午的竖直面归零 ---------------------------------------------
    // 竖直面的入射角余弦 = cos(方位差) x sin(太阳的天顶角)，正午那个因子约等于 0
    const float noonWallIncidence = std::sqrt(std::max(1.0F - noonSun * noonSun, 0.0F));
    REQUIRE(shaderBias::sunDirectWeight(noonWallIncidence, noonSun) < 0.35F,
            "a wall at noon must lose most of its direct light; that is what closes the step "
            "at the wall's foot");
    REQUIRE(shaderBias::sunDirectWeight(0.0F, noonSun) == 0.0F,
            "a face exactly edge-on to the sun takes no direct light");

    // ---- 3. 清晨朝向太阳的墙仍旧吃满 -------------------------------------
    // 低太阳时竖直面的入射角余弦远大于仰角余弦 ⇒ 比值 > 1 ⇒ 被 clamp 收到 1。
    // vanilla 那种「早上东面亮」的观感靠这一条保住
    const float morningWallIncidence = std::sqrt(std::max(1.0F - morningSun * morningSun, 0.0F));
    REQUIRE(std::abs(shaderBias::sunDirectWeight(morningWallIncidence, morningSun) - 1.0F) < 1e-6F,
            "a wall facing a low sun must still take full direct light");

    // ---- 4. 背对太阳的面是 0，而且有界 -----------------------------------
    REQUIRE(shaderBias::sunDirectWeight(-1.0F, noonSun) == 0.0F,
            "a back-facing surface takes no direct light");
    for (const float sunUp : {-1.0F, 0.0F, 1e-9F, 1.0F}) {
        for (const float incidence : {-1.0F, 0.0F, 0.5F, 1.0F, 2.0F}) {
            const float weight = shaderBias::sunDirectWeight(incidence, sunUp);
            REQUIRE(std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F,
                    "direct weight must stay finite and inside [0,1] even at the horizon");
        }
    }

    // ---- 5. 接进了 sunSkyFactor，而受光地面一个字没动 --------------------
    const float ground = shaderBias::sunSkyFactor(1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    REQUIRE(std::abs(ground - 1.0F) < 1e-6F, "受光的水平地面必须仍旧是 1.0");
    const float noonWall =
        shaderBias::sunSkyFactor(1.0F, 1.0F, 1.0F, 0.0F, 0.0F, noonWallIncidence, noonSun);
    REQUIRE(noonWall < 0.5F * ground,
            "正午的竖直面必须明显暗于地面——那正是这个节点买到的东西");
    // ★ 而且它不能低于散射那一份：竖直面丢的是直射，不是全部
    REQUIRE(noonWall >= shaderBias::kSkyAmbientFraction - 1e-6F,
            "a wall must never fall below the ambient share");

    // ---- 6. 散射份额收窄了，影子因此更暗 ---------------------------------
    REQUIRE(shaderBias::kSkyAmbientFraction < 0.2F,
            "RN-42 收窄了散射份额（实机反馈影子偏亮）");
    REQUIRE(shaderBias::kSkyAmbientFraction >= 0.10F,
            "低于晴空散射的实测下沿就不是物理量了，只是把影子涂黑");

    // ---- 7. 太阳落山后，直射那一份整份转给散射 ---------------------------
    // ★ 这一条是 RN-42 第一版漏掉的：夜里 sunUp < 0 ⇒ 直射权重是 0，而散射份额如果
    //   仍旧只有 0.15，整个夜晚的天光通道就会掉到从前的 15%。实测（离屏，tick 23000）
    //   平均亮度 12.1 → 7.0，是这条断言要钉死的东西。月光本来就没有方向。
    REQUIRE(shaderBias::sunPresence(-1.0F) == 0.0F && shaderBias::sunPresence(0.0F) == 0.0F,
            "the sun below the horizon must contribute no direct share");
    REQUIRE(std::abs(shaderBias::sunPresence(1.0F) - 1.0F) < 1e-6F &&
                std::abs(shaderBias::sunPresence(0.5F) - 1.0F) < 1e-6F,
            "a sun well above the horizon must give the full direct share");
    for (const float sunUp : {-1.0F, -0.5F, -0.01F, 0.0F}) {
        // 夜里：不管朝哪一面，天光通道必须是**满的**——与 RN-42 之前逐位相同
        for (const float incidence : {-1.0F, 0.0F, 0.5F, 1.0F}) {
            const float night =
                shaderBias::sunSkyFactor(1.0F, 1.0F, 0.0F, 0.0F, 0.0F, incidence, sunUp);
            REQUIRE(std::abs(night - 1.0F) < 1e-6F,
                    "night must keep the whole sky channel: the direct share transfers to "
                    "ambient exactly the way an overcast sky does");
        }
    }

    // ---- 8. 三个采样者都得算这个权重 -------------------------------------
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    for (const char* name : {"grass_block.frag", "block_cutout.frag", "item_entity.frag"}) {
        const std::string source = stripLineComments(readFile(shaderDir / name));
        // 三条路现在都走 sunSkyFactor，入射角与太阳高度是它的参数——手抄一份
        // 「环境 + 直射」正是 RN-38 到 RN-42 之间那条路上反复出问题的地方
        REQUIRE(source.find("sunSkyFactor(") != std::string::npos,
                std::string{name} + " must take its sky split from the shared function");
        REQUIRE(source.find("normalize(camera.sunDirection.xyz).y") != std::string::npos,
                std::string{name} + " must feed the sun elevation into the sky split");
        REQUIRE(source.find("kSkyAmbientFraction +") == std::string::npos,
                std::string{name} + " must not hand-copy the ambient/direct split");
    }
}

// RN-43：级联接缝的过渡带。
void checkCascadeBlend() {
    // ---- 1. 混合系数本身 ---------------------------------------------------
    REQUIRE(shaderBias::sunShadowCascadeBlend(0.0F, 0.0F) == 0.0F,
            "the middle of the near box must be pure near cascade");
    REQUIRE(std::abs(shaderBias::sunShadowCascadeBlend(1.0F, 0.0F) - 1.0F) < 1e-6F &&
                std::abs(shaderBias::sunShadowCascadeBlend(0.0F, -1.0F) - 1.0F) < 1e-6F,
            "the near box's own edge must be pure far cascade");
    REQUIRE(std::abs(shaderBias::sunShadowCascadeBlend(0.9F, 0.0F) - 0.5F) < 1e-6F,
            "the band must be linear across its width");
    // 对称、单调、有界
    float previous = -1.0F;
    for (const float edge : {0.0F, 0.5F, 0.8F, 0.85F, 0.9F, 0.95F, 1.0F, 2.0F}) {
        const float blend = shaderBias::sunShadowCascadeBlend(edge, 0.0F);
        REQUIRE(blend >= previous && blend >= 0.0F && blend <= 1.0F,
                "the blend must rise monotonically and stay inside [0,1]");
        REQUIRE(shaderBias::sunShadowCascadeBlend(-edge, 0.0F) == blend &&
                    shaderBias::sunShadowCascadeBlend(0.0F, edge) == blend,
                "the blend must be symmetric in both axes");
        previous = blend;
    }
    // 带宽：近段框半边长 8 格，两成就是 1.6 格。带太窄就化不开 8 倍的半影差，
    // 太宽就是让大半个近段白付两级的采样钱
    REQUIRE(shaderBias::kSunShadowCascadeBlendStart >= 0.6F &&
                shaderBias::kSunShadowCascadeBlendStart <= 0.9F,
            "the blend band must stay a rim of the near box");

    // ---- 2. 带内两级各采一次，结果落在两者之间 -----------------------------
    const auto run = [](float x) {
        shaderReceiver::Samples samples{};
        // 前四个 tap 是近段（全亮），后四个是远段（全暗）
        samples.visibility = {1, 1, 1, 1, 0, 0, 0, 0, 0};
        samples.blockerDepth = {0.499F, 0.499F, 0.499F, 0.499F,
                                0.499F, 0.499F, 0.499F, 0.499F};
        const float factor = shaderReceiver::sunShadowFactor(
            &samples, &samples, glm::mat4{1.0F}, glm::mat4{1.0F}, glm::vec3{x, 0, 0.5F},
            glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather, kSolidFace);
        return std::pair{factor, samples.count};
    };
    // 带外：只采近段
    const auto [insideFactor, insideTaps] = run(0.5F);
    REQUIRE(insideTaps == 4 && insideFactor == 1.0F,
            "a receiver well inside the near box must still cost four taps");
    // 带内（NDC 0.9 ⇒ 混合系数 0.5）：八个 tap，一半在层 0 一半在层 1
    const auto [bandFactor, bandTaps] = run(0.9F);
    REQUIRE(bandTaps == 8, "inside the band both cascades must be sampled");
    REQUIRE(std::abs(bandFactor - 0.5F) < 1e-6F,
            "the band must return the blend of the two cascades, not one of them");
    // ★ 0.5 那个点对「混反了」是瞎的（mix(1,0,0.5) 与 mix(0,1,0.5) 一样）。再取一个
    //   不对称的：NDC 0.85 ⇒ 系数 0.25 ⇒ 近段占四分之三
    const auto [quarterFactor, quarterTaps] = run(0.85F);
    REQUIRE(quarterTaps == 8 && std::abs(quarterFactor - 0.75F) < 1e-6F,
            "at a quarter of the way across the band the near cascade must still weigh three "
            "quarters; swapping the two ends of the mix reads identically at the midpoint");

    // 层号：前四个 tap 在层 0，后四个在层 1。混错层的症状是「接缝处影子整片错位」
    shaderReceiver::Samples samples{};
    samples.visibility = {1, 1, 1, 1, 0, 0, 0, 0, 0};
    samples.blockerDepth = {0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F};
    shaderReceiver::sunShadowFactor(&samples, &samples, glm::mat4{1.0F}, glm::mat4{1.0F},
                                    glm::vec3{0.9F, 0, 0.5F}, glm::vec3{0, 1, 0},
                                    glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather, kSolidFace);
    for (std::size_t tap = 0; tap < 4; ++tap) {
        REQUIRE(samples.coordinates.at(tap).z == 0.0F, "the first four taps must read layer 0");
        REQUIRE(samples.coordinates.at(tap + 4U).z == 1.0F,
                "the band's second four taps must read layer 1");
    }
    // 要混掉的那道跳变有多大：遮挡物远到两级都吃满上限（0.5 纹素）时，**世界尺度**的
    // 半影一个是 0.0039 格、一个是 0.031 格——正好 8 倍。uv 里两者反而一样宽，因为
    // 两级共用一张 2048 的图；差别全在一个纹素有多大
    const float saturatedNear = shaderBias::sunShadowPenumbraTexels(
                                    160.0F, shaderBias::kSunShadowNearTexelBlocks) *
                                shaderBias::kSunShadowNearTexelBlocks;
    const float saturatedFar = shaderBias::sunShadowPenumbraTexels(
                                   160.0F, shaderBias::kSunShadowFarTexelBlocks) *
                               shaderBias::kSunShadowFarTexelBlocks;
    REQUIRE(std::abs(saturatedFar / saturatedNear - 8.0F) < 1e-3F,
            "the seam this node blends away is an eightfold jump in penumbra width");
    // 而两级各按各的纹素算半影：把 texelBlocks 传错的症状是「带内的影子比带外更糊」
    shaderReceiver::Samples saturated{};
    saturated.visibility = {1, 1, 1, 1, 0, 0, 0, 0, 0};
    saturated.blockerDepth = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    shaderReceiver::sunShadowFactor(&saturated, &saturated, glm::mat4{1.0F}, glm::mat4{1.0F},
                                    glm::vec3{0.9F, 0, 0.5F}, glm::vec3{0, 1, 0},
                                    glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather,
                                    kSolidFace);
    REQUIRE(saturated.count == 8, "the saturated fixture must still take both cascades");
    const float nearSpan =
        std::abs(saturated.coordinates.at(1).x - saturated.coordinates.at(0).x);
    const float farSpan = std::abs(saturated.coordinates.at(5).x - saturated.coordinates.at(4).x);
    // 遮挡物**近**的时候两级都没吃满上限，半影是物理宽度，于是 uv 里远段反而窄
    // 八倍——这一条钉的是「远段那一次用的是远段自己的纹素」。忘了换 texelBlocks
    // 的症状是带内的影子比带外更糊，而源码读起来毫无异样
    const float closeNearSpan =
        std::abs(samples.coordinates.at(1).x - samples.coordinates.at(0).x);
    const float closeFarSpan = std::abs(samples.coordinates.at(5).x - samples.coordinates.at(4).x);
    REQUIRE(closeFarSpan < closeNearSpan * 0.25F && closeFarSpan > 0.0F,
            "the far cascade must size its penumbra with its own texel");
    REQUIRE(std::abs(nearSpan - farSpan) < 1e-9F && nearSpan > 0.0F,
            "both cascades saturate at half a texel of the same 2048 map, so their uv footprints "
            "match; the eightfold difference lives in the texel's world size");

    // ---- 3. 近段关掉时不混 -------------------------------------------------
    shaderReceiver::Samples off{};
    off.visibility = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    off.blockerDepth = {0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F, 0.499F};
    shaderReceiver::sunShadowFactor(&off, &off, glm::mat4{1.0F}, glm::mat4{1.0F},
                                    glm::vec3{0.9F, 0, 0.5F}, glm::vec3{0, 1, 0},
                                    glm::vec3{0, 1, 0}, kNearCascadeOff, kClearWeather,
                                    kSolidFace);
    REQUIRE(off.count == 4, "with the near cascade off there is nothing to blend with");
}

void checkContactHardening() {
    const auto run = [](float blockerDepth) {
        shaderReceiver::Samples samples{};
        samples.visibility = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        samples.blockerDepth = {blockerDepth, blockerDepth, blockerDepth, blockerDepth};
        const float factor = shaderReceiver::sunShadowFactor(
            &samples, &samples, kMissNearCascade, glm::mat4{1.0F}, glm::vec3{0, 0, 0.5F},
            glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, kNearCascadeOn, kClearWeather, kSolidFace);
        return std::pair{factor, samples};
    };
    const auto tapSpreadTexels = [](const shaderReceiver::Samples& samples) {
        float minimum = 1.0F;
        float maximum = -1.0F;
        for (std::size_t i = 0; i < samples.count; ++i) {
            minimum = std::min(minimum, samples.coordinates[i].x);
            maximum = std::max(maximum, samples.coordinates[i].x);
        }
        return (maximum - minimum) * 2048.0F;   // 回到纹素
    };

    // ① 一个遮挡物都没有 ⇒ 全亮，而且**一次 PCF 都不做**。
    //    这条提前返回是接触硬化的性能来源：受光的地面是画面的大头，它们现在四次搜索
    //    之后就直接返回，省掉四次 PCF。少了这条，接触硬化就是净增开销。
    {
        const auto [factor, samples] = run(1.0F);
        REQUIRE(factor == 1.0F && samples.count == 0,
                "a receiver with no blocker in the search radius must return 1 without any PCF tap;"
                " factor=" + std::to_string(factor) + ", taps=" + std::to_string(samples.count));
        REQUIRE(samples.blockerCount == 4,
                "the blocker search itself must always run its four taps");
    }

    // ② 遮挡物很远（深度 0 = 光源那一侧，离接收面约 160 格）⇒ 半影吃满上限，
    //    tap 展开 2 x 0.5 = 1 纹素，与接触硬化之前逐位相同。远处的影子不该被改动。
    const auto [farFactor, farSamples] = run(0.0F);
    REQUIRE(farSamples.count == 4, "a shadowed receiver must still take four PCF taps");
    const float farSpread = tapSpreadTexels(farSamples);
    REQUIRE(std::abs(farSpread - 2.0F * shaderBias::kSunMaxPenumbraTexels) < 1e-3F,
            "a distant blocker must saturate the penumbra at the old fixed radius; spread=" +
                std::to_string(farSpread));
    // RN-38：完全被挡住 ⇒ 可见度 0。影子里剩下的那一份是天空散射，由 sunSkyFactor
    // 从 kSkyAmbientFraction 算，不再烘在这里
    REQUIRE(farFactor == 0.0F, "fully occluded receiver must report zero visibility");

    // ③ 遮挡物就在脚下（0.05 格）⇒ 半影收到近乎 0，这正是墙根那一段。
    const float contactDepth = 0.5F - 0.05F / 319.9F;
    const auto [contactFactor, contactSamples] = run(contactDepth);
    REQUIRE(contactSamples.count == 4, "the contact case must still sample, just tightly");
    const float contactSpread = tapSpreadTexels(contactSamples);
    REQUIRE(contactSpread < 0.05F,
            "a blocker 0.05 blocks away must collapse the PCF radius to near zero; spread=" +
                std::to_string(contactSpread) + " texels");
    REQUIRE(contactSpread < farSpread * 0.1F,
            "contact and distance must give materially different radii, or nothing was hardened");
    REQUIRE(contactFactor == 0.0F, "hardening must not brighten a fully occluded receiver");

    // ④ 单调、有界。中间那一档也要真的落在中间，否则上面两条对一个「非 0 即满」的
    //    实现也会成立。
    const float halfDepth = 0.5F - 1.0F / 319.9F;      // 遮挡物在 1 格外
    const auto [_, halfSamples] = run(halfDepth);
    const float halfSpread = tapSpreadTexels(halfSamples);
    REQUIRE(halfSpread > contactSpread && halfSpread < farSpread,
            "the penumbra must grow with blocker distance rather than switch between two values; "
            "contact=" + std::to_string(contactSpread) + " one-block=" + std::to_string(halfSpread) +
            " far=" + std::to_string(farSpread));
    // 逐值：半影 = 距离 x tan(太阳视角半径) / 纹素尺寸，上限 kSunMaxPenumbraTexels。
    for (const float blocks : {0.0F, 0.5F, 1.0F, 2.0F, 4.0F}) {
        const float wanted = std::min(blocks * shaderBias::kSunPenumbraTangent /
                                          shaderBias::kSunShadowFarTexelBlocks,
                                      shaderBias::kSunMaxPenumbraTexels);
        REQUIRE(std::abs(shaderBias::sunShadowPenumbraTexels(blocks, shaderBias::kSunShadowFarTexelBlocks) - wanted) < 1e-6F,
                "penumbra golden value mismatch at " + std::to_string(blocks) + " blocks");
    }
    REQUIRE(shaderBias::sunShadowPenumbraTexels(-1.0F, shaderBias::kSunShadowFarTexelBlocks) == 0.0F &&
            shaderBias::sunShadowPenumbraTexels(1e6F, shaderBias::kSunShadowFarTexelBlocks) == shaderBias::kSunMaxPenumbraTexels,
            "penumbra must clamp on both ends (negative distances come from roundoff)");
}

void checkEntityWiring() {
    const auto world = stripLineComments(readFile(MC_REBEDROCK_WORLD_RENDERER_SRC));
    const auto renderer = stripLineComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    const auto record =
        functionBody(world, "void recordShadow(FrameContext& frame, std::size_t cascade)");
    REQUIRE(record.find("selectSunShadowSceneCasters(") != std::string::npos &&
            record.find("pipelines.entityShadowPipelines[cascade]") != std::string::npos &&
            record.find("sizeof(draw.push), &draw.push") != std::string::npos &&
            record.find("draw.vertexCount, 1, draw.firstVertex") != std::string::npos,
            "shadow pass must select and submit the collected entity geometry");
    // RN-37：玻璃的阴影几何与镂空地形共用**同一条**管线（同一个顶点程序、同一次
    // alpha 测试），所以它们必须在一次绑定里画完；分两次调用是白切一次管线。
    // 顶点则与半透明层共用——两层的 vertexOffset 必须是同一个值，写错的症状是
    // 玻璃的影子长在别的方块上。
    REQUIRE(record.find("{&GpuMesh::cutout, &GpuMesh::translucentShadow}") != std::string::npos,
            "the glass shadow geometry must ride the cutout pipeline's single bind");
    REQUIRE(record.find("mesh.translucentShadow.indexCount == 0U") != std::string::npos,
            "a section whose only caster is glass must still pass the caster filter");
    const auto upload = functionBody(world, "void uploadRenderMesh(");
    REQUIRE(upload.find("destination.translucentShadow.vertexOffset = vertexOffset;") !=
                    std::string::npos &&
                upload.find("destination.translucent.vertexOffset = vertexOffset;") !=
                    std::string::npos,
            "both layers must take the same vertex offset: the shadow layer is indices only");
    REQUIRE(upload.find("translucentShadowIndices.size() * sizeof(std::uint32_t)") !=
                    std::string::npos &&
                upload.find("translucentShadowIndices.data()") != std::string::npos,
            "the shadow layer must upload indices and nothing else — copying its vertices is the "
            "expensive way to get the same picture");

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
    // RN-35：一级一条管线，特化常量从 bool 变成 int（-1 = 主通道）
    for (const auto token : {"entity_shadow.frag.spv", "item_entity.vert.spv",
             "entityShadowPipelines[cascade]", "cascadeStages[0].pSpecializationInfo",
             "vertexBindingDescriptionCount = 0", "vertexAttributeDescriptionCount = 0",
             "rasterization.cullMode = VK_CULL_MODE_NONE", "push.size = sizeof(ItemPush)"})
        REQUIRE(pipeline.find(token) != std::string::npos,
                std::string{"entity depth pipeline contract missing: "} + token);
    const std::filesystem::path shaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
    const auto vertex = stripLineComments(readFile(shaderDir / "item_entity.vert"));
    // 主通道**不传**特化信息，所以默认值必须是「不是阴影趟」的那一档。
    // 从 bool 改成 int 之后这一条更要紧：一个默认为 0 的 int 会让主通道去投近段的光源矩阵
    REQUIRE(vertex.find("layout(constant_id = 0) const int sunShadowCascade = -1") !=
                    std::string::npos &&
                vertex.find("const bool sunShadowPass = sunShadowCascade >= 0") != std::string::npos,
            "world item vertex shader must default to color projection");
    std::size_t count = 0, offset = 0;
    while ((offset = vertex.find("? camera.lightViewProj[max(sunShadowCascade, 0)] * vec4(worldPosition, 1.0)",
                                 offset)) != std::string::npos) {
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
            sampling.find("sunShadowNormalOffsetBlocks(incidence, texelBlocks)") != std::string::npos,
            "production shadow sampling must use the exact GLSL offset function compiled by the headless test");
    // ★ 抬高必须加在**投影之前**。加在之后就退化成沿光线方向的位移，也就是旧的深度偏置，
    // 亮边原样回来——而这是一处只差一行位置的错误，源码读起来毫无异样。
    //
    // RN-35：两级各抬一次。抬的量是**被选中那一级**的纹素，而纹素逐级差 8 倍——
    // 拿远段的抬升去投近段，影子会整片从脚下浮起来。所以「先选级、再抬、再投」这个
    // 顺序在两条分支上都要成立，两条各钉一次
    REQUIRE(sampling.find("offsetPosition = worldPosition + normal * "
                          "sunShadowNormalOffsetBlocks(incidence, texelBlocks)") !=
                    std::string::npos &&
            sampling.find("(tryNear ? lightViewProjNear : lightViewProjFar) * "
                         "vec4(offsetPosition, 1.0)") != std::string::npos &&
            sampling.find("lightViewProjFar * vec4(offsetPosition, 1.0)") != std::string::npos &&
            sampling.find("* vec4(worldPosition, 1.0)") == std::string::npos,
            "the normal offset must be applied to the world position before projection");
    // 每一级都必须先把 texelBlocks 换成自己那一档，再算抬升。漏掉第二次赋值时源码
    // 仍然「看起来对」——两条分支的文字几乎一样
    REQUIRE(sampling.find("texelBlocks = sunShadowTexelBlocks(cascade);") != std::string::npos,
            "each cascade must re-read its own texel size before lifting along the normal");
    REQUIRE(sampling.find("float tapReference = reference + sunShadowTapOffsetBlocks(") != std::string::npos &&
            sampling.find("tapReference));") != std::string::npos,
            "PCF must compare each tap against its receiver-plane depth");
}

} // namespace

int main() {
    try {
        checkShadowFacing();
        checkEntityCasters();
        checkCascades();
        checkWeatherResponse();
        checkBias();
        checkContactHardening();
        checkThinPlaneBias();
        checkDirectWeight();
        checkCascadeBlend();
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
