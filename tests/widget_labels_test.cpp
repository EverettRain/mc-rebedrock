// ui/WidgetLabels.hpp 是每个 WidgetId 的标签来源分类表
//
// 覆盖性，也就是每个 id 恰好归一类，已经由该头文件里的 static_assert 在编译期保证
// 把这个头 include 进本测试，就等于在又一个编译单元里跑了一遍那道检查
// 这里补的是 static_assert 表达不了、或者表达出来读不懂的那部分
// 即表里有没有重复行、有没有空键，以及分类是否与它声称的语义一致
//
// 这张表替掉的是 widgetLabel() 里二十来个 return translated(k, f); 分支
// 那些分支是纯数据，却因为待在 switch 里而无法被遍历，也就无法被这样检查

#include "ui/WidgetLabels.hpp"

#include <cassert>
#include <cstdint>
#include <set>
#include <string_view>

namespace {

using mc::ui::findCyclingOption;
using mc::ui::findStaticLabel;
using mc::ui::hasRuntimeLabel;
using mc::ui::isUnlabelled;
using mc::ui::kRuntimeWidgetLabels;
using mc::ui::kStaticWidgetLabels;
using mc::ui::kUnlabelledWidgets;
using mc::ui::WidgetId;

// 表行不得重复
// 同一个 id 出现两次时，改其中一行会静默失效，因为查找命中的永远是第一行
void testNoDuplicateRows() {
    std::set<WidgetId> seen;
    for (const auto& row : kStaticWidgetLabels) {
        assert(seen.insert(row.id).second);
    }
    seen.clear();
    for (const WidgetId id : kRuntimeWidgetLabels) {
        assert(seen.insert(id).second);
    }
    seen.clear();
    for (const WidgetId id : kUnlabelledWidgets) {
        assert(seen.insert(id).second);
    }
}

// 每个静态标签都得真的有键和兜底文本
// 兜底文本是硬要求，缺这个键的语言若拿到空串，按钮就成了一个没有文字的框
void testStaticLabelsAreComplete() {
    for (const auto& row : kStaticWidgetLabels) {
        assert(row.id != WidgetId::None);
        assert(!row.key.empty());
        assert(!row.fallback.empty());
    }
}

// 后缀是数据而不是特例分支
// vanilla 的 selectWorld.experimental 只有 "Experimental"，省略号由这一列拼上
// 目前只有它一行用到
void testSuffixIsDataNotASpecialCase() {
    const auto* experimental = findStaticLabel(WidgetId::Experimental);
    assert(experimental != nullptr);
    assert(experimental->suffix == std::string_view{"..."});

    std::size_t withSuffix = 0;
    for (const auto& row : kStaticWidgetLabels) {
        if (!row.suffix.empty()) {
            ++withSuffix;
        }
    }
    assert(withSuffix == 1U);
}

// 分类互斥，且运行期那一档确实不在静态表里
// 否则渲染器算出来的实时文本会被静态表先一步拦截返回，症状是分辨率按钮永远显示同一个数字
void testClassesDoNotOverlap() {
    for (std::uint16_t raw = 0; raw < static_cast<std::uint16_t>(WidgetId::Count); ++raw) {
        const auto id = static_cast<WidgetId>(raw);
        const int sources = (findCyclingOption(id) != nullptr ? 1 : 0) +
                            (findStaticLabel(id) != nullptr ? 1 : 0) +
                            (hasRuntimeLabel(id) ? 1 : 0) + (isUnlabelled(id) ? 1 : 0);
        assert(sources == 1);
    }
    for (const WidgetId id : kRuntimeWidgetLabels) {
        assert(findStaticLabel(id) == nullptr);
        assert(findCyclingOption(id) == nullptr);
    }
}

// 循环选项的标签只能来自 OptionCycle 的表行，它们的文本和点击时的步进读的是同一行
// 一旦有人把某个循环选项也写进静态表，按钮就会显示一个不随点击变化的文本
void testCyclingOptionsKeepTheirOwnTable() {
    for (const auto& option : mc::ui::kCyclingOptions) {
        assert(findStaticLabel(option.id) == nullptr);
        assert(!hasRuntimeLabel(option.id));
        assert(!isUnlabelled(option.id));
    }
}

}  // namespace

int main() {
    testNoDuplicateRows();
    testStaticLabelsAreComplete();
    testSuffixIsDataNotASpecialCase();
    testClassesDoNotOverlap();
    testCyclingOptionsKeepTheirOwnTable();
    return 0;
}
