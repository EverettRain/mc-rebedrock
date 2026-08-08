#!/usr/bin/env python3
"""Box-UV preview / skin-mapping tool for mc-rebedrock models.

Renders a Bedrock-style ``*.geo.json`` model with a texture applied through the
same box-UV unwrap the game uses, from several viewpoints, into one PNG montage.
It uses only numpy + Pillow (no Vulkan), so it is a fast, standalone way to check
how each cube face is mapped and oriented before touching the shader.

    # Simplest: just name the model; geo + texture + output are resolved for you.
    python3 tools/entity_uv_preview.py pig

    # Or point at files explicitly:
    python3 tools/entity_uv_preview.py --geo path/to.geo.json --texture skin.png --out out.png

On first run it creates a private virtualenv under ``tools/.preview-venv`` and
installs numpy + Pillow, then re-executes itself inside it — no manual setup.

The box-UV convention is the single source of truth: the geometry and net math
live in ``entity_uv_lib.py`` (mirrored in ``boxUvFaceRect`` /
``item_entity.vert`` / the texture-editor frontend), and this file is just a
thin CLI wrapper around it.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


def _venv_python(venv):
    """The python executable inside `venv`, whatever it is named. venv layouts
    differ: some create bin/python, some only bin/python3 or bin/python3.x."""
    if os.name == "nt":
        candidates = [venv / "Scripts" / "python.exe"]
    else:
        candidates = [venv / "bin" / "python", venv / "bin" / "python3"]
        candidates += sorted(venv.glob("bin/python3.*"))
    return next((c for c in candidates if c.exists()), None)


def _bail_manual(reason):
    sys.exit(f"[entity_uv_preview] {reason}.\n"
             f"Install the two dependencies into your Python and re-run:\n"
             f"    {sys.executable} -m pip install numpy pillow\n"
             f"(on a PEP 668 'externally managed' Python, add --break-system-packages)")


def _ensure_deps():
    """Import numpy + Pillow, bootstrapping a private venv on first run so the
    tool works out of the box on a fresh (PEP 668 "externally managed") Python."""
    try:
        import numpy  # noqa: F401
        import PIL  # noqa: F401
        return
    except ImportError:
        pass
    venv = Path(__file__).resolve().parent / ".preview-venv"
    # Detect "already running inside our venv" via sys.prefix (the venv root), not
    # the executable path — venv pythons are symlinks to the base interpreter, so
    # resolving the exe path would false-match and we would bail on every reuse.
    if Path(sys.prefix).resolve() == venv.resolve():
        sys.exit("numpy/Pillow still missing inside tools/.preview-venv — "
                 "delete that folder and re-run, or install them yourself.")

    py = _venv_python(venv)
    if py is None:  # no usable venv yet (or a broken/partial one) — (re)create it
        import shutil
        shutil.rmtree(venv, ignore_errors=True)
        print("[entity_uv_preview] one-time setup: creating virtualenv...", file=sys.stderr)
        try:
            subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
        except (subprocess.CalledProcessError, OSError) as exc:
            _bail_manual(f"could not create a virtualenv ({exc})")
        py = _venv_python(venv)
        if py is None:
            _bail_manual(f"the virtualenv at {venv} has no python executable")

    # Make sure the deps are actually present in the venv (a prior run may have
    # created the venv but failed before installing).
    have = subprocess.call([str(py), "-c", "import numpy, PIL"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    if not have:
        print("[entity_uv_preview] installing numpy + Pillow (needs network)...", file=sys.stderr)
        try:
            subprocess.check_call([str(py), "-m", "pip", "install", "-q", "numpy", "pillow"])
        except (subprocess.CalledProcessError, OSError) as exc:
            _bail_manual(f"could not install numpy/pillow ({exc})")
    os.execv(str(py), [str(py), str(Path(__file__).resolve()), *sys.argv[1:]])


_ensure_deps()

import numpy as np  # noqa: E402

from entity_uv_lib import (  # noqa: E402
    available_models, bone_index, find_texture, parse_geo, repo_root,
    resolve_geo, resolve_texture,
)
from entity_uv_render import build_faces, render, render_net  # noqa: E402


def main():
    ap = argparse.ArgumentParser(
        description="Box-UV model preview. Name a model (e.g. 'pig') to auto-resolve "
                    "its geometry, texture and output paths, or pass files explicitly.")
    ap.add_argument("model", nargs="?", help="model name, e.g. 'pig'")
    ap.add_argument("--geo", help="override geometry .geo.json path")
    ap.add_argument("--texture", help="override texture .png path")
    ap.add_argument("--out", help="override preview output .png path")
    ap.add_argument("--net", nargs="?", const="AUTO",
                    help="also write a UV-net overlay (default path unless one is given)")
    ap.add_argument("--res", type=int, default=360)
    args = ap.parse_args()

    root = repo_root()
    if not args.model and not (args.geo and args.texture):
        ap.error("give a model name (e.g. 'pig'), or both --geo and --texture")

    geo_path = Path(args.geo) if args.geo else resolve_geo(root, args.model)
    if args.texture:
        tex_path = Path(args.texture)
    else:
        tex_path = args.model and find_texture(root, args.model)
        if tex_path is None:
            tex_path = resolve_texture(root, args.model)
    stem = args.model or geo_path.stem.replace(".geo", "")
    out_dir = root / "tools" / "preview-out"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = Path(args.out) if args.out else out_dir / f"{stem}_preview.png"
    # The net overlay is written by default (name mode); suppress with --net "".
    if args.net == "AUTO" or (args.net is None and not args.out):
        net_path = out_dir / f"{stem}_net.png"
    else:
        net_path = Path(args.net) if args.net else None

    print(f"geo:     {geo_path}\ntexture: {tex_path}")
    geo, tw, th, _identifier = parse_geo(geo_path)
    bones, order = bone_index(geo)
    faces = build_faces(bones, order, tw, th)
    from PIL import Image
    texture = Image.open(tex_path)

    if net_path is not None:
        render_net(bones, order, tw, th, texture).save(net_path)
        print(f"wrote {net_path}  (UV-net overlay)")

    # A montage of labelled viewpoints so every face is checkable at a glance.
    views = [("front", 0, 0), ("back", 180, 0), ("left", 90, 0),
             ("right", -90, 0), ("top", 0, 89), ("3/4", -35, 22)]
    tiles = [(label, render(faces, texture, yaw, pitch, args.res))
             for label, yaw, pitch in views]

    from PIL import ImageDraw
    cols, pad = 3, 6
    rows = (len(tiles) + cols - 1) // cols
    r = args.res
    sheet = Image.new("RGB", (cols * r + (cols + 1) * pad, rows * (r + 18) + pad),
                      (15, 16, 20))
    draw = ImageDraw.Draw(sheet)
    for i, (label, tile) in enumerate(tiles):
        cx = pad + (i % cols) * (r + pad)
        cy = pad + (i // cols) * (r + 18)
        draw.text((cx + 2, cy), label, fill=(230, 230, 230))
        sheet.paste(tile, (cx, cy + 14))
    sheet.save(out_path)
    print(f"wrote {out_path}  ({tw}x{th} texture, {len(faces)} faces)")


if __name__ == "__main__":
    main()
