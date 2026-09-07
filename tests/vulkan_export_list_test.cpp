// `cmake/vulkan-1.def` 必须覆盖 src 里真正调用到的每一个 Vulkan 入口点。
//
// 那张名单是**手工维护**的，而漏一行的后果分布得很不均匀：Linux 与 macOS 走
// `find_package(Vulkan)`，链的是真正的 loader，漏了照样编过；只有 Windows 交叉编译
// （mingw + 从这份 .def 生成的导入库）会在**链接期**报 undefined reference。
// 于是「本机全绿」与「构建是好的」之间有一段真空，而它已经咬过两次。
//
// 文件头本来就写着自查命令。这个测试就是那条命令，只是改成每次 ctest 都跑一遍——
// 一条要靠人记得执行的检查，等于一条迟早会被忘记的检查。

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif

namespace {

const std::filesystem::path kSourceDir{MC_REBEDROCK_SOURCE_DIR};

// 不是 Vulkan 入口点、却长着 `vk` + 大写字母的名字。
//
// 只有一个：本仓自己的 `vkStructure<T>()`（填 sType 的小模板）。
//
// 扩展函数（`vkCreateDebugUtilsMessengerEXT` 等）**不需要**在这里列出——它们在源码里
// 只以 `PFN_vk…`（`_` 是词字符，没有词边界）和字符串字面量的形式出现，两者都被下面
// 的扫描排除掉了。这不是巧合而是它们的本来形态：那些函数是 `vkGetInstanceProcAddr`
// 动态取的，本来就不该进导入库。
const std::set<std::string> kNotEntryPoints{"vkStructure"};

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

// 去掉注释与字符串/字符字面量。
//
// 两者都必须去：注释里提到一个函数不会产生任何符号引用；而字符串更关键——
// `checkVk(..., "vkCreateQueryPool(timestamps)")` 里的名字是给人看的错误消息，
// `vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")` 里的更是
// **明确不走导入库**的那条路。把它们算进来，这张名单就会开始要求它不该有的东西。
[[nodiscard]] std::string stripCommentsAndLiterals(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) ++i;
            i = i + 2 < source.size() ? i + 2 : source.size();
        } else if (source[i] == '"' || source[i] == '\'') {
            const char quote = source[i];
            ++i;
            while (i < source.size() && source[i] != quote) {
                i += source[i] == '\\' ? 2 : 1;
            }
            i = i < source.size() ? i + 1 : i;
            out.push_back(' ');
        } else {
            out.push_back(source[i]);
            ++i;
        }
    }
    return out;
}

[[nodiscard]] bool wordCharacter(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// `vk` + 一个大写字母开头的标识符，且前面不是词字符——后一条正是把 `PFN_vkXxx`
// 排除在外的那一半。
void collectVulkanNames(std::string_view code, std::set<std::string>& into) {
    for (std::size_t i = 0; i + 2 < code.size(); ++i) {
        if (code[i] != 'v' || code[i + 1] != 'k') {
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(code[i + 2])) == 0) {
            continue;
        }
        if (i > 0 && wordCharacter(code[i - 1])) {
            continue;
        }
        std::size_t end = i + 2;
        while (end < code.size() && wordCharacter(code[end])) {
            ++end;
        }
        into.emplace(code.substr(i, end - i));
        i = end - 1;
    }
}

// 扫描器自己的单元测试。
//
// 这一段是补出来的，而且是**第二次**补：先补了「锚点必须被扫到」，sabotage 关掉注释
// 剥离之后测试**仍然是绿的**——丢掉的 8 个入口点恰好一个锚点都不沾。靠仓库里碰巧
// 存在的形状去抓一个缺陷，抓不抓得住是运气（[[lesson-assertion-fixture-shape]]）。
//
// 所以改成直接喂夹具给那个函数，每条夹具对着一种具体的失效形状：
// 注释里的一个孤立引号会让后面的真代码被当成字符串整段吞掉——那正是关掉注释剥离时
// 发生的事，也是它「只会漏报、而且无声」的原因。
[[nodiscard]] std::set<std::string> scan(std::string_view code) {
    std::set<std::string> names;
    collectVulkanNames(stripCommentsAndLiterals(code), names);
    return names;
}

int scannerFailures = 0;

void expectScan(std::string_view code, std::string_view name, bool present, const char* what) {
    const auto names = scan(code);
    if ((names.count(std::string{name}) != 0) != present) {
        std::cerr << "FAIL: " << what << '\n';
        ++scannerFailures;
    }
}

