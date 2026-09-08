#!/usr/bin/env python3
"""读出图用的命令行（RN-34）。度量的理由都在 tools/shot_compare_lib.py 的抬头里。

    # ★ 拿到任何一张出图先做这一步：整张图相邻像素的最大跃变
    #   糊了是个位数（曾经 5/255），正常是几十（78/255）
    tools/shot_compare.py sharpness out/*/north-west-up.png

    # 两张图差多少：变化面积与变化幅度分开报
    tools/shot_compare.py diff before.png after.png

    # 一行的亮度剖面，看边缘过渡宽度
    tools/shot_compare.py profile after.png --row 378 --from-x 330 --to-x 400

    # 沿一条边平均，量垂直于它的剖面（纹理噪声大过被测现象时用这个）
    tools/shot_compare.py band shot.png --start 1454,708 --end 1224,1028 --grass

    # 裁一块放大，和把几张并排——读图时最常用的两件事
    tools/shot_compare.py crop shot.png zoom.png --at 215,235 --size 110,90 --scale 4
    tools/shot_compare.py sbs ab.png before.png after.png --at 215,235 --size 100,80 --scale 5

    # 解码器自检（PNG 五种行滤波器逐条验）
    tools/shot_compare.py --self-test
"""
import argparse
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from shot_compare_lib import (band_profile, compare, crop, luminance, max_adjacent_step,
                              read_png, row_profile, side_by_side, write_png)


def _pair(text):
    a, b = text.split(",")
    return int(a), int(b)


def _labels(paths):
    """去掉几条路径的公共前缀。出图的目录名是 `<out>/<很长的场景哈希>/<视角>.png`，
    只印 basename 的话三张对比图会印出三个一模一样的名字——那种输出没法读。"""
    if len(paths) == 1:
        return [paths[0]]
    prefix = 0
    shortest = min(len(path) for path in paths)
    while prefix < shortest and len({path[prefix] for path in paths}) == 1:
        prefix += 1
    prefix = paths[0].rfind("/", 0, prefix + 1) + 1
    return [path[prefix:] for path in paths]


