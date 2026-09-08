#!/usr/bin/env python3
"""出图对比库：无依赖读写 PNG，外加几个「读图」时反复要用的度量（RN-34）。

为什么需要它
------------
本仓所有视觉验收都建立在 ``tools/export_block_preview.sh`` 出的八角 PNG 上，而
「看一眼图」得不出结论——RN-30 到 RN-34 这一串里，每一条真正定性的证据都是**量**
出来的，不是看出来的：

  * RN-33：出图**整批是糊的**，而且没人发现。判据是「整张图相邻像素的最大跃变」：
    糊的时候 5/255，正常 78/255。肉眼只觉得「渲染得比较柔」。
  * RN-34：墙根那条亮带，单条像素剖面会被草纹理的棋盘噪声（±10 个亮度级）淹没，
    **沿边缘方向平均 60 个样本**才出信号。而「比两侧亮 25 以上」的细亮脊检测把它
    整个漏掉了——它是 10 级摊在 24 像素上的梯度。

**「肉眼很明显」和「脚本抓得到」是两件事**，阈值要按现象定。这个库把上面那几种量法
固定下来，省得每次重写，也省得每次重新踩一遍"仪器本身在骗人"。

只支持 8 位、非隔行、颜色类型 2（RGB）与 6（RGBA）的 PNG——`export_block_preview.sh`
和 `export_ui_screens.sh` 出的就是这两种。遇到别的一律显式报错，**不要**默默给出
一个看起来合理的错数字。
"""
import struct
import zlib

__all__ = [
    "read_png", "write_png", "luminance", "max_adjacent_step", "compare",
    "row_profile", "band_profile", "crop", "side_by_side",
]


def read_png(path):
    """→ (width, height, channels, rows)；rows[y] 是一行原始字节。"""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} 不是 PNG")
    offset = 8
    width = height = None
    colour_type = bit_depth = interlace = None
    idat = b""
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, bit_depth, colour_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk[:13])
        elif kind == b"IDAT":
            idat += chunk
        offset += 12 + length
    if bit_depth != 8 or colour_type not in (2, 6) or interlace != 0:
        raise ValueError(
            f"{path}: 只支持 8 位、非隔行、颜色类型 2/6 的 PNG"
            f"（实际 depth={bit_depth} colour={colour_type} interlace={interlace}）")

    channels = 4 if colour_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    cursor = 0
    for _ in range(height):
        method = raw[cursor]
        cursor += 1
        line = bytearray(raw[cursor:cursor + stride])
        cursor += stride
        # PNG 的五种逐行滤波器。写错任何一条，解出来的图都还是"一张看得过去的图"，
        # 只是数字全是假的——所以 shot_compare.py --self-test 逐条验过它们。
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if method == 1:
                line[i] = (line[i] + left) & 255
            elif method == 2:
                line[i] = (line[i] + up) & 255
            elif method == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif method == 4:
                estimate = left + up - upper_left
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upper_left))
                nearest = left if (da <= db and da <= dc) else (up if db <= dc else upper_left)
                line[i] = (line[i] + nearest) & 255
            elif method != 0:
                raise ValueError(f"{path}: 未知的行滤波器 {method}")
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def write_png(path, width, height, rgb_rows):
    """写一张颜色类型 2 的 PNG。rgb_rows[y] 是 width*3 字节。"""
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def luminance(rows, channels, x, y):
    offset = x * channels
    row = rows[y]
    return (row[offset] + row[offset + 1] + row[offset + 2]) / 3.0


def max_adjacent_step(rows, height, width, channels):
    """整张图里**水平相邻**两像素的最大亮度跃变，以及它出现的位置。

    ★ 拿到任何一张出图先跑这一步。一张 3D 渲染图必然有几何轮廓，天空与石地板的交界
    应当是一两个像素的硬边；跃变只有个位数就说明画面被什么东西整体糊过。
    实测：被菜单模糊糊掉时是 **5**，正常是 **78**。
    """
    biggest = 0
    where = (0, 0)
    for y in range(height):
        for x in range(1, width):
            step = abs(luminance(rows, channels, x - 1, y) - luminance(rows, channels, x, y))
            if step > biggest:
                biggest = step
                where = (x, y)
    return biggest, where


