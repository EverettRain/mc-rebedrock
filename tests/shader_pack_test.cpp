// RN-20f-0 — 光影包的边界：不开包必须回到 vanilla 口径
//
// 这个测试守的是一条**口径**不变量，不是一个数值。
//
// 立包之前，仓库里同时住着两套判据：「与 26.1 逐值对齐」（本作很多测试的真相源）
// 与「物理上更对」（RN-11 起的自研光照）。两者搅在同一条代码路径上，后果是可量的：
// 即使 `sunShadows` 默认为 false，`sunSkyFactor` 仍无条件把天光拆成直射与散射并乘上
// 入射角，于是正南/正北的墙全天只有 vanilla 的 42.5%、正东/正西的墙正午只有 20.1%。
//
// 出图侧的判据（容器里跑过，见 docs 的 RN-20f-0）：
//   * 开包：改动前后**逐字节相同**——这一轮不改变任何已开启的行为；
//   * 不开包：同一场景两个时刻的逐像素比值落进**单一个桶**（1.0×，100%）。
//     那正是 vanilla 的性质——天光只有一个总量，各面之间的比由 cardinalShade 定死、
//     与太阳方位无关。开包时同样的比值分裂成两个桶（1.0 与 2.5），入射角在起作用。
//
// 这里守的是那两条判据背后的**接线**：出图要 GPU，进不了 ctest；而接线一旦断了，
// 出图那两条就都不再成立，且没有任何东西会红（HANDOFF §5.3）。

#include "render/ShaderPack.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif
#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
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

[[nodiscard]] std::size_t countOf(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// ---- 1. 包本身的不变量 -----------------------------------------------------
//
// 两位永远同开同关。理由在 ShaderPack.hpp 抬头：阴影与直射模型不是两个特性，
// `sunSkyFactor` 消费 `shadowFactor`，而 `shadowFactor` 只有在天光分了直射项之后
// 才有东西可挡。分开开关会造出「有阴影但天光不分项」这种没有意义的组合。
void testPackInvariants() {
    using namespace mc::render;
    check(kNoShaderPack.consistent() && kBuiltinShaderPack.consistent(),
          "两个包的两位都必须同开同关");
    check(!kNoShaderPack.enabled(), "不开包就是什么都不加——它是逐值对齐 26.1 的那条基线");
    check(kBuiltinShaderPack.enabled(), "内置包要真的开着点什么，否则它不是一个包");
    // 穷尽：consistent() 只允许两种状态，而我们恰好有两个包。多一个包就要多一条断言，
    // 这正是「边界」这件事该有的形状——它不该悄悄长出第三种口径。
    check(kNoShaderPack.enabled() != kBuiltinShaderPack.enabled(),
          "两个包必须落在 consistent() 允许的两种状态上，一边一个");
    check(!kNoShaderPack.name.empty() && !kBuiltinShaderPack.name.empty(),
          "包要有名字，日志与诊断按它区分");
}

// ---- 2. 接收端：那一位真的门控着整套模型 -----------------------------------
void testShaderTakesThePackBit() {
    const std::filesystem::path shaders{MC_REBEDROCK_SHADER_SRC_DIR};
    const std::string bias = readFile(shaders / "include/sun_shadow_bias.glsl");
    check(!bias.empty(), "读得到 sun_shadow_bias.glsl");

    // 早退必须在**这个函数里**，不是在三个调用点各写一遍——三份手抄正是这条链子上
    // 反复出问题的形状（RN-51 的法线表就是因此搬进共享 include 的）
    check(bias.find("float sunSkyFactor(") != std::string::npos &&
              bias.find("float submergedBlocks, float shaderPack)") != std::string::npos,
          "sunSkyFactor 必须收下光影包这一位");
    check(bias.find("if (shaderPack < 0.5F) {\n        return skyLightFactor * weatherDimming;") !=
              std::string::npos,
          "不开包时它必须原样退回 vanilla 的 SKY_LIGHT_FACTOR x 天气昏暗，一项不多");

    // ★ 会错的那个量：**有没有采样者绕过那一位**。加第四个采样者却忘了传，
    //   出图那两条判据就都不再成立，而 GLSL 编译器只会在参数个数对不上时才说话——
    //   传一个字面量 1.0 是编译得过的，那正是这条断言要抓的形态。
    std::size_t callSites = 0;
    for (const char* frag : {"grass_block.frag", "block_cutout.frag", "item_entity.frag"}) {
        const std::string source = readFile(shaders / frag);
        check(!source.empty(), frag);
        const std::size_t calls = countOf(source, "sunSkyFactor(");
        const std::size_t passes = countOf(source, "camera.lightingSettings.w);");
        check(calls > 0, "这三个 frag 都该在调用 sunSkyFactor");
        check(calls == passes,
              "每一处 sunSkyFactor 调用都必须把 camera.lightingSettings.w 传进去");
        callSites += calls;
    }
    check(callSites == 3, "今天恰好三个采样者；多一个就要在这里加一行，而不是默默漏掉");
}

// ---- 3. 发送端：那一位与帧图的 pass 集合同源 --------------------------------
//
// 从前这两件事各自读一次 `shadowDisabled`，而接收端那条公式**压根没读**。
// 同源是结构性的保证：帧图里没有阴影步时，着色器不可能还在跑那套模型。
void testRendererDrivesBothSidesFromThePack() {
    const std::filesystem::path root{MC_REBEDROCK_SOURCE_DIR};
    const std::string renderer = readFile(root / "src/render/vulkan/VulkanRenderer.cpp");
    check(!renderer.empty(), "读得到 VulkanRenderer.cpp");

    check(renderer.find("const render::ShaderPack& activeShaderPack() const noexcept") !=
              std::string::npos,
          "本帧用哪个包必须只有一个来源");
    check(renderer.find(
              "uniform.lightingSettings.w = activeShaderPack().directSkyModel ? 1.0F : 0.0F;") !=
              std::string::npos,
          "接收端那一位从包来，不是再读一次 shadowDisabled");
    check(renderer.find("activeShaderPack().sunShadowPasses && options.cascadedShadows") !=
              std::string::npos,
          "近段级联那一步也从包来");
    // 帧图里两条阴影步与它们的边界屏障：三处都要从包取。漏一处的形态是
    // 「图里剪掉了那一步、屏障却还在」——那条 oldLayout 会对不上，只有校验层会说话。
    check(countOf(renderer, "activeShaderPack().sunShadowPasses") >= 4U,
          "远段那一步、两处屏障/录制的 drawn 判断都要从包取");
}

} // namespace

int main() {
    testPackInvariants();
    testShaderTakesThePackBit();
    testRendererDrivesBothSidesFromThePack();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "shader_pack_test ok\n";
    return 0;
}
