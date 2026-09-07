// 一个 SkeletonPose 什么时候可以被查询。
//
// 起点是一个真实崩溃：`tools/export_block_preview.sh <block> --sun-shadows`
// （不带 `--shadow-entities`）在写出第一张图之前 SIGSEGV。根因不在阴影里——
// `SkeletonPose::worldMatrix` 解引用了空的 `model_`。动画器**先**持有模型、**后**
// 才第一次 evaluate，中间那段时间「模型有骨骼」为真而「姿态可查询」为假；两个渲染
// 消费者都用前者驱动循环、用后者取矩阵，于是任何绕过了那次 evaluate 的路径都会拿
// 一个非零骨骼数去索引空姿态。隐藏导出正是这样一条路径：它直接调 drawFrame，不走
// 主循环每帧的 updateWorldPlayer。
//
// 所以这个测试钉两件事：① `bound()` 是那个区分状态的唯一答案，且「模型非空 + 姿态
// 未绑定」确实可达（不是理论状态）；② 渲染侧读姿态的地方恰好是那两处，且都问了它。
// 第二条是源码护栏：渲染器本身不是 headless 可跑的，守卫被删掉不会有任何运行期断言
// 变红，只有读源码能拦住。

#include "animation/PlayerModelAnimator.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif

namespace {

const std::filesystem::path kSourceDir{MC_REBEDROCK_SOURCE_DIR};

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    assert(stream && "source file must be readable");
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 注释里提到 `bound()` 不算数——护栏要的是调用，不是一句话。
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

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

int main() {
    using mc::animation::PlayerModelAnimator;
    using mc::animation::SkeletonPose;

    // ---- 契约本身 --------------------------------------------------------
    const SkeletonPose empty;
    assert(!empty.bound());
    assert(empty.model() == nullptr);
    assert(empty.boneCount() == 0);

    // ---- 「模型有骨骼、姿态未绑定」是可达状态，不是理论状态 --------------
    // 这是崩溃的前置条件，也是这条测试存在的全部理由：把它写成断言，任何让
    // 构造后就顺带 evaluate 一次的改动都会在这里现形（那也是一种修法，但它得
    // 是被选中的修法，不是被默默滑进来的）。
    PlayerModelAnimator fresh;
    assert(fresh.model().boneCount() > 0);
    assert(!fresh.skeletonPose().bound());
    assert(fresh.skeletonPose().boneCount() == 0);

    // ---- 第一次 evaluate 之后姿态才可查询 -------------------------------
    // 世界玩家与背包预览各走一条入口，两条都必须把姿态绑上。
    PlayerModelAnimator worldPlayer;
    worldPlayer.updateWorldPlayer(0.0F, 0.0F, 0.0F, 0.0F, false);
    assert(worldPlayer.skeletonPose().bound());
    assert(worldPlayer.skeletonPose().boneCount() == worldPlayer.model().boneCount());
    assert(worldPlayer.skeletonPose().model() == &worldPlayer.model());

    PlayerModelAnimator preview;
    preview.update(0.0F, false);
    assert(preview.skeletonPose().bound());
    assert(preview.skeletonPose().boneCount() == preview.model().boneCount());

    // 绑定之后每根骨骼都能取到矩阵，且不再触碰空模型。
    for (std::size_t index = 0; index < worldPlayer.skeletonPose().boneCount(); ++index) {
        const glm::mat4 world = worldPlayer.skeletonPose().worldMatrix(static_cast<int>(index));
        assert(world[3][3] == 1.0F);
    }

    // ---- 源码护栏：读姿态的地方恰好是这些，且都问了 bound() -------------
    // 渲染器不进 headless 测试，守卫被删掉不会有运行期断言变红。
    std::vector<std::filesystem::path> readers;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{kSourceDir / "src"}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension();
        if (extension != ".hpp" && extension != ".cpp") {
            continue;
        }
        const std::string body = stripComments(readFile(entry.path()));
        if (contains(body, "skeletonPose()")) {
            readers.push_back(entry.path());
        }
    }

    // 访问器自身 + 两个渲染消费者。第三个消费者出现时这里会红，写它的人因此会
    // 被带到上面那段契约前面，而不是复制一遍「用模型的骨骼数循环」。
    std::size_t accessors = 0;
    std::size_t consumers = 0;
    for (const auto& path : readers) {
        const std::string name = path.filename().string();
        const std::string body = stripComments(readFile(path));
        if (name == "PlayerModelAnimator.hpp") {
            ++accessors;
            continue;
        }
        if (name != "WorldRenderer.hpp" && name != "HudRenderer.hpp") {
            std::cerr << "unexpected SkeletonPose consumer: " << path.string()
                      << "\n  it must ask pose.bound() before indexing the model's bones; "
                         "then add it to this list.\n";
            assert(false && "a new SkeletonPose consumer appeared without a bound() guard");
        }
        // 守卫的形状可以变（早退、循环上界、三元），调用不能没有。
        if (!contains(body, "bound()")) {
            std::cerr << "SkeletonPose consumer without a bound() guard: " << path.string()
                      << "\n  the model's boneCount() is non-zero before the first evaluate; "
                         "driving the loop with it indexes an unbound pose.\n";
            assert(false && "SkeletonPose consumer dropped its bound() guard");
        }
        ++consumers;
    }
    assert(accessors == 1);
    assert(consumers == 2);

    std::cout << "skeleton_pose_binding ok\n";
    return 0;
}
