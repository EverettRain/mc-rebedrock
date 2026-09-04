#!/usr/bin/env bash
# RN-15d：把一个方块渲成八张图，并（可选）验证它们是可复现的。
#
# 用法：
#   tools/export_block_preview.sh <方块规格> [--pack <资源包>] [--size N] [--out <目录>]
#   tools/export_block_preview.sh --verify <方块规格> [同上]
#
# 例：
#   tools/export_block_preview.sh 'oak_trapdoor[open=true,half=top]' --pack ~/packs/vanilla
#   tools/export_block_preview.sh --verify oak_stairs --pack ~/packs/vanilla
#
# --verify 跑两遍、写进两个目录、逐字节比对。
# 这个工具的全部价值在于"可比"：一张不可复现的漂亮图片毫无价值，所以"两次运行逐字节
# 相同"是它的验收条件，而不是一句性质描述。比对失败会打印哪几张图不同并非零退出。
#
# 需要 GPU。本仓库的开发容器是 headless 的，跑不了这条路径。

set -euo pipefail

BINARY="${MC_REBEDROCK_BINARY:-build/linux-debug/mc_rebedrock}"
VERIFY=0
SPEC=""
SIZE=""
OUT=""
PACKS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify) VERIFY=1; shift ;;
        --pack)   PACKS+=(--pack "$2"); shift 2 ;;
        --size)   SIZE="$2"; shift 2 ;;
        --out)    OUT="$2"; shift 2 ;;
        -*)       echo "未知参数：$1" >&2; exit 2 ;;
        *)        if [[ -n "$SPEC" ]]; then echo "只能给一个方块规格" >&2; exit 2; fi
                  SPEC="$1"; shift ;;
    esac
done

if [[ -z "$SPEC" ]]; then
    echo "用法：$0 [--verify] <方块规格> [--pack <资源包>] [--size N] [--out <目录>]" >&2
    exit 2
fi
if [[ ! -x "$BINARY" ]]; then
    echo "找不到可执行文件：$BINARY（用 MC_REBEDROCK_BINARY 指定）" >&2
    exit 2
fi

run_export() {  # $1 = 输出根目录
    local args=(--test-scene "$SPEC" --export-preview --preview-out "$1")
    [[ -n "$SIZE" ]] && args+=(--preview-size "$SIZE")
    [[ ${#PACKS[@]} -gt 0 ]] && args+=("${PACKS[@]}")
    "$BINARY" "${args[@]}"
}

if [[ "$VERIFY" -eq 0 ]]; then
    run_export "${OUT:-export/blocks-preview}"
    exit $?
fi

ROOT="${OUT:-export/blocks-preview-verify}"
rm -rf "$ROOT/run-a" "$ROOT/run-b"
run_export "$ROOT/run-a"
run_export "$ROOT/run-b"

# diff -r 会把"只在一边存在的文件"也报出来，所以少出一张图同样是失败
if diff -r "$ROOT/run-a" "$ROOT/run-b" >/dev/null; then
    echo "确定性通过：两次运行的八张图逐字节相同（$ROOT）"
else
    echo "确定性失败：两次运行的图片不同" >&2
    diff -rq "$ROOT/run-a" "$ROOT/run-b" >&2 || true
    exit 1
fi