def compare(a, b, threshold=40):
    """两张同尺寸图的差异。→ dict(changed, big, max_delta, at, total, mean_a, mean_b)。

    `big` 数的是超过 `threshold` 的像素：**变化面积**和**变化幅度**要分开看。
    一次「大面积但每个像素都没跳过 40」的改动（RN-32 的树叶 AO：7.2% 的像素变了而
    Δ>40 的一个都没有）与一次「小面积但很扎眼」的改动（RN-32 的楼梯阴影：491 个
    Δ>40）是完全不同的两件事，只报总差异会把它们混成一个数。
    """
    (width, height, channels, rows_a) = a
    (width_b, height_b, channels_b, rows_b) = b
    if (width, height) != (width_b, height_b):
        raise ValueError(f"尺寸不同：{width}x{height} vs {width_b}x{height_b}")
    changed = big = 0
    total = 0.0
    sum_a = sum_b = 0.0
    biggest = 0.0
    at = (0, 0)
    for y in range(height):
        for x in range(width):
            la = luminance(rows_a, channels, x, y)
            lb = luminance(rows_b, channels_b, x, y)
            sum_a += la
            sum_b += lb
            delta = abs(la - lb)
            if delta >= 1.0:
                changed += 1
                total += delta
            if delta > threshold:
                big += 1
            if delta > biggest:
                biggest = delta
                at = (x, y)
    pixels = width * height
    return {"changed": changed, "big": big, "max_delta": biggest, "at": at,
            "total": total, "pixels": pixels,
            "mean_a": sum_a / pixels, "mean_b": sum_b / pixels}


def row_profile(rows, channels, y, x0, x1):
    """一行的亮度剖面。看边缘过渡宽度用；**只在被测现象强于纹理噪声时才可信**。"""
    return [luminance(rows, channels, x, y) for x in range(x0, x1)]


def band_profile(rows, height, width, channels, start, end, max_distance=140, step=2,
                 samples=60, keep=None):
    """沿一条线（`start`→`end`）平均，量**垂直于它**的亮度剖面。

    这是 RN-34 找到墙根那条亮带的量法：单条剖面被草纹理的棋盘噪声（±10 级）淹没，
    沿边缘方向平均几十个样本之后信号立刻干净。

    `keep(r, g, b) -> bool` 可以筛掉不属于被测表面的样本（例如量地面时把墙面的
    土色像素剔掉：`lambda r, g, b: g >= r`）。
    → [(距离, 平均亮度, 样本数), ...]
    """
    ax, ay = start
    bx, by = end
    length = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
    if length == 0:
        raise ValueError("start 与 end 不能是同一点")
    lx, ly = (bx - ax) / length, (by - ay) / length
    px, py = -ly, lx          # 垂直方向
    if px < 0:                # 固定指向 +x 那一侧，免得两次调用的符号不一致
        px, py = -px, -py
    out = []
    for distance in range(0, max_distance, step):
        values = []
        for k in range(samples):
            travel = length * k / max(samples - 1, 1)
            x = int(round(ax + lx * travel + px * distance))
            y = int(round(ay + ly * travel + py * distance))
            if not (0 <= x < width and 0 <= y < height):
                continue
            offset = x * channels
            r, g, b = rows[y][offset], rows[y][offset + 1], rows[y][offset + 2]
            if keep is not None and not keep(r, g, b):
                continue
            values.append((r + g + b) / 3.0)
        if values:
            out.append((distance, sum(values) / len(values), len(values)))
    return out


def crop(rows, channels, x0, y0, width, height, scale=1):
    """裁一块并按整数倍最近邻放大。放大用最近邻，**不要**插值——插值会伪造出
    一条本来不存在的渐变，而我们量的正是渐变。"""
    out = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            offset = (x0 + x) * channels
            line += rows[y0 + y][offset:offset + 3] * scale
        for _ in range(scale):
            out.append(bytes(line))
    return out


def side_by_side(panels, gap=8, gap_colour=(32, 32, 32)):
    """把若干张等高的 RGB 图横排成一张，中间留一条分隔线。"""
    height = len(panels[0])
    if any(len(panel) != height for panel in panels):
        raise ValueError("并排的图必须等高")
    separator = bytes(gap_colour) * gap
    return [b"".join(panel[y] if index == 0 else separator + panel[y]
                     for index, panel in enumerate(panels)) for y in range(height)]
