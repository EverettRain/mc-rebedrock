#!/usr/bin/env bash
# UI-12 / 乙：界面出图的**回归门禁**。
#
# 这条线做到 UI-11 为止，52 张基线从来没有入库，也没有任何自动比对：每个节点都是
# 手工重拍、肉眼看。于是"改一处静默弄坏另一屏"没有任何东西会红——而这条线的改动
# 恰恰几乎每一次都横跨多屏（一个 PageLayoutKind、一个图集层、一条布局分支）。
#
#   tools/ui_baseline.sh record [--dir 目录] [--scale 2,3]   # 记一套基线
#   tools/ui_baseline.sh check  [--dir 目录] [--scale 2,3]   # 重拍并比对，有差异非零退出
#
# ★ **目标清单来自二进制**（`--ui-list` 读 `kTargetNames`），不是脚本里抄的一份。
#   README 与出图脚本各抄过一份，两份都落后过（`experimental` 删了、容器八屏加了、
#   UI-11 又加了两个）。门禁抄清单 = 新加的屏不在门禁里。
#
# ★ **哈希是环境相关的**。这套图由本机的驱动渲染（本容器是 Xvfb + lavapipe），
#   换一块 GPU、换一版驱动，整表都会不同——那不是回归。所以基线目录**不入库**，
#   它是"你自己这台机器上、改动之前的那一套"。用法是：动手前 record，收工前 check。
#
# ★ 它读的仍是离屏 `sceneTargets`（copySceneToSwapchain 之前），**看不到呈现链**。
#
# 退出码：0 = 全同；1 = 有差异或缺图；2 = 用法错。

set -euo pipefail

MODE="${1:-}"
shift || true

DIR="export/ui-baseline"
SCALE="2,3"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir)   DIR="$2"; shift 2 ;;
        --scale) SCALE="$2"; shift 2 ;;
        *)       echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

if [[ "$MODE" != "record" && "$MODE" != "check" ]]; then
    echo "用法：$0 record|check [--dir 目录] [--scale 2,3]" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHOT="$HERE/tools/export_ui_screens.sh"

# 本机能不能执行这个文件 —— 判据与 export_ui_screens.sh 相同（魔数，不看目录名）。
BINARY="${MC_REBEDROCK_BINARY:-}"
if [[ -z "$BINARY" ]]; then
    while IFS= read -r candidate; do
        magic="$(head -c 4 < "$candidate" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
        case "$(uname -s)" in
            Darwin) [[ "$magic" == cffaedfe || "$magic" == cefaedfe || \
                       "$magic" == cafebabe || "$magic" == bebafeca ]] || continue ;;
            Linux)  [[ "$magic" == 7f454c46 ]] || continue ;;
        esac
        if [[ -z "$BINARY" || "$candidate" -nt "$BINARY" ]]; then BINARY="$candidate"; fi
    done < <(find "$HERE/build" -maxdepth 4 -type f -name 'mc_rebedrock' 2>/dev/null)
fi
if [[ -z "$BINARY" ]]; then
    echo "找不到本机可执行的 mc_rebedrock" >&2
    exit 2
fi
export MC_REBEDROCK_BINARY="$BINARY"

# 全部目标，逗号分隔。**单一来源是二进制**。
TARGETS="$("$BINARY" --ui-list 2>/dev/null | paste -sd, -)"
if [[ -z "$TARGETS" ]]; then
    echo "--ui-list 没有输出：这个二进制太旧了？" >&2
    exit 2
fi

# 四根轴各自的变体。**每一条都要说清它为什么在这里**——一个不解释的变体，
# 下一个人不知道能不能删。
#   标签 | 目标 | 附加参数
VARIANTS=(
    # 悬停：槽位高亮的两张 24x24 精灵与物品提示框只有这一档进得了画。
    "inventory-hover|inventory|--ui-cursor 640,360"
    # 手上拿着东西：它**抑制**提示框，是上面那一档的对照。
    "inventory-carry|inventory|--ui-cursor 640,360 --ui-carry"
    # 创建世界的另外两页：三页的控件与页签下划线各不相同。
    "create-world-tab1|create-world|--ui-tab 1"
    "create-world-tab2|create-world|--ui-tab 2"
    # 焦点：按钮的 highlighted 与复选框的两张 highlighted 只有这一档进得了画。
    "notice-focus|advanced-graphics-notice|--ui-focus 1"
    "options-focus|options|--ui-focus 1"
)

shoot() {  # $1 = 输出根目录
    local root="$1"
    rm -rf "$root"
    "$SHOT" "$TARGETS" --scale "$SCALE" --out "$root/base" >/dev/null
    local entry label target extra
    for entry in "${VARIANTS[@]}"; do
        label="${entry%%|*}"
        target="${entry#*|}"; target="${target%%|*}"
        extra="${entry##*|}"
        # shellcheck disable=SC2086
        "$SHOT" "$target" --scale "$SCALE" --out "$root/variants/$label" $extra >/dev/null
    done
}

manifest() {  # $1 = 目录 -> stdout：`相对路径  sha256`，按路径排序
    (cd "$1" && find . -name '*.png' | sort | while IFS= read -r file; do
        printf '%s  %s\n' "${file#./}" "$(sha256sum "$file" | cut -d' ' -f1)"
    done)
}

if [[ "$MODE" == "record" ]]; then
    shoot "$DIR"
    manifest "$DIR" > "$DIR/manifest.sha256"
    echo "已记下 $(grep -c . "$DIR/manifest.sha256") 张基线到 $DIR"
    echo "★ 这套哈希只对本机这套驱动有意义；换机器整表都会不同，那不是回归。"
    exit 0
fi

if [[ ! -f "$DIR/manifest.sha256" ]]; then
    echo "$DIR 下没有 manifest.sha256 —— 先跑一次 $0 record" >&2
    exit 2
fi

FRESH="$(mktemp -d "${TMPDIR:-/tmp}/mc-ui-baseline.XXXXXX")"
trap 'rm -rf "$FRESH"' EXIT
shoot "$FRESH"
manifest "$FRESH" > "$FRESH/manifest.sha256"

if diff -q "$DIR/manifest.sha256" "$FRESH/manifest.sha256" >/dev/null; then
    echo "界面回归门禁通过：$(grep -c . "$DIR/manifest.sha256") 张图与基线逐字节相同。"
    exit 0
fi

echo "界面回归门禁**未通过**——下面这些图与基线不同："
# 逐行比：只在一边出现的（新增/删除的屏）也要报出来，这正是比清单而不是逐文件 cmp 的理由。
comm -3 <(sort "$DIR/manifest.sha256") <(sort "$FRESH/manifest.sha256") |
    awk '{print $1}' | sort -u | while IFS= read -r file; do
        if [[ -f "$DIR/$file" && -f "$FRESH/$file" ]]; then
            printf '  %-52s %s\n' "$file" \
                "$(python3 "$HERE/tools/shot_compare.py" diff "$DIR/$file" "$FRESH/$file" 2>&1 | tail -1)"
        elif [[ -f "$FRESH/$file" ]]; then
            printf '  %-52s %s\n' "$file" "只在新的一套里（新增的屏？）"
        else
            printf '  %-52s %s\n' "$file" "只在基线里（少拍了？）"
        fi
    done
echo "★ 想接受这些改动就重跑 $0 record；想看差在哪用 tools/shot_compare.py。"
exit 1
