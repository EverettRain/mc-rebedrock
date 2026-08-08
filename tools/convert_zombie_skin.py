#!/usr/bin/env python3
"""Convert the vanilla Java zombie skin into the Bedrock box-UV layout.

The Java 1.16 ``entity/zombie/zombie.png`` is a *classic* 64x32 skin: content
lives only in the top half, the left arm/leg are left empty (the model mirrors
the right limbs), and each limb's front/back faces sit 8px apart. The game's
box-UV pipeline instead samples a net whose front/back faces sit ``2*sz + sx``
apart (see ``animation::boxUvFaceRect``), so the two layouts cannot map 1:1.
This tool re-unwraps the classic skin into the standard Bedrock net the model
in resources/animation/zombie.geo.json samples:

  head 8x8x8 uv (0,0)   body 8x12x4 uv (16,16)
  arm  4x12x4 uv (40,16) leg  4x12x4 uv (0,16)   (left limbs mirror the right)

The target rects are computed with ``entity_uv_lib.face_rects`` (the single
source of truth for the box-UV net), so the output stays in lockstep with the
runtime shader. Run:

  python3 tools/convert_zombie_skin.py
"""
from pathlib import Path

from PIL import Image

from entity_uv_lib import face_rects

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "resources/vanilla/1.16.1/textures/minecraft/entity/zombie/zombie.png"
TARGET = ROOT / "resources/entity/zombie/zombie.png"

# Box-UV origins/sizes of the cubes in resources/animation/zombie.geo.json.
# The left arm/leg reuse the right-hand origins with mirror:true, so only the
# right-hand parts are painted; the left-hand net stays empty.
CUBES = {
    "head": ((0, 0), (8, 8, 8)),
    "body": ((16, 16), (8, 12, 4)),
    "arm": ((40, 16), (4, 12, 4)),
    "leg": ((0, 16), (4, 12, 4)),
}

# Source rects, in texels (x, y, w, h), from the classic Java layout. Face
# names follow entity_uv_lib: up/down/west/front/east/back.
#   head: top (8,0), bottom (16,0), right (0,8), front (8,8), back (16,8), left (24,8)
#   body: front is the teal shirt + green shoulder row; back and the sides are
#         taken from the solid teal shirt to avoid the transparent collar patch
#         at (16,16)-(20,20).
#   arm:  the right arm block is x40-48; its solid half (x44-48) carries the
#         teal sleeve and green hand, the sides reuse the same sleeve/hand.
#   leg:  the right leg block is x0-8; its solid half (x4-8) is the blue pants.
SRC = {
    "head": {
        "up": (8, 0, 8, 8),
        "down": (16, 0, 8, 8),
        "east": (0, 8, 8, 8),
        "front": (8, 8, 8, 8),
        "back": (16, 8, 8, 8),
        "west": (24, 8, 8, 8),
    },
    "body": {
        "up": (20, 16, 8, 4),
        "down": (20, 20, 8, 4),
        "east": (28, 20, 4, 12),
        "front": (20, 16, 8, 12),
        "back": (20, 20, 8, 12),
        "west": (16, 20, 4, 12),
    },
    "arm": {
        "up": (44, 16, 4, 4),
        "down": (44, 20, 4, 4),
        "east": (40, 20, 4, 12),
        "front": (44, 16, 4, 12),
        "back": (40, 20, 4, 12),
        "west": (44, 20, 4, 12),
    },
    "leg": {
        "up": (4, 16, 4, 4),
        "down": (4, 20, 4, 4),
        "east": (8, 20, 4, 12),
        "front": (4, 16, 4, 12),
        "back": (0, 20, 4, 12),
        "west": (4, 20, 4, 12),
    },
}


def main() -> int:
    source = Image.open(SOURCE).convert("RGBA")
    if source.size != (64, 64):
        raise SystemExit(f"expected a 64x64 skin, got {source.size}")
    out = Image.new("RGBA", (64, 64), (0, 0, 0, 0))

    for part, (uv, size) in CUBES.items():
        target_rects = face_rects(uv, size)
        for face, target in target_rects.items():
            src = SRC[part][face]
            crop = source.crop((*src[:2], src[0] + src[2], src[1] + src[3]))
            out.paste(crop, (int(target[0]), int(target[1])))

    out.save(TARGET)
    print(f"wrote {TARGET.relative_to(ROOT)} ({out.size[0]}x{out.size[1]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
