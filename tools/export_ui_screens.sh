#!/usr/bin/env bash
# UI-2：把一个或多个前端界面各拍一张 PNG，并（可选）验证它们是可复现的。
#
# 全仓在这之前唯一的画面回读是方块预览的八机位循环，界面一张图都出不来——于是
# "与 26.1 1:1" 这句话不可验收。这条脚本是那条入口的命令行外壳，规矩照抄
# export_block_preview.sh：确定性是它的**验收条件**，不是对它的描述。
#
# 用法：
#   tools/export_ui_screens.sh [--verify] <页名>[,<页名>...] [选项]
#
# 选项：
#   --scale <档>[,<档>...]   GUI 缩放档，0 = Auto。默认 2,3
#   --size  <宽>x<高>        画布尺寸，默认 1280x720
#   --out   <目录>           输出根目录
#   --pack  <资源包>         附加资源包，可重复
#
# 例：
#   tools/export_ui_screens.sh title
#   tools/export_ui_screens.sh --verify title,options --scale 1,2,4
#
# ★ 同一个屏幕在不同 GUI scale 下是**不同的版面**（spec §5 的几何全是逻辑画布上的
#   整数运算，而逻辑画布 = ceil(帧缓冲 / scale)）。只拍一档等于没拍，所以默认两档。
#
# ★ 这条通道读的是离屏的 sceneTargets，位于 copySceneToSwapchain **之前**，
#   因此它**看不到呈现链**（RN-25 §4）。凡引用它说"视觉验收通过"都要带这个限定。
#
# 环境：容器内可用（Xvfb + lavapipe），不需要 macOS，也不需要独立 GPU。
#   Xvfb :99 -screen 0 1280x1024x24 &   然后   export DISPLAY=:99

set -euo pipefail

BINARY="${MC_REBEDROCK_BINARY:-}"
VERIFY=0
PAGES=""
SCALE=""
SIZE=""
OUT=""
PACKS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify) VERIFY=1; shift ;;
        --scale)  SCALE="$2"; shift 2 ;;
        --size)   SIZE="$2"; shift 2 ;;
        --out)    OUT="$2"; shift 2 ;;
        --pack)   PACKS+=(--pack "$2"); shift 2 ;;
        -*)       echo "未知参数：$1" >&2; exit 2 ;;
        *)        if [[ -n "$PAGES" ]]; then echo "只能给一份页名列表" >&2; exit 2; fi
                  PAGES="$1"; shift ;;
    esac
done

if [[ -z "$PAGES" ]]; then
    echo "用法：$0 [--verify] <页名>[,<页名>...] [--scale 2,3] [--size 1280x720] [--out 目录]" >&2
    echo "  页名：title / world-list / create-world / edit-world / confirm-delete /" >&2
    echo "        options / video-settings / controls / language / experimental" >&2
    exit 2
fi

# 本机能不能执行这个文件 —— 靠头四个魔数字节判，不靠目录名也不靠 `file`。
# 一个 build/ 树里同时躺着给别的平台构建的同名二进制是常态，而 `-x` 对它们一样为真。
host_can_run() {
    local magic
    magic="$(head -c 4 < "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    case "$(uname -s)" in
        Darwin) [[ "$magic" == cffaedfe || "$magic" == cefaedfe || \
                   "$magic" == cafebabe || "$magic" == bebafeca ]] ;;   # Mach-O / universal
        Linux)  [[ "$magic" == 7f454c46 ]] ;;                            # ELF
        *)      return 0 ;;
    esac
}

if [[ -z "$BINARY" ]]; then
    newest=""
    while IFS= read -r candidate; do
        [[ -x "$candidate" ]] || continue
        host_can_run "$candidate" || continue
        if [[ -z "$newest" || "$candidate" -nt "$newest" ]]; then
            newest="$candidate"
        fi
    done < <(find build -maxdepth 4 -type f -name 'mc_rebedrock' 2>/dev/null)
    BINARY="$newest"
fi
if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "找不到本机可执行的 mc_rebedrock。已在 build/*/game/bin/ 下搜过；" >&2
    echo "用 MC_REBEDROCK_BINARY=<路径> 指定，或先为本机构建 mc_rebedrock。" >&2
    exit 2
fi
echo "使用可执行文件：${BINARY}"

run_capture() {  # $1 = 输出根目录
    local args=(--ui-shot "$PAGES" --ui-out "$1")
    if [[ -n "$SCALE" ]]; then args+=(--ui-scale "$SCALE"); fi
    if [[ -n "$SIZE"  ]]; then args+=(--ui-size  "$SIZE");  fi
    # `${PACKS[@]+"${PACKS[@]}"}` 而不是 `"${PACKS[@]}"`：macOS 自带 bash 3.2，
    # 在 `set -u` 下展开一个空数组会报 unbound variable
    "$BINARY" "${args[@]}" ${PACKS[@]+"${PACKS[@]}"}
}

if [[ "$VERIFY" -eq 0 ]]; then
    # 不要把它接进管道：`./tool | tail` 的退出码是 tail 的 0，本仓踩过四次。
    run_capture "${OUT:-export/ui-preview}"
    exit $?
fi

ROOT="${OUT:-export/ui-preview-verify}"
rm -rf "$ROOT/run-a" "$ROOT/run-b"
run_capture "$ROOT/run-a"
run_capture "$ROOT/run-b"

# diff -r 会把"只在一边存在的文件"也报出来，所以少出一张图同样是失败
if diff -r "$ROOT/run-a" "$ROOT/run-b" >/dev/null; then
    count=$(find "$ROOT/run-a" -name '*.png' | wc -l | tr -d ' ')
    echo "确定性通过：两次运行的 ${count} 张图逐字节相同（${ROOT}）"
else
    echo "确定性失败：两次运行的图片不同" >&2
    diff -rq "$ROOT/run-a" "$ROOT/run-b" >&2 || true
    exit 1
fi
