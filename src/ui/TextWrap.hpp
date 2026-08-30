#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::ui {

// 把一个逻辑行拆成若干视觉行，每行都不宽于 maxWidth
// 输入假定不含 '\n'，硬换行由调用方先行拆开
// 断行按词，与 vanilla 的 TextHandler.wrapLines 一致：断点落在溢出前的最后一个空格上
// 用于断行的那个空格被丢弃，比 maxWidth 还宽的单个词则在 UTF-8 码点边界处切开，绝不切在字节中间
//
// measure 返回一个 UTF-8 子串的宽度，单位与 maxWidth 相同，比如未缩放的 GUI 像素
// 宽度逐码点累加，对这种只有步进值、没有字距调整的字体来说是精确的
// 因此换行是 O(n)，不必对越来越长的前缀反复测量
// 空输入不产出任何行
[[nodiscard]] std::vector<std::string>
wrapText(std::string_view text, float maxWidth,
         const std::function<float(std::string_view)>& measure);

} // namespace mc::ui
