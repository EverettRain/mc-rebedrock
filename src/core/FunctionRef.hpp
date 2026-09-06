#pragma once

// 不拥有、不分配的可调用引用（RN-20a）
//
// 存在的理由只有一条：frame graph 的 pass body 落在**每帧**路径上。`std::function`
// 每次构造都可能堆分配，而 graph 的 body 是捕获了 `this` 的 lambda——那正是会分配的
// 那一类。`function_ref` 把「一个可调用体」压成两个指针（目标 + 蹦床），构造是两次
// 赋值，调用是一次间接跳转，两者都不碰堆。
//
// 它**不延长目标的生命周期**：绑定对象时，调用方必须保证该对象活得比 function_ref 久。
// 因此本仓的 graph 侧一律绑**自由函数指针**（下面第二个构造函数），把状态从
// `PassContext::user` 里取——那条路径没有生命周期可踩。绑 lambda 对象的能力留给
// 20f/20g 的光影包前端，那里的前端对象与图同寿。

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace mc::core {

template <typename Signature> class function_ref;

template <typename Result, typename... Args> class function_ref<Result(Args...)> final {
  public:
    function_ref() = default;

    // 自由函数指针：不引用任何对象，因此没有悬垂可能
    // 捕获为空的 lambda 会先隐式转换成函数指针再走这里
    function_ref(Result (*function)(Args...)) noexcept  // NOLINT(google-explicit-constructor)
        : storage_{.function = function},
          invoke_(function == nullptr ? nullptr : &invokeFunction) {}

    // 任意可调用对象：只存地址，**不拥有**
    template <typename Callable>
        requires(!std::is_same_v<std::decay_t<Callable>, function_ref> &&
                 !std::is_convertible_v<std::decay_t<Callable>, Result (*)(Args...)> &&
                 std::is_invocable_r_v<Result, std::remove_reference_t<Callable>&, Args...>)
    function_ref(Callable&& callable) noexcept  // NOLINT(google-explicit-constructor)
        : storage_{.object = const_cast<void*>(  // NOLINT(cppcoreguidelines-pro-type-const-cast)
              static_cast<const void*>(std::addressof(callable)))},
          invoke_(&invokeObject<std::remove_reference_t<Callable>>) {}

    Result operator()(Args... args) const {
        return invoke_(storage_, std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }

  private:
    union Storage {
        void* object;
        Result (*function)(Args...);
    };

    static Result invokeFunction(Storage storage, Args... args) {
        return storage.function(std::forward<Args>(args)...);
    }

    template <typename Callable> static Result invokeObject(Storage storage, Args... args) {
        return std::invoke(*static_cast<Callable*>(storage.object), std::forward<Args>(args)...);
    }

    Storage storage_{.object = nullptr};
    Result (*invoke_)(Storage, Args...) = nullptr;
};

} // namespace mc::core
