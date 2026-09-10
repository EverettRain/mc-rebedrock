#!/usr/bin/env sh
# 一条命令构建两个规范分支：
#   debug   -> build/rebedrock-debug
#   release -> build/rebedrock-release
# 项目只支持这两个构建目录（见 README「macOS 构建与运行」），请勿自创其他分支。
#
# 用法:
#   ./build.sh              # 构建两个分支（未配置会自动配置）
#   ./build.sh --configure  # 强制重新配置后再构建
#   ./build.sh --test       # 构建后对两个分支跑 ctest
#   ./build.sh -j4          # 指定并行数（默认 8）
set -e

cd "$(dirname "$0")"

CONFIGURE=0
TEST=0
JOBS=8
for arg in "$@"; do
    case "$arg" in
        --configure) CONFIGURE=1 ;;
        --test) TEST=1 ;;
        -j*) JOBS="${arg#-j}" ;;
        --help|-h)
            echo "用法: ./build.sh [--configure] [--test] [-j<N>]"
            echo "  构建两个规范分支（build/rebedrock-debug 与 build/rebedrock-release）。"
            exit 0
            ;;
        *)
            echo "未知参数: $arg（用 --help 查看用法）" >&2
            exit 1
            ;;
    esac
done

# 秒级墙钟计时；构建阶段包含按需配置及 Windows 伴随构建。
time_stage() {
    stage_started=$(date +%s)
    "$@"
    STAGE_SECONDS=$(( $(date +%s) - stage_started ))
}

print_duration() {
    printf '  %-16s %d 分 %02d 秒 (%d 秒)\n' \
        "$1" "$(( $2 / 60 ))" "$(( $2 % 60 ))" "$2"
}

build_branch() {
    dir="$1"
    shift
    if [ "$CONFIGURE" = 1 ] || [ ! -f "$dir/CMakeCache.txt" ]; then
        echo ">>> 配置 $dir"
        cmake -S . -B "$dir" "$@"
    fi
    echo ">>> 构建 $dir"
    cmake --build "$dir" --parallel "$JOBS"
}

BUILD_STARTED=$(date +%s)
time_stage build_branch build/rebedrock-debug \
    -DBUILD_TESTING=ON -DMC_REBEDROCK_BUILD_WINDOWS_ON_MAC=ON
DEBUG_SECONDS=$STAGE_SECONDS
time_stage build_branch build/rebedrock-release \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
    -DMC_REBEDROCK_BUILD_WINDOWS_ON_MAC=ON
RELEASE_SECONDS=$STAGE_SECONDS

if [ "$TEST" = 1 ]; then
    echo ">>> 测试 debug"
    time_stage ctest --test-dir build/rebedrock-debug --output-on-failure
    DEBUG_TEST_SECONDS=$STAGE_SECONDS
    echo ">>> 测试 release"
    time_stage ctest --test-dir build/rebedrock-release --output-on-failure
    RELEASE_TEST_SECONDS=$STAGE_SECONDS
fi

echo ">>> 组装 release-pak（预编译双平台二进制包，剔除个人信息）"
time_stage ./tools/make-release-pak.sh
PAK_SECONDS=$STAGE_SECONDS
TOTAL_SECONDS=$(( $(date +%s) - BUILD_STARTED ))

echo "完成："
echo "  debug   -> build/rebedrock-debug/game/bin/mc_rebedrock"
echo "  release -> build/rebedrock-release/game/bin/mc_rebedrock"
echo "  pak     -> $(ls build/mc-rebedrock-*-win-mac.zip 2>/dev/null | head -1 || echo build/mc-rebedrock-<version>-win-mac.zip)"

echo "耗时统计（debug/release 含配置及 Windows 伴随构建）："
print_duration debug "$DEBUG_SECONDS"
print_duration release "$RELEASE_SECONDS"
if [ "$TEST" = 1 ]; then
    print_duration 'test debug' "$DEBUG_TEST_SECONDS"
    print_duration 'test release' "$RELEASE_TEST_SECONDS"
fi
print_duration pak "$PAK_SECONDS"
print_duration total "$TOTAL_SECONDS"