def self_test():
    """PNG 的五种行滤波器逐条验一遍。

    ★ 这个自检不是形式主义：解码器的 Paeth/Average 分支写错时，解出来的仍然是"一张
    看得过去的图"，只是每个数字都是假的——而这个库的全部用途就是产生数字。仪器骗过
    我们一次了（RN-33 的出图模糊），不能再骗第二次。
    """
    import os
    import tempfile
    width, height, channels = 23, 17, 3
    original = [bytes(component for x in range(width)
                      for component in ((x * 7 + y * 13) % 256, (x * 3 + y * 5) % 256,
                                        (x + y * 29) % 256))
                for y in range(height)]

    # 逐行换一种滤波器编码，把五条分支全走到
    encoded = b""
    previous = bytearray(width * channels)
    for y, row in enumerate(original):
        method = y % 5
        line = bytearray(row)
        out = bytearray()
        for i in range(len(line)):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if method == 0:
                out.append(line[i])
            elif method == 1:
                out.append((line[i] - left) & 255)
            elif method == 2:
                out.append((line[i] - up) & 255)
            elif method == 3:
                out.append((line[i] - (left + up) // 2) & 255)
            else:
                estimate = left + up - upper_left
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upper_left))
                nearest = left if (da <= db and da <= dc) else (up if db <= dc else upper_left)
                out.append((line[i] - nearest) & 255)
        encoded += bytes([method]) + bytes(out)
        previous = line

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    path = os.path.join(tempfile.mkdtemp(), "filters.png")
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(encoded, 9)) + chunk(b"IEND", b""))

    got_width, got_height, got_channels, rows = read_png(path)
    assert (got_width, got_height, got_channels) == (width, height, channels), "尺寸/通道数不对"
    for y in range(height):
        assert rows[y] == original[y], f"第 {y} 行（滤波器 {y % 5}）解错了"

    # 写出去再读回来，确认 write_png 与 read_png 闭合
    round_trip = os.path.join(os.path.dirname(path), "roundtrip.png")
    write_png(round_trip, width, height, original)
    assert read_png(round_trip)[3] == original, "write_png/read_png 不闭合"

    # 度量本身：一张左黑右白的图，跃变必须正好是 255；纯色图必须是 0
    step_rows = [bytes((0, 0, 0) * 10 + (255, 255, 255) * 10) for _ in range(4)]
    write_png(round_trip, 20, 4, step_rows)
    image = read_png(round_trip)
    assert max_adjacent_step(image[3], 4, 20, image[2])[0] == 255, "硬边的跃变应当是 255"
    flat = [bytes((90, 90, 90) * 20) for _ in range(4)]
    write_png(round_trip, 20, 4, flat)
    image = read_png(round_trip)
    assert max_adjacent_step(image[3], 4, 20, image[2])[0] == 0, "纯色图的跃变应当是 0"
    print("shot_compare self-test ok（五种行滤波器 + 往返 + 跃变度量）")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="解码器与度量的自检")
    sub = parser.add_subparsers(dest="command")

    sharp = sub.add_parser("sharpness", help="整张图相邻像素的最大跃变")
    sharp.add_argument("files", nargs="+")

    diff = sub.add_parser("diff", help="两张图的差异")
    diff.add_argument("before")
    diff.add_argument("after")
    diff.add_argument("--threshold", type=float, default=40.0)

    profile = sub.add_parser("profile", help="一行的亮度剖面")
    profile.add_argument("files", nargs="+")
    profile.add_argument("--row", type=int, required=True)
    profile.add_argument("--from-x", type=int, required=True)
    profile.add_argument("--to-x", type=int, required=True)

    band = sub.add_parser("band", help="沿一条边平均，量垂直于它的剖面")
    band.add_argument("file")
    band.add_argument("--start", type=_pair, required=True, help="x,y")
    band.add_argument("--end", type=_pair, required=True, help="x,y")
    band.add_argument("--max-distance", type=int, default=140)
    band.add_argument("--step", type=int, default=4)
    band.add_argument("--grass", action="store_true", help="只取绿色像素（把土面剔掉）")

    crop_cmd = sub.add_parser("crop", help="裁一块并最近邻放大")
    crop_cmd.add_argument("file")
    crop_cmd.add_argument("out")
    crop_cmd.add_argument("--at", type=_pair, required=True)
    crop_cmd.add_argument("--size", type=_pair, required=True)
    crop_cmd.add_argument("--scale", type=int, default=1)

    sbs = sub.add_parser("sbs", help="把几张图的同一块并排")
    sbs.add_argument("out")
    sbs.add_argument("files", nargs="+")
    sbs.add_argument("--at", type=_pair, required=True)
    sbs.add_argument("--size", type=_pair, required=True)
    sbs.add_argument("--scale", type=int, default=1)

    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.command is None:
        parser.print_help()
        return 2

    if args.command == "sharpness":
        for path, label in zip(args.files, _labels(args.files)):
            width, height, channels, rows = read_png(path)
            biggest, (x, y) = max_adjacent_step(rows, height, width, channels)
            verdict = "★ 疑似被整体模糊过" if biggest < 20 else "看起来是锐的"
            print(f"{label[:48]:<48} 相邻像素最大跃变 {biggest:.0f}/255 @({x},{y})  {verdict}")
    elif args.command == "diff":
        result = compare(read_png(args.before), read_png(args.after), args.threshold)
        print(f"变化像素 {result['changed']}/{result['pixels']} "
              f"({100 * result['changed'] / result['pixels']:.2f}%)，"
              f"Δ>{args.threshold:.0f} 的 {result['big']}，"
              f"最大 Δ={result['max_delta']:.0f} @{result['at']}，"
              f"平均亮度 {result['mean_a']:.1f} → {result['mean_b']:.1f}")
    elif args.command == "profile":
        # 公共前缀已经剥掉，剩下的**开头**就是区分处，所以截前 24 个字符够用了
        labels = [label[:24] for label in _labels(args.files)]
        for path, label in zip(args.files, labels):
            _, _, channels, rows = read_png(path)
            values = row_profile(rows, channels, args.row, args.from_x, args.to_x)
            print(f"{label:<24}", " ".join(f"{v:3.0f}" for v in values))
    elif args.command == "band":
        width, height, channels, rows = read_png(args.file)
        keep = (lambda r, g, b: g >= r) if args.grass else None
        for distance, mean, count in band_profile(rows, height, width, channels, args.start,
                                                  args.end, args.max_distance, args.step,
                                                  keep=keep):
            print(f"  离边 {distance:3d} px  平均亮度 {mean:6.1f}  （{count} 个样本）")
    elif args.command == "crop":
        _, _, channels, rows = read_png(args.file)
        piece = crop(rows, channels, args.at[0], args.at[1], args.size[0], args.size[1],
                     args.scale)
        write_png(args.out, args.size[0] * args.scale, args.size[1] * args.scale, piece)
        print(f"wrote {args.out}")
    elif args.command == "sbs":
        panels = []
        for path in args.files:
            _, _, channels, rows = read_png(path)
            panels.append(crop(rows, channels, args.at[0], args.at[1], args.size[0],
                               args.size[1], args.scale))
        joined = side_by_side(panels)
        width = len(joined[0]) // 3
        write_png(args.out, width, len(joined), joined)
        print(f"wrote {args.out} ({width}x{len(joined)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
