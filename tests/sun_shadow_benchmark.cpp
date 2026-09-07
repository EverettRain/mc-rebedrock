// RN-11b：Release 下同场景比较 RN-11 的地形选择与新增的地形+实体选择。
// 只量 CPU 几何准备/选择，不包含动画求值、Vulkan 命令记录或 GPU 光栅化，不推算 FPS。
#include "render/EntityRenderDraws.hpp"
#include <glm/ext/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {
std::size_t checksum = 0;
template<class F> double medianMicros(F&& work) {
    std::array<double, 21> samples{};
    for (auto& sample : samples) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) work();
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 100;
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}
}
int main() {
    using namespace mc::render;
    const glm::vec3 sun = glm::normalize(glm::vec3{.7F, 1, .28F});
    const auto matrix = sunShadowLightViewProj(sun, {0, 70, 0});
    std::vector<Aabb> terrain;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 p{static_cast<float>(i % 50) - 25, 70, static_cast<float>(i / 50) - 20};
        terrain.push_back({p, p + glm::vec3{1}});
    }
    std::vector<std::size_t> selectedTerrain, selectedEntities;
    const double baseline = medianMicros([&] {
        selectSunShadowCasters(matrix, sun, terrain, selectedTerrain);
        checksum += selectedTerrain.size();
    });
    std::printf("RN-11 terrain selection baseline: %.3f us (2000 candidates, 512 selected)\n", baseline);
    for (const std::size_t count : {0U, 16U, 128U, 512U, 1024U}) {
        EntityRenderDraws scene;
        const auto prepare = [&] {
            scene.clear();
            for (std::size_t e = 0; e < count; ++e) {
                scene.begin(e == 0 ? ShadowEntityKind::Player : ShadowEntityKind::Creature);
                for (int bone = 0; bone < 8; ++bone) {
                    ItemPush push{};
                    push.data.x = kItemModeWorldMatrixCuboid;
                    push.dimensions = {.5F, .5F, .5F, 0};
                    push.viewModelTransform = glm::translate(glm::mat4{1},
                        {static_cast<float>(e % 32) - 16, 71 + static_cast<float>(bone) * .15F,
                         static_cast<float>(e / 32) - 16});
                    scene.append(push, 36);
                }
            }
        };
        prepare(); // 预热容量，不把启动分配算进稳态
        const double preparation = medianMicros([&] { prepare(); checksum += scene.draws.size(); });
        const double selection = medianMicros([&] {
            selectSunShadowSceneCasters(matrix, sun, terrain, scene.casters, selectedTerrain, selectedEntities);
            checksum += selectedTerrain.size() + selectedEntities.size();
        });
        std::printf("entities=%zu draws=%zu selected=%zu: prepare=%.3f us select=%.3f us vs RN-11=%+.2f%%\n",
            count, scene.draws.size(), selectedEntities.size(), preparation, selection,
            (selection / baseline - 1) * 100);
    }
    std::printf("checksum=%zu\n", checksum);
}
