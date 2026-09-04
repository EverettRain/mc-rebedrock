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

BINARY="${MC_REBEDROCK_BINARY:-}"
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
# 本机能不能执行这个文件 —— 靠头四个魔数字节判，不靠目录名也不靠 `file`
# 一个 build/ 树里同时躺着给别的平台构建的同名二进制是常态（容器里出的 Linux 版
# 与本机的 macOS 版并存），而 `-x` 对它们一样为真：POSIX 权限位不认架构
host_can_run() {
    local magic
    # 用重定向而不是 `head -c 4 "$1"`：既绕开 BSD/GNU head 的选项差异，
    # 也不用担心路径以 `-` 开头
    magic="$(head -c 4 < "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    case "$(uname -s)" in
        Darwin) [[ "$magic" == cffaedfe || "$magic" == cefaedfe || \
                   "$magic" == cafebabe || "$magic" == bebafeca ]] ;;   # Mach-O / universal
        Linux)  [[ "$magic" == 7f454c46 ]] ;;                            # ELF
        *)      return 0 ;;   # 认不出的平台就别拦，让 exec 自己去报错
    esac
}

# 可执行文件：MC_REBEDROCK_BINARY 优先，否则在 build/ 下找暂存出来的那个
# 暂存布局是 <构建目录>/game/bin/mc_rebedrock（见 CMakeLists 的 MC_REBEDROCK_GAME_ROOT），
# 构建目录名各人各异（预设名、rebedrock-debug、linux-debug…），所以是搜不是写死
if [[ -z "$BINARY" ]]; then
    newest=""
    skipped=""
    while IFS= read -r candidate; do
        [[ -x "$candidate" ]] || continue
        if ! host_can_run "$candidate"; then
            skipped="${skipped}  ${candidate}（不是本机架构）"$'\n'
            continue
        fi
        if [[ -z "$newest" || "$candidate" -nt "$newest" ]]; then
            newest="$candidate"
        fi
    done < <(find build -maxdepth 4 -type f -name 'mc_rebedrock' 2>/dev/null)
    BINARY="$newest"
    if [[ -z "$BINARY" && -n "$skipped" ]]; then
        echo "build/ 下只找到别的平台的二进制：" >&2
        printf '%s' "$skipped" >&2
    fi
fi
if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "找不到本机可执行的 mc_rebedrock。已在 build/*/game/bin/ 下搜过；" >&2
    echo "用 MC_REBEDROCK_BINARY=<路径> 指定，或先为本机构建 mc_rebedrock。" >&2
    exit 2
fi
if ! host_can_run "$BINARY"; then
    echo "警告：${BINARY} 看起来不是本机架构，执行大概率会失败" >&2
fi
echo "使用可执行文件：${BINARY}"

run_export() {  # $1 = 输出根目录
    local args=(--test-scene "$SPEC" --export-preview --preview-out "$1")
    if [[ -n "$SIZE" ]]; then
        args+=(--preview-size "$SIZE")
    fi
    # `${PACKS[@]+"${PACKS[@]}"}` 而不是 `"${PACKS[@]}"`：macOS 自带的是 bash 3.2，
    # 在 `set -u` 下展开一个空数组会报 unbound variable
    "$BINARY" "${args[@]}" ${PACKS[@]+"${PACKS[@]}"}
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
    echo "确定性通过：两次运行的八张图逐字节相同（${ROOT}）"
else
    echo "确定性失败：两次运行的图片不同" >&2
    diff -rq "$ROOT/run-a" "$ROOT/run-b" >&2 || true
    exit 1
fi
