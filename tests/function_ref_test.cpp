// RN-20a：pass body 骑的那个不拥有、不分配的可调用引用
//
// 它存在的唯一理由是「每帧路径上不许堆分配」，所以这里的第一条断言就是分配计数。

#include "core/FunctionRef.hpp"

#include <cstdlib>
#include <iostream>
#include <new>

namespace {

std::size_t& allocationCount() {
    static std::size_t count = 0;
    return count;
}

bool& counting() {
    static bool enabled = false;
    return enabled;
}

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

int doubled(int value) { return value * 2; }

} // namespace

void* operator new(std::size_t size) {
    if (counting()) {
        ++allocationCount();
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    using mc::core::function_ref;

    {
        // 自由函数指针：graph 侧走的就是这条路径
        const function_ref<int(int)> reference{&doubled};
        check(static_cast<bool>(reference), "绑了函数的引用应当为真");
        check(reference(21) == 42, "自由函数调用");
    }
    {
        // 捕获 this 的 lambda——`std::function` 正是在这里可能堆分配
        int total = 0;
        const auto accumulate = [&total](int value) { total += value; };
        counting() = true;
        const std::size_t before = allocationCount();
        const function_ref<void(int)> reference{accumulate};
        reference(3);
        reference(4);
        const std::size_t allocations = allocationCount() - before;
        counting() = false;
        check(allocations == 0, "构造与调用都不许堆分配");
        check(total == 7, "捕获状态的 lambda 被正确调用");
    }
    {
        // 默认构造的是空的：graph 的 compile 靠这条拒掉没有 body 的 pass
        const function_ref<void()> empty;
        check(!static_cast<bool>(empty), "默认构造的引用应当为假");
    }
    {
        // 大小契约：两个指针。超了说明有人往里塞了状态，那就不再是「引用」了
        check(sizeof(function_ref<void(int)>) == 2 * sizeof(void*), "两个指针大小");
    }
    {
        // 返回值与多参数
        const function_ref<int(int, int)> sum{+[](int a, int b) { return a + b; }};
        check(sum(2, 3) == 5, "多参数与返回值");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "function_ref_test ok\n";
    return 0;
}
