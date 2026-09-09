#pragma once

// 把 Unix 秒数拆成年月日时分秒。
//
// ★ **这是本仓唯一一处带平台分支的时间调用**，立成一个头文件就是为了它只有一处：
//   POSIX 有 `localtime_r(&time, &tm)` / `gmtime_r`，Windows 的 CRT 只有参数顺序
//   **相反**的 `localtime_s(&tm, &time)` / `gmtime_s`。UI-11 的世界列表第一次用到它，
//   而我当时只写了 POSIX 那一支——Windows 交叉构建当场报
//   `'localtime_r' was not declared in this scope`。
//
// ★ 时区是**参数**，不是进程状态。想要确定性的调用方（界面截图通道）传 `utc = true`，
//   而不是去改 `TZ` 环境变量：`setenv` 在 Windows 的 CRT 里根本不存在，而且
//   `getenv` 与 `setenv` 并发是未定义行为（`core/EnvFlags.hpp` 开篇那段说的就是这件事，
//   本作的区块流送、光照与音频各自跑在自己的线程上）。

#include <cstdint>
#include <ctime>

namespace mc::core {

[[nodiscard]] inline std::tm brokenDownTime(std::int64_t unixSeconds, bool utc) {
    const auto time = static_cast<std::time_t>(unixSeconds);
    std::tm broken{};
#if defined(_WIN32)
    if (utc) {
        static_cast<void>(gmtime_s(&broken, &time));
    } else {
        static_cast<void>(localtime_s(&broken, &time));
    }
#else
    if (utc) {
        static_cast<void>(gmtime_r(&time, &broken));
    } else {
        static_cast<void>(localtime_r(&time, &broken));
    }
#endif
    return broken;
}

} // namespace mc::core