void testScanner() {
    // 一句注释不该吞掉它后面的代码——哪怕注释里有个孤立的引号。
    expectScan("// 说明里提了一句 don't\nvkCmdDraw(buffer);\n", "vkCmdDraw", true,
               "行注释里的孤立引号不得吞掉后面的真调用");
    expectScan("/* 块注释里带一个 \" 引号 */\nvkCmdEndQuery(buffer);\n", "vkCmdEndQuery", true,
               "块注释里的孤立引号不得吞掉后面的真调用");
    // 注释里提到的函数不产生任何符号引用。
    expectScan("// 这里将来要改用 vkCmdBeginQuery\n", "vkCmdBeginQuery", false,
               "只在注释里出现的名字不算调用");
    // 字符串里的名字同理——错误消息与 vkGetInstanceProcAddr 的参数都长这样。
    expectScan("checkVk(result, \"vkCreateQueryPool(timestamps)\");", "vkCreateQueryPool", false,
               "只在字符串里出现的名字不算调用");
    // `PFN_vkXxx` 是函数指针类型，不是导入库里的符号。
    expectScan("reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(f);",
               "vkCreateDebugUtilsMessengerEXT", false, "PFN_ 前缀的类型名不算调用");
    // 真调用要认出来。
    expectScan("vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);", "vkQueueSubmit", true,
               "普通调用要被认出来");
}

} // namespace

int main() {
    testScanner();
    if (scannerFailures != 0) {
        std::cerr << scannerFailures
                  << " scanner check(s) failed — the repository scan below would be meaningless\n";
        return 1;
    }

    // 名单
    std::set<std::string> exported;
    {
        const std::string text = readFile(kSourceDir / "cmake" / "vulkan-1.def");
        if (text.empty()) {
            return 1;
        }
        std::istringstream lines{text};
        std::string line;
        while (std::getline(lines, line)) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == ';') {
                continue;  // 空行与 `;` 注释
            }
            const auto last = line.find_last_not_of(" \t\r");
            std::string name = line.substr(first, last - first + 1);
            if (name.rfind("vk", 0) == 0) {
                exported.insert(std::move(name));
            }
        }
    }
    if (exported.empty()) {
        std::cerr << "FAIL: cmake/vulkan-1.def listed no vk entry points at all\n";
        return 1;
    }

    // 用到的
    std::set<std::string> used;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{kSourceDir / "src"}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension();
        if (extension != ".hpp" && extension != ".cpp") {
            continue;
        }
        collectVulkanNames(stripCommentsAndLiterals(readFile(entry.path())), used);
    }

    // 扫描器必须真的在扫。
    //
    // 这条是补出来的：sabotage 把注释剥离关掉之后，测试**仍然是绿的**——但它认出的
    // 入口点从 81 个掉到 74 个（注释里的一个引号会让后面的真代码被当成字符串吞掉）。
    // 一个只会漏报的检查等于没有检查，而且它漏得无声无息。所以钉几个锚点：
    // 这几个入口点在 src 里的存在是结构性的（每一帧都在调），扫不到它们就说明
    // 扫的不是源码本身。
    const std::vector<std::string> kAnchors{
        "vkCmdDraw",         "vkCmdDrawIndexed",     "vkCmdPipelineBarrier",
        "vkCmdBeginRenderPass", "vkCmdBindPipeline", "vkCreateQueryPool",
        "vkCmdWriteTimestamp",  "vkQueueSubmit",     "vkWaitForFences",
    };
    bool scannerIntact = true;
    for (const std::string& anchor : kAnchors) {
        if (used.count(anchor) == 0) {
            std::cerr << "FAIL: the scan did not find " << anchor
                      << ", which src/ certainly calls — the scanner itself is broken, so a\n"
                         "  green result here would mean nothing\n";
            scannerIntact = false;
        }
    }
    if (!scannerIntact) {
        return 1;
    }

    std::vector<std::string> missing;
    for (const std::string& name : used) {
        if (kNotEntryPoints.count(name) != 0 || exported.count(name) != 0) {
            continue;
        }
        missing.push_back(name);
    }

    if (!missing.empty()) {
        std::cerr << "FAIL: cmake/vulkan-1.def is missing " << missing.size()
                  << " entry point(s) that src/ calls:\n";
        for (const std::string& name : missing) {
            std::cerr << "    " << name << '\n';
        }
        std::cerr << "  Add each one to cmake/vulkan-1.def (alphabetically). Linux and macOS link\n"
                     "  the real loader and will not notice; the Windows cross build fails at link\n"
                     "  time with `undefined reference`.\n";
        return 1;
    }

    // 反方向只报告，不判失败：名单里多几个不用的入口点无害（导入库多一个符号而已），
    // 而 `#if defined(__APPLE__)` 之类的平台分支会让「用到」的集合本身随平台变化。
    // 把它做成硬判据就会逼着人按当前平台删名单，那是拿一个真问题换一个假问题。
    std::vector<std::string> unused;
    for (const std::string& name : exported) {
        if (used.count(name) == 0) {
            unused.push_back(name);
        }
    }
    std::cout << "vulkan_export_list ok (" << used.size() << " entry points used, "
              << exported.size() << " exported";
    if (!unused.empty()) {
        std::cout << ", " << unused.size() << " listed but unused";
    }
    std::cout << ")\n";
    return 0;
}
