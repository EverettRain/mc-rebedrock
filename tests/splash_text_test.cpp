// UI-4：主菜单 splash 的纯逻辑（GUI spec §6.3 / 26.1 `SplashRenderer`）。
//
// ★ 姿态常量取自源码，不是 spec §6.3 的正文。spec 那里的锚点 `W/2 + 90` 与
// `scale = 1.8 - abs(sin(t/100)*0.1) * 0.5` 都是旧值（UI-2 的更正表 D 已登记）。

#include "ui/SplashText.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("splash_text_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// --- 1. 姿态常量 -------------------------------------------------------------
void testConstants() {
    CHECK(mc::ui::kSplashAnchorX == 123.0F);   // WIDTH_OFFSET，不是 spec 的 90
    CHECK(mc::ui::kSplashAnchorY == 69.0F);    // HEIGH_OFFSET
    // TEXT_ANGLE = -PI/9 = -20°；负号让文字**向右上**扬起，与 vanilla 一致
    CHECK(std::fabs(mc::ui::kSplashRotation + 3.14159265F / 9.0F) < 1.0e-5F);
    CHECK(mc::ui::kSplashRotation < 0.0F);
    CHECK(mc::ui::kSplashTextOffsetY == -8.0F);
}

// --- 2. 切行 -----------------------------------------------------------------
void testParsing() {
    const auto lines = mc::ui::parseSplashes("As seen on TV!\nAwesome!\n\n  100% pure!  \n");
    CHECK(lines.size() == 3U);
    CHECK(lines[0] == "As seen on TV!");
    CHECK(lines[1] == "Awesome!");
    CHECK(lines[2] == "100% pure!");   // 首尾空白被去掉

    // CRLF 也要认，否则每一行末尾都会多一个不可见字符
    const auto crlf = mc::ui::parseSplashes("one\r\ntwo\r\n");
    CHECK(crlf.size() == 2U && crlf[0] == "one" && crlf[1] == "two");

    // 空文件、纯空行、注释行
    CHECK(mc::ui::parseSplashes("").empty());
    CHECK(mc::ui::parseSplashes("\n\n\n").empty());
    CHECK(mc::ui::parseSplashes("# not a splash\nreal\n").size() == 1U);

    // 没有结尾换行的最后一行不该被吞掉
    const auto tail = mc::ui::parseSplashes("only");
    CHECK(tail.size() == 1U && tail[0] == "only");
}

// --- 3. 选行：同一个种子必须给出同一行 ---------------------------------------
//
// 这条直接关系到截图通道那句"两遍逐字节相同"：选行只要不是种子的函数，
// 主菜单的截图就不可能稳定。
void testChoice() {
    const std::vector<std::string> lines{"a", "b", "c", "d"};
    for (std::uint64_t seed = 0; seed < 50; ++seed) {
        CHECK(mc::ui::chooseSplash(lines, seed) == mc::ui::chooseSplash(lines, seed));
    }
    // 不同种子应当能取到不止一行，否则"随机"是假的
    bool varied = false;
    for (std::uint64_t seed = 0; seed < 50; ++seed) {
        if (mc::ui::chooseSplash(lines, seed) != mc::ui::chooseSplash(lines, 0)) {
            varied = true;
            break;
        }
    }
    CHECK(varied);
    // 空表不该炸，也不该返回悬垂引用
    CHECK(mc::ui::chooseSplash({}, 12345U).empty());
}

// --- 4. 脉动缩放 -------------------------------------------------------------
void testScale() {
    // scale = (1.8 - |sin(2*PI*phase)| * 0.1) * 100 / (宽 + 32)
    // phase = 0 时 sin = 0，缩放取最大值 1.8 * 100 / (宽+32)
    const float wide = mc::ui::splashScale(68.0F, 0U);
    CHECK(std::fabs(wide - 1.8F * 100.0F / 100.0F) < 1.0e-4F);
    // phase = 0.25 时 |sin| = 1，缩放取最小值 1.7 * 100 / (宽+32)
    const float pulsed = mc::ui::splashScale(68.0F, 250U);
    CHECK(std::fabs(pulsed - 1.7F) < 1.0e-4F);
    CHECK(pulsed < wide);
    // 只有毫秒的千分之一周期有意义：1000ms 与 0ms 同相
    CHECK(std::fabs(mc::ui::splashScale(68.0F, 1000U) - wide) < 1.0e-5F);
    CHECK(std::fabs(mc::ui::splashScale(68.0F, 123456U) -
                    mc::ui::splashScale(68.0F, 456U)) < 1.0e-5F);
    // 越长的字缩得越小——这就是那个 100/(宽+32) 存在的理由
    CHECK(mc::ui::splashScale(200.0F, 0U) < mc::ui::splashScale(68.0F, 0U));
    // 宽度为零也不该除零
    CHECK(mc::ui::splashScale(0.0F, 0U) > 0.0F);
}

} // namespace

int main() {
    testConstants();
    testParsing();
    testChoice();
    testScale();
    if (failures != 0) {
        std::printf("splash_text_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
