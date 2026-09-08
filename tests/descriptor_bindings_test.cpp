// 重建描述符集时，布局声明的**每一个** binding 都必须被写。
//
// 现场（用户 macOS Release 实机）：开着太阳阴影时整个阴影范围一片全黑，没有一点阳光；
// 重开一次又好了，复现不出来。
//
// 根因不在阴影里。`createDescriptorPoolAndSets` 的第一件事是销毁描述符池——旧的集合
// 随之消失——然后分配一套新的，并写 binding 0..6 与 9。**binding 8（阴影深度图）不在
// 那批里**：它的资源建得比这套集合晚，所以首次初始化时由 `createShadowResources` 单独写。
//
// 而那个函数会被**重复调用**：改语言、切「强制 Unicode 字体」（`recreateFontTexture`）、
// 改各向异性（`recreateTextureSampler`）都会走到它。重建之后 binding 8 就是未写入的，
// 地形着色器接着去采样一个未写入的描述符——未定义行为。用户那次日志里字体数组在运行期
// 重建了两次（232 → 225 → 232），正是触发点；而那一跑 `Vulkan validation: disabled`，
// 所以一声不吭。时有时无，取决于那块描述符内存恰好是什么。
//
// 这条测试读源码，因为失效发生在**两个函数之间的关系**上：单看任何一个都是对的。
// 运行期也测不到——它需要一台真设备、一次选项切换，以及一个恰好不返回「全亮」的驱动。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

namespace {

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAILED: cannot read " << path.string() << "\n";
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 注释里写的绑定号不是声明，也不是写入。这两个函数的注释都很长，正是重点。
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

// `marker` 之后那一对大括号里的内容（注释已剥）。
[[nodiscard]] std::string bodyOf(std::string_view cleanSource, std::string_view marker) {
    const auto start = cleanSource.find(marker);
    if (start == std::string_view::npos) {
        std::cerr << "FAILED: could not find `" << marker << "`\n";
        ++failures;
        return {};
    }
    const auto open = cleanSource.find('{', start);
    if (open == std::string_view::npos) {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < cleanSource.size(); ++i) {
        if (cleanSource[i] == '{') ++depth;
        if (cleanSource[i] == '}') {
            --depth;
            if (depth == 0) {
                return std::string{cleanSource.substr(open + 1, i - open - 1)};
            }
        }
    }
    std::cerr << "FAILED: unbalanced braces after `" << marker << "`\n";
    ++failures;
    return {};
}

// `<prefix><digits>` 后面跟一个分号的那些数字。
[[nodiscard]] std::set<int> numbersAfter(std::string_view body, std::string_view prefix) {
    std::set<int> found;
    for (std::size_t i = body.find(prefix); i != std::string_view::npos;
         i = body.find(prefix, i + 1)) {
        std::size_t cursor = i + prefix.size();
        const std::size_t begin = cursor;
        while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor]))) {
            ++cursor;
        }
        if (cursor > begin) {
            found.insert(std::stoi(std::string{body.substr(begin, cursor - begin)}));
        }
    }
    return found;
}

[[nodiscard]] std::string join(const std::set<int>& values) {
    std::string out;
    for (const int value : values) {
        if (!out.empty()) out += ", ";
        out += std::to_string(value);
    }
    return out.empty() ? "(空)" : out;
}

} // namespace

int main() {
    const std::string clean = stripComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    if (clean.empty()) {
        return 1;
    }

    // 布局声明了哪些绑定
    const std::set<int> declared =
        numbersAfter(bodyOf(clean, "void createDescriptorSetLayout()"), ".binding = ");
    // 重建集合那条路上写了哪些——它自己写的，加上它调用的那个单一源写的
    const std::string rebuild = bodyOf(clean, "void createDescriptorPoolAndSets()");
    const std::string shadowWrite = bodyOf(clean, "void writeShadowDescriptorSets()");
    std::set<int> written = numbersAfter(rebuild, ".dstBinding = ");
    for (const int binding : numbersAfter(shadowWrite, ".dstBinding = ")) {
        written.insert(binding);
    }

    require(!declared.empty(), "布局里必须解析出绑定，否则这条测试是空的");
    require(declared.size() >= 8U, "布局的绑定数看起来太少，扫描器八成没在扫");

    // 重建那条路必须真的把单一源接上；少了这一句，下面的集合相等就成了假的。
    require(rebuild.find("writeShadowDescriptorSets()") != std::string::npos,
            "createDescriptorPoolAndSets 必须调用 writeShadowDescriptorSets——"
            "销毁描述符池会让旧集合连同它的 binding 8 一起消失");

    std::set<int> missing;
    std::set_difference(declared.begin(), declared.end(), written.begin(), written.end(),
                        std::inserter(missing, missing.end()));
    if (!missing.empty()) {
        std::cerr << "FAILED: 布局声明了 binding " << join(missing)
                  << "，但重建描述符集时没有写它们。\n"
                     "  改语言 / 切强制 Unicode 字体 / 改各向异性都会销毁描述符池并重新\n"
                     "  分配整套集合；没被写的那个绑定此后就是未写入的，采样它是未定义行为。\n";
        ++failures;
    }

    std::set<int> extra;
    std::set_difference(written.begin(), written.end(), declared.begin(), declared.end(),
                        std::inserter(extra, extra.end()));
    if (!extra.empty()) {
        std::cerr << "FAILED: 写了 binding " << join(extra) << "，但布局里没有声明它们\n";
        ++failures;
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "descriptor_bindings ok (布局与重建各 " << declared.size() << " 个绑定："
              << join(declared) << ")\n";
    return 0;
}
