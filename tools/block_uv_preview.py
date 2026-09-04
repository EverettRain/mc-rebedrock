#!/usr/bin/env python3
"""Offline UV preview for BLOCK models (RN-13-3).

Renders a block's baked model with its real textures, from several viewpoints,
plus a UV-net overlay — and, side by side, the same picture built independently
from the vanilla model json. The sibling of tools/entity_uv_preview.py, which
does this for entity box-UV models; this one covers the model-json side of the
world (doors, trapdoors, fence gates, diodes, the lever, the anvil).

Why: a defect of the form "every number is right and the texture still faces the
wrong way" is invisible to headless assertions and has cost a Mac round trip
every time it turned up. Two pictures from two independent readings of the same
json settle it in this container.

    # Our baked model against vanilla's, for an open trapdoor facing north:
    python3 tools/block_uv_preview.py oak_trapdoor --state open=true \\
        --state facing=north --pack /path/to/your/resourcepack

    # The whole closed/open x four-facings sheet:
    python3 tools/block_uv_preview.py oak_trapdoor --sweep --pack /path/to/pack

TEXTURES COME FROM THE PACK YOU NAME. Nothing from Mojang is bundled with this
repository, copied into resources/, or cached anywhere under it — pass the path
to a resource pack you already have. `--pack` may also come from
MC_REBEDROCK_RESOURCE_PACK.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def _venv_python(venv):
    if os.name == "nt":
        candidates = [venv / "Scripts" / "python.exe"]
    else:
        candidates = [venv / "bin" / "python", venv / "bin" / "python3"]
        candidates += sorted(venv.glob("bin/python3.*"))
    return next((c for c in candidates if c.exists()), None)


def _bail_manual(reason):
    sys.exit(f"[block_uv_preview] {reason}.\n"
             f"Install the two dependencies into your Python and re-run:\n"
             f"    {sys.executable} -m pip install numpy pillow\n"
             f"(on a PEP 668 'externally managed' Python, add --break-system-packages)")


def _ensure_deps():
    """Import numpy + Pillow, bootstrapping a private venv on first run — the
    same one entity_uv_preview.py uses, so the setup cost is paid once."""
    try:
        import numpy  # noqa: F401
        import PIL  # noqa: F401
        return
    except ImportError:
        pass
    venv = Path(__file__).resolve().parent / ".preview-venv"
    if Path(sys.prefix).resolve() == venv.resolve():
        sys.exit("numpy/Pillow still missing inside tools/.preview-venv — "
                 "delete that folder and re-run, or install them yourself.")
    py = _venv_python(venv)
    if py is None:
        import shutil
        shutil.rmtree(venv, ignore_errors=True)
        print("[block_uv_preview] one-time setup: creating virtualenv...", file=sys.stderr)
        try:
            subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
        except (subprocess.CalledProcessError, OSError) as exc:
            _bail_manual(f"could not create a virtualenv ({exc})")
        py = _venv_python(venv)
        if py is None:
            _bail_manual(f"the virtualenv at {venv} has no python executable")
    have = subprocess.call([str(py), "-c", "import numpy, PIL"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    if not have:
        print("[block_uv_preview] installing numpy + Pillow (needs network)...", file=sys.stderr)
        try:
            subprocess.check_call([str(py), "-m", "pip", "install", "-q", "numpy", "pillow"])
        except (subprocess.CalledProcessError, OSError) as exc:
            _bail_manual(f"could not install numpy/pillow ({exc})")
    os.execv(str(py), [str(py), str(Path(__file__).resolve()), *sys.argv[1:]])


_ensure_deps()

sys.path.insert(0, str(Path(__file__).resolve().parent))

import block_uv_lib as lib  # noqa: E402

# Labelled by the face each viewpoint actually shows, not by "front"/"left":
# which side of a block is its front is a question about the block, and a wrong
# label here is how a mirrored model gets called correct.
VIEWS = [("south", 0, 0), ("north", 180, 0), ("west", 90, 0),
         ("east", -90, 0), ("top", 0, 89), ("3/4", -35, 22)]


def repo_root():
    return Path(__file__).resolve().parent.parent


def find_dumper(explicit):
    """The block_model_dump binary. Built by -DMC_REBEDROCK_BUILD_TOOLS=ON."""
    if explicit:
        return Path(explicit)
    root = repo_root()
    found = sorted(root.glob("build/*/**/mc_rebedrock_block_model_dump"))
    if not found:
        sys.exit("mc_rebedrock_block_model_dump not built — configure with "
                 "-DMC_REBEDROCK_BUILD_TOOLS=ON and build that target, or pass --dumper")
    return found[0]


def run_dumper(dumper, block, state):
    command = [str(dumper), "--block", block]
    for key, value in state.items():
        command += ["--state", f"{key}={value}"]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{dumper} failed: {result.stderr.strip()}")
    # The runtime prints a spawn-table notice on stderr; stdout is pure JSON.
    return json.loads(result.stdout)


def collect_sprites(pack, quads):
    sprites = {}
    for quad in quads:
        name = quad["sprite"]
        if name not in sprites:
            sprites[name] = lib.load_sprite(pack, name)
    return sprites


def render_row(quads, sprites, res, shaded):
    return [(label, lib.render(quads, sprites, yaw, pitch, res, shaded=shaded))
            for label, yaw, pitch in VIEWS]


def build_sheet(rows, res, title_height=18):
    """rows = [(row label, [(view label, image), ...]), ...]"""
    from PIL import Image, ImageDraw
    pad = 6
    columns = max(len(tiles) for _, tiles in rows)
    width = columns * res + (columns + 1) * pad
    height = len(rows) * (res + title_height * 2) + pad
    sheet = Image.new("RGB", (width, height), (15, 16, 20))
    draw = ImageDraw.Draw(sheet)
    y = pad
    for label, tiles in rows:
        draw.text((pad, y), label, fill=(255, 220, 120))
        y += title_height
        for column, (view, tile) in enumerate(tiles):
            x = pad + column * (res + pad)
            draw.text((x + 2, y), view, fill=(210, 210, 210))
            sheet.paste(tile, (x, y + title_height - 4))
        y += res + title_height
    return sheet


def state_for_vanilla(block, state):
    """The blockstate property dict a vanilla blockstate json is keyed by."""
    resolved = {"facing": state.get("facing", "north"),
                "half": state.get("half", "bottom"),
                "open": state.get("open", "false"),
                "powered": state.get("powered", "false"),
                "hinge": state.get("hinge", "left"),
                "in_wall": state.get("in_wall", "false"),
                "locked": state.get("locked", "false"),
                "delay": state.get("delay", "1"),
                "mode": state.get("mode", "compare")}
    if block.endswith("door") and not block.endswith("trapdoor"):
        resolved["half"] = "upper" if state.get("half") in ("top", "upper") else "lower"
    return resolved


def icon_case(args, dumper, pack, out_dir):
    """RN-14: the inventory icon — ours beside the vanilla item model."""
    result = subprocess.run([str(dumper), "--block", args.block, "--item"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{dumper} --item failed: {result.stderr.strip()}")
    dump = json.loads(result.stdout)
    if dump["itemSprite"]:
        print(f"  {args.block}'s item is a flat sprite (item/{dump['itemSprite']}.png), "
              f"not a model — nothing to draw here")
        return
    if dump["boxCount"] == 0:
        print(f"  {args.block}'s item is a flat sprite — nothing to draw here")
        return

    sprites = {}
    for box in dump["iconBoxes"]:
        for face in box["faces"]:
            sprites.setdefault(face["sprite"], lib.load_sprite(pack, face["sprite"]))
    missing = [n for n, v in sprites.items() if v is None]
    if missing:
        print(f"  ! no texture in the pack for: {', '.join(sorted(missing))}", file=sys.stderr)

    ours = lib.render_icon(dump["iconBoxes"], sprites, args.res, shaded=not args.no_shade)
    rows = [(f"rebedrock  {args.block} icon  ({dump['boxCount']} box(es), "
             f"turn {dump['iconTurn']})", [("gui", ours)])]

    vanilla_name = dump["vanilla"].split(":")[-1] or args.block
    if not args.no_vanilla:
        try:
            quads, model = lib.quads_from_item_model(pack, vanilla_name)
            reference_sprites = collect_sprites(pack, quads)
            # vanilla's gui display transform is rotation [30, 225, 0]; in this
            # renderer's orbit that is yaw 135 / pitch 30 (it looks along -Z, the
            # transform turns the model).
            tile = lib.render(quads, reference_sprites, 135.0, 30.0, args.res,
                              shaded=not args.no_shade)
            rows.append((f"vanilla    {model}  gui [30, 225, 0]", [("gui", tile)]))
        except (SystemExit, FileNotFoundError, json.JSONDecodeError) as exc:
            print(f"  ! vanilla side unavailable: {exc}", file=sys.stderr)

    sheet = build_sheet(rows, args.res)
    out = out_dir / f"{args.block}_icon.png"
    sheet.save(out)
    print(f"wrote {out}")


def one_case(args, dumper, pack, state, out_dir, tag):
    dump = run_dumper(dumper, args.block, state)
    ours = lib.quads_from_dump(dump)
    rows = []
    sprites = collect_sprites(pack, ours)
    missing = [n for n, s in sprites.items() if s is None]
    if missing:
        print(f"  ! no texture in the pack for: {', '.join(sorted(missing))}", file=sys.stderr)
    rows.append((f"rebedrock  {args.block} {tag}", render_row(ours, sprites, args.res,
                                                              not args.no_shade)))

    vanilla_name = dump["vanilla"].split(":")[-1] or args.block
    if not args.no_vanilla:
        try:
            if args.vanilla_model:
                variant = {"model": args.vanilla_model, "x": args.x, "y": args.y}
            else:
                variant = lib.blockstate_variant(pack, vanilla_name,
                                                 state_for_vanilla(vanilla_name, state))
            reference = lib.quads_from_vanilla(pack, variant["model"], variant["y"], variant["x"])
            reference_sprites = collect_sprites(pack, reference)
            rows.append((f"vanilla    {variant['model']}  y={variant['y']:g} x={variant['x']:g}",
                         render_row(reference, reference_sprites, args.res, not args.no_shade)))
            # The verdict is this, not the pixels: geometry-paired, corner for
            # corner. The sprite NAME is compared separately below, since two
            # models can agree on every coordinate and still bind a different
            # texture (the diode `_on` top plate did exactly that).
            problems = lib.compare_quads(ours, reference)
            ours_sprites = sorted({q["sprite"].split("/")[-1] for q in ours})
            reference_names = sorted({q["sprite"].split("/")[-1] for q in reference})
            if ours_sprites != reference_names:
                problems.append(f"sprites {ours_sprites} vs vanilla {reference_names}")
            if problems:
                print(f"  GEOMETRY/UV DIFFERS from vanilla ({len(problems)}):")
                for problem in problems[:12]:
                    print(f"    - {problem}")
            else:
                print("  geometry, UV and sprites match the vanilla model exactly")
        except (SystemExit, FileNotFoundError, json.JSONDecodeError) as exc:
            print(f"  ! vanilla side unavailable: {exc}", file=sys.stderr)
            reference = None

    # The whole point of drawing the two rows: say, as a number, whether they
    # agree. A defect of the "everything checks out and it still looks wrong"
    # kind shows up here as a non-zero difference on one view.
    # Secondary, and informational only: how the two rows differ as pixels. It is
    # not the verdict — the rasteriser can tie-break differently on an edge-on
    # view and report a hundredth of a grey level that means nothing. The verdict
    # is the geometry/UV/sprite comparison printed above.
    if len(rows) == 2:
        import numpy as np
        for (view, ours_tile), (_, reference_tile) in zip(rows[0][1], rows[1][1]):
            difference = np.abs(np.asarray(ours_tile, float) -
                                np.asarray(reference_tile, float)).mean()
            marker = "   " if difference < 0.5 else " ! "
            print(f"{marker}{tag:<24} {view:<6} pixel mean|difference| = {difference:.4f}")

    sheet = build_sheet(rows, args.res)
    out = out_dir / f"{args.block}_{tag}_preview.png"
    sheet.save(out)
    print(f"wrote {out}")

    net = lib.render_net(ours, sprites)
    if net is not None:
        out_net = out_dir / f"{args.block}_{tag}_net.png"
        net.save(out_net)
        print(f"wrote {out_net}  (UV rects, first corner marked red)")


def main():
    parser = argparse.ArgumentParser(
        description="Offline UV preview for block models: ours vs the vanilla json.")
    parser.add_argument("block", help="block name, e.g. oak_trapdoor")
    parser.add_argument("--state", action="append", default=[],
                        help="a blockstate property, e.g. --state open=true (repeatable)")
    parser.add_argument("--sweep", action="store_true",
                        help="render open=false/true x the four facings")
    parser.add_argument("--icon", action="store_true",
                        help="render the INVENTORY ICON (RN-14) instead of the world model")
    parser.add_argument("--pack", default=os.environ.get("MC_REBEDROCK_RESOURCE_PACK"),
                        help="root of YOUR resource pack (the folder holding assets/)")
    parser.add_argument("--dumper", help="path to mc_rebedrock_block_model_dump")
    parser.add_argument("--out", help="output directory (default tools/preview-out)")
    parser.add_argument("--vanilla-model", help="override the vanilla model reference")
    parser.add_argument("--x", type=float, default=0.0, help="blockstate x rotation for --vanilla-model")
    parser.add_argument("--y", type=float, default=0.0, help="blockstate y rotation for --vanilla-model")
    parser.add_argument("--no-vanilla", action="store_true", help="skip the vanilla comparison row")
    parser.add_argument("--no-shade", action="store_true",
                        help="draw every face at full brightness (ignore CardinalLighting)")
    parser.add_argument("--res", type=int, default=300)
    args = parser.parse_args()

    if not args.pack:
        parser.error("--pack (or MC_REBEDROCK_RESOURCE_PACK) must point at a resource pack; "
                     "this repository ships no Mojang textures")
    pack = Path(args.pack)
    if not (pack / "assets").is_dir():
        parser.error(f"{pack} has no assets/ directory")

    base_state = {}
    for entry in args.state:
        key, _, value = entry.partition("=")
        base_state[key] = value

    dumper = find_dumper(args.dumper)
    out_dir = Path(args.out) if args.out else repo_root() / "tools" / "preview-out"
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.icon:
        icon_case(args, dumper, pack, out_dir)
        return
    if args.sweep:
        for opened in ("false", "true"):
            for facing in ("north", "east", "south", "west"):
                state = dict(base_state, open=opened, facing=facing)
                one_case(args, dumper, pack, state, out_dir, f"{facing}_open{opened}")
    else:
        tag = "_".join(f"{k}{v}" for k, v in sorted(base_state.items())) or "default"
        one_case(args, dumper, pack, base_state, out_dir, tag)


if __name__ == "__main__":
    main()
