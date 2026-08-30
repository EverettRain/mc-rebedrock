#pragma once

// MC_REBEDROCK_* 诊断开关的一次性求值
//
// 每个开关在进程内只读一次 getenv，理由有两条，第二条更要紧
// 其一是环境变量在运行期不会变，而读它的位置常常在热路径上
// queueStreamBatch 曾在世界写锁持有期间每批次调两次 getenv("MC_REBEDROCK_SMOKE_TEST")
// 其二是 getenv 本身不是线程安全的，POSIX 下它与 setenv 并发是未定义行为
// 而本项目的区块流送、光照与音频都跑在各自的线程上
// 把求值收进函数级 static，整个进程就只在第一次调用时读一次，之后是一个纯读的 bool
//
// 范式取自 core/FrameTrace.hpp 的 traceEnabled()
// 以及 world/ChunkStreamingTrace.hpp 的 chunkTraceEnabled()
// 它们早就是这么写的，这里补上散落在渲染器各处、每次用都现读一遍的其余开关

#include <cstdlib>

namespace mc::diag {

// 脚本化的烟测会话，调度器见 render/SmokeScript.hpp
// 它同时把校验层的报错升级为致命错误，见下面的 fatalValidationEnabled
[[nodiscard]] inline bool smokeTestEnabled() {
    static const bool enabled = std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr;
    return enabled;
}

// 校验层一报错就终止进程
// 校验回调不能跨 C ABI 抛异常，所以打印完整的 VUID 与句柄之后只能直接 abort
// 烟测始终开着这道闸，交互运行或定点复现时可以单独打开它
[[nodiscard]] inline bool fatalValidationEnabled() {
    static const bool enabled = std::getenv("MC_REBEDROCK_FATAL_VALIDATION") != nullptr;
    return enabled;
}

}  // namespace mc::diag
