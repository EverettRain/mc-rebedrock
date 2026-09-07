// `GpuMesh` 拥有的缓冲只有一张表。
//
// 起点是一个真实缺陷：RN-22 给 `GpuMesh` 添了第三条缓冲 `translucentIndexBuffer`，
// 而释放路径有**两条**——运行期退回流式池（`retireMesh`）、退出期直接销毁（`~Impl`）——
// 只有第一条被更新。于是任何含半透明几何的场景在退出时泄漏一条缓冲，debug 下是
// VMA 的 `Some allocations were not freed` 断言。
//
// 它为什么能溜过去：八张预览图**全部正常写出**，泄漏只在进程退出时发作。离屏验收
// 数图片数是数不出来的，只有退出码会说话（RN-25 §4 那条盲区的又一个实例）。
//
// 所以这里钉两件事：① 那张表覆盖 `GpuMesh` 的**每一个** `AllocatedBuffer` 成员；
// ② 表里没有重复项。①靠读源码——成员是声明，不是运行期可枚举的东西。

#include "render/vulkan/WorldRenderTypes.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MC_REBEDROCK_WORLD_RENDER_TYPES_SRC
#error "MC_REBEDROCK_WORLD_RENDER_TYPES_SRC must point at src/render/vulkan/WorldRenderTypes.hpp"
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
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 注释里出现的 `AllocatedBuffer` 不是成员声明——而这个结构体的注释很长，正是重点。
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

// `marker` 之后那一对大括号里的内容，注释已剥。
[[nodiscard]] std::string blockAfter(std::string_view source, std::string_view marker) {
    const std::string clean = stripComments(source);
    const auto start = clean.find(marker);
    if (start == std::string::npos) {
        std::cerr << "FAIL: could not find `" << marker << "`\n";
        ++failures;
        return {};
    }
    const auto open = clean.find('{', start);
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < clean.size(); ++i) {
        if (clean[i] == '{') ++depth;
        if (clean[i] == '}') {
            --depth;
            if (depth == 0) {
                return clean.substr(open + 1, i - open - 1);
            }
        }
    }
    std::cerr << "FAIL: unbalanced braces after `" << marker << "`\n";
    ++failures;
    return {};
}

// `AllocatedBuffer <name>;` 的 name，按声明序。
[[nodiscard]] std::vector<std::string> bufferMembers(std::string_view body) {
    static constexpr std::string_view kType = "AllocatedBuffer";
    std::vector<std::string> names;
    for (std::size_t i = body.find(kType); i != std::string_view::npos;
         i = body.find(kType, i + 1)) {
        std::size_t cursor = i + kType.size();
        while (cursor < body.size() && (body[cursor] == ' ' || body[cursor] == '\n' ||
                                        body[cursor] == '\t' || body[cursor] == '\r')) {
            ++cursor;
        }
        const std::size_t begin = cursor;
        while (cursor < body.size() &&
               (std::isalnum(static_cast<unsigned char>(body[cursor])) != 0 ||
                body[cursor] == '_')) {
            ++cursor;
        }
        if (cursor == begin) {
            continue;
        }
        // `AllocatedBuffer x;` 才是成员；`AllocatedBuffer* p` 之类不是。
        while (cursor < body.size() && (body[cursor] == ' ' || body[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor < body.size() && body[cursor] == ';') {
            names.emplace_back(body.substr(begin, cursor - begin));
        }
    }
    return names;
}

// `ownedBuffers` 体里 `&mesh.<name>` 的 name。
[[nodiscard]] std::vector<std::string> listedBuffers(std::string_view body) {
    static constexpr std::string_view kPrefix = "&mesh.";
    std::vector<std::string> names;
    for (std::size_t i = body.find(kPrefix); i != std::string_view::npos;
         i = body.find(kPrefix, i + 1)) {
        std::size_t cursor = i + kPrefix.size();
        const std::size_t begin = cursor;
        while (cursor < body.size() &&
               (std::isalnum(static_cast<unsigned char>(body[cursor])) != 0 ||
                body[cursor] == '_')) {
            ++cursor;
        }
        if (cursor > begin) {
            names.emplace_back(body.substr(begin, cursor - begin));
        }
    }
    return names;
}

} // namespace

int main() {
    const std::string source = readFile(std::filesystem::path{MC_REBEDROCK_WORLD_RENDER_TYPES_SRC});
    if (source.empty()) {
        return 1;
    }

    // `final` 不能省：`struct GpuMesh` 是 `struct GpuMeshLayer` 的前缀，而后者先出现，
    // 少了它就会去数一个没有任何缓冲的结构体（第一版正是这么写的，测试自己抓住了）。
    const std::vector<std::string> members =
        bufferMembers(blockAfter(source, "struct GpuMesh final"));
    const std::vector<std::string> listed =
        listedBuffers(blockAfter(source, "ownedBuffers(GpuMesh& mesh)"));

    check(!members.empty(), "GpuMesh 必须至少有一条 AllocatedBuffer 成员（扫描器没在扫）");

    // ① 覆盖：每一个成员都在表里。这正是本轮那个缺陷的形状。
    const std::set<std::string> listedSet{listed.begin(), listed.end()};
    for (const std::string& member : members) {
        if (listedSet.count(member) == 0) {
            std::cerr << "FAIL: GpuMesh::" << member
                      << " is not in ownedBuffers() — it will be missed by one of the two release\n"
                         "  paths (retireMesh / ~Impl) and leak. Add it to the list in "
                         "WorldRenderTypes.hpp.\n";
            ++failures;
        }
    }
    // 反过来：表里不该有已经不存在的成员（那会编译失败，但顺手钉住语义）。
    const std::set<std::string> memberSet{members.begin(), members.end()};
    for (const std::string& name : listed) {
        if (memberSet.count(name) == 0) {
            std::cerr << "FAIL: ownedBuffers() lists `" << name
                      << "`, which is not an AllocatedBuffer member of GpuMesh\n";
            ++failures;
        }
    }

    // ② 表里没有重复项，且长度与成员数一致。
    check(listedSet.size() == listed.size(), "ownedBuffers() 里有重复项");
    check(listed.size() == members.size(),
          "ownedBuffers() 的条目数必须等于 GpuMesh 的 AllocatedBuffer 成员数");

    // 运行期一条：返回的指针互不相同，且都指进同一个对象。
    // 编译期的数组长度与源码里数出来的成员数也必须一致——写 `std::array<..., 3>` 却
    // 只填两项会在这里现形（另外两项是空指针）。
    mc::render::GpuMesh mesh;
    const auto pointers = mc::render::ownedBuffers(mesh);
    check(pointers.size() == members.size(),
          "std::array 的长度必须跟着 GpuMesh 的缓冲成员数走");
    std::set<const void*> distinct;
    for (const auto* pointer : pointers) {
        check(pointer != nullptr, "ownedBuffers() 里有空指针");
        distinct.insert(pointer);
    }
    check(distinct.size() == pointers.size(), "ownedBuffers() 返回了同一条缓冲两次");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "gpu_mesh_buffers ok (" << members.size() << " buffers per mesh)\n";
    return 0;
}
