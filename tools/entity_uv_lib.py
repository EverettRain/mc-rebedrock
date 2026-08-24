#!/usr/bin/env python3
"""Shared, dependency-free box-UV geometry and IO helpers for mc-rebedrock.

This module is the SINGLE SOURCE OF TRUTH for the entity box-UV net convention:
``face_rects()`` must stay in exact agreement with ``animation::boxUvFaceRect``
(src/animation/SkeletalModel.cpp), the box-UV block of
``resources/shaders/src/item_entity.vert``, and the box-UV math in
``tools/texture_editor/static/app.js``. Keep them in lockstep.

Nothing here imports numpy or Pillow at module scope, so the web editor server
(tools/texture_editor/server.py) can import it on a stock interpreter. The
software rasterizer that needs numpy/Pillow lives in ``entity_uv_render.py``.

Run ``python3 tools/entity_uv_lib.py`` to self-test against the exact values
pinned by tests/box_uv_test.cpp.

LOCAL RESEARCH TOOL ONLY. Its ``--texture`` resolution can point at a locally
extracted vanilla tree, and nothing it produces may be committed to
``resources/`` or shipped: ReBedrock distributes no Mojang-derived art, and
entity skins come from the player's resource pack (see
docs/wait-for-opus/resourcepack-distribution-cleanup.md).
"""
import json
import math
import re
import struct
import sys
from pathlib import Path

# Face name -> (normal axis, (TL, TR, BR, BL) cube-corner codes). Corners are
# (x, y, z) in {0, 1} on the cube; y is texture-down. This is the convention the
# shader mirrors (face codes 0..5 = east, west, up, down, back, front).
FACES = {
    "front": ((0, 0, -1), ((1, 1, 0), (0, 1, 0), (0, 0, 0), (1, 0, 0))),
    "back":  ((0, 0, 1),  ((0, 1, 1), (1, 1, 1), (1, 0, 1), (0, 0, 1))),
    "east":  ((1, 0, 0),  ((1, 1, 1), (1, 1, 0), (1, 0, 0), (1, 0, 1))),
    "west":  ((-1, 0, 0), ((0, 1, 0), (0, 1, 1), (0, 0, 1), (0, 0, 0))),
    "up":    ((0, 1, 0),  ((0, 1, 1), (1, 1, 1), (1, 1, 0), (0, 1, 0))),
    "down":  ((0, -1, 0), ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1))),
}

# Face labels + outline colours for the UV-net overlay, shared with the frontend.
NET_STYLE = {
    "front": ("F", (80, 200, 120)), "back": ("B", (200, 90, 90)),
    "east": ("E", (90, 150, 230)), "west": ("W", (230, 190, 80)),
    "up": ("U", (200, 120, 220)), "down": ("D", (120, 200, 220)),
}

# The runtime loader's defaults when a geometry omits its texture declaration
# (SkeletalModel::loadGeometry uses 16x16). Tools must not invent a friendlier
# default: a silently different number here means the editor previews a mapping
# the game will never produce.
DEFAULT_TEXTURE_WIDTH = 16
DEFAULT_TEXTURE_HEIGHT = 16

# Fragment shaders discard entity texels below this alpha (item_entity.frag uses
# `texel.a < 0.1` on normalised alpha).
ALPHA_CUTOFF = 0.1 * 255.0


def rot_matrix(deg):
    """Bedrock euler rotation matrix for `deg` = (x, y, z) degrees.

    Bedrock stacks the axes Z, then Y, then X, so the matrix is Rz @ Ry @ Rx and
    a point is rotated about X first. This must match ``animation::rotationMatrix``
    (src/animation/SkeletalModel.cpp) and ``rotMatrix()`` in the texture editor —
    composing them in the opposite order silently poses multi-axis bones
    differently from the game.
    """
    import numpy as np  # lazy: only the 3D preview path needs it
    rx, ry, rz = (math.radians(d) for d in deg)
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    mx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]], float)
    my = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], float)
    mz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]], float)
    return mz @ my @ mx


def load_json(path):
    """Read a JSON document, tolerating `//` comments (Bedrock files sometimes
    carry them)."""
    with open(path) as handle:
        text = re.sub(r"//.*", "", handle.read())
    return json.loads(text)


def parse_geo(path):
    """Parse a Bedrock `*.geo.json` file. See :func:`parse_geo_document`."""
    return parse_geo_document(load_json(path), path)


def parse_geo_document(doc, what="document"):
    """Parse an already-loaded Bedrock geometry document.

    Returns (geometry_dict, tw, th, identifier). The first geometry entry in
    ``minecraft:geometry`` is used, matching the runtime loader when no
    identifier is requested (SkeletalModel::loadGeometry). Missing texture
    dimensions fall back to the loader's own defaults, not to a nicer-looking
    guess, so the editor previews what the game will actually sample.
    """
    geometries = doc["minecraft:geometry"]
    if not geometries:
        raise ValueError(f"{what}: empty 'minecraft:geometry' array")
    geo = geometries[0]
    tw, th = declared_texture_size(geo)
    return geo, tw, th, geo.get("description", {}).get("identifier", "")


def dumps_geo(doc, indent=2, width=110):
    """Serialise a geometry document the way the project authors them.

    ``json.dumps(indent=2)`` explodes every ``[x, y, z]`` onto three lines and
    turns a one-line cube into eleven, so a one-texel uv tweak lands as a
    thousand-line diff. This keeps the hand-written shape: structure is
    indented, but scalar arrays stay inline and a cube-like object (a list entry
    whose values are all scalars or scalar arrays) stays on one line.
    """
    def scalar(value):
        return value is None or isinstance(value, (bool, int, float, str))

    def inline(value):
        return json.dumps(value, ensure_ascii=False, separators=(", ", ": "))

    def inlinable_list(value):
        return isinstance(value, list) and all(scalar(item) for item in value)

    def render(value, depth, in_list):
        pad = " " * (indent * depth)
        inner = " " * (indent * (depth + 1))
        if scalar(value):
            return inline(value)
        if isinstance(value, list):
            if not value:
                return "[]"
            if inlinable_list(value):
                return inline(value)
            items = [inner + render(item, depth + 1, True) for item in value]
            return "[\n" + ",\n".join(items) + "\n" + pad + "]"
        if isinstance(value, dict):
            if not value:
                return "{}"
            if in_list and all(scalar(v) or inlinable_list(v) for v in value.values()):
                body = "{ " + ", ".join(f"{inline(k)}: {inline(v)}"
                                        for k, v in value.items()) + " }"
                if len(pad) + len(body) <= width:
                    return body
            items = [f"{inner}{inline(k)}: {render(v, depth + 1, False)}"
                     for k, v in value.items()]
            return "{\n" + ",\n".join(items) + "\n" + pad + "}"
        raise TypeError(f"cannot serialise {type(value).__name__} into a geometry document")

    return render(doc, 0, False) + "\n"


def declared_texture_size(geo):
    """The box-UV coordinate space of one geometry entry: its declared
    ``texture_width``/``texture_height``, falling back to the runtime loader's
    defaults. This is what the shader divides texel coordinates by."""
    desc = geo.get("description", {}) if isinstance(geo, dict) else {}
    return (int(desc.get("texture_width", DEFAULT_TEXTURE_WIDTH)),
            int(desc.get("texture_height", DEFAULT_TEXTURE_HEIGHT)))


def bone_index(geo):
    """Return (bones_by_name, declaration_order) for a parsed geometry."""
    bones = {}
    order = []
    for bone in geo.get("bones", []):
        bones[bone["name"]] = bone
        order.append(bone["name"])
    return bones, order


def face_rects(uv, size):
    """The box-UV net for one cube: face name -> (u, v, w, h) texel rect.

    MUST stay identical to ``animation::boxUvFaceRect`` and the frontend
    ``faceRect()``. Net layout (u right, v down; depth sz faces on the sides,
    width sx faces front/back, top row holds up/down caps). Matches vanilla
    ModelPart.Cube: DOWN is the left cap, UP the right cap.

              +----+----+
              | -Y | +Y |          (each sx x sz)
         +----+----+----+----+
         | -X | -Z | +X | +Z |     (-X/+X: sz x sy, -Z/+Z: sx x sy)
         +----+----+----+----+
    """
    u, v = uv
    w, h, d = (float(s) for s in size)  # sx, sy, sz
    return {
        "up":    (u + d + w,      v,     w, d),
        "down":  (u + d,          v,     w, d),
        "west":  (u,              v + d, d, h),
        "front": (u + d,          v + d, w, h),
        "east":  (u + d + w,      v + d, d, h),
        "back":  (u + 2 * d + w,  v + d, w, h),
    }


def cube_face_rects(bones, order, tw, th):
    """Every cube's box-UV rects in declared texel space, keyed
    ``"<bone>#<cubeIndex>"``. The server hands these to the frontend so it can
    cross-check its own ``faceRect()`` against this reference implementation."""
    del tw, th  # rects are in texel space; dims are only needed to render
    result = {}
    for name in order:
        for idx, cube in enumerate(bones[name].get("cubes", [])):
            rects = face_rects(cube.get("uv", [0, 0]), cube.get("size", [0, 0, 0]))
            result[f"{name}#{idx}"] = {fn: [float(v) for v in r]
                                       for fn, r in rects.items()}
    return result


def bone_world(bones, name, cache):
    """World matrix (4x4) for a bone: its rotation about its pivot, then the
    parent chain. Requires numpy (the 3D preview path)."""
    import numpy as np  # lazy
    if name in cache:
        return cache[name]
    bone = bones[name]
    pivot = np.array(bone.get("pivot", [0, 0, 0]), float)
    rot = rot_matrix(bone.get("rotation", [0, 0, 0]))
    local = np.eye(4)
    local[:3, :3] = rot
    local[:3, 3] = pivot - rot @ pivot
    parent = bone.get("parent")
    world = bone_world(bones, parent, cache) @ local if parent else local
    cache[name] = world
    return world


def build_faces(bones, order, tw, th):
    """Flatten every cube face into (world-space 4 corner points, normalized
    UVs, world normal) triples ready for the software rasterizer. Requires
    numpy (the 3D preview path).

    UVs are normalized by the *declared* ``tw``/``th``, which is the box-UV
    coordinate space the shader divides by (item_entity.vert receives the
    geometry's texture_width/height, not the atlas pixel size). A skin whose PNG
    is a scaled copy of that declaration therefore maps face-for-face.

    ``inflate`` grows the drawn box around its centre without touching the net,
    and ``neverRender`` bones are transform-only, exactly as the renderer treats
    them.
    """
    import numpy as np  # lazy
    cache = {}
    faces = []
    for name in order:
        bone = bones[name]
        world = bone_world(bones, name, cache)
        if bone.get("neverRender", False):
            continue
        for cube in bone.get("cubes", []):
            origin = np.array(cube["origin"], float)
            size = np.array(cube["size"], float)
            inflate = float(cube.get("inflate", 0.0))
            draw_origin = origin - inflate
            draw_size = size + 2.0 * inflate
            uv = cube.get("uv", [0, 0])
            mirror = bool(cube.get("mirror", False))
            crot = rot_matrix(cube["rotation"]) if "rotation" in cube else None
            cpivot = np.array(cube.get("pivot", origin + size / 2), float)
            rects = face_rects(uv, size)  # the net follows the uninflated size

            def to_world(corner):
                p = draw_origin + np.array(corner, float) * draw_size
                if crot is not None:
                    p = crot @ (p - cpivot) + cpivot
                return (world @ np.append(p, 1.0))[:3]

            for fname, (normal, corners) in FACES.items():
                rx, ry, rw, rh = rects[fname]
                uvs = np.array([[rx, ry], [rx + rw, ry],
                                [rx + rw, ry + rh], [rx, ry + rh]], float)
                if mirror:
                    uvs[:, 0] = 2 * rx + rw - uvs[:, 0]
                pts = np.array([to_world(c) for c in corners], float)
                nlocal = np.array(normal, float)
                if crot is not None:
                    nlocal = crot @ nlocal
                faces.append((pts, uvs / [tw, th], world[:3, :3] @ nlocal))
    return faces


def png_dimensions_bytes(data, what="data"):
    """Read a PNG's pixel size from the IHDR chunk of an in-memory image."""
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {what}")
    return struct.unpack(">II", data[16:24])


def png_dimensions(path):
    """Read a PNG's pixel size from its IHDR chunk (stdlib only)."""
    with open(path, "rb") as handle:
        head = handle.read(24)
    return png_dimensions_bytes(head, path)


def encode_png(width, height, rgba):
    """Encode a raw RGBA byte buffer as a PNG (stdlib zlib only)."""
    import zlib

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


# The renderer's procedural entity skin (VulkanRenderer::createEntityTextureArray)
# paints these per-bone colours when the .png is missing, so an entity is still
# readable. Same table here, so the editor previews the same thing.
FALLBACK_COLOURS = {
    "head": ((240, 175, 178), (180, 110, 115)),
    "leg": ((190, 120, 125), (150, 90, 95)),
    "": ((230, 155, 160), (180, 110, 115)),
}


def fallback_colour(bone_name):
    if bone_name == "head":
        return FALLBACK_COLOURS["head"]
    if bone_name.startswith("leg"):
        return FALLBACK_COLOURS["leg"]
    return FALLBACK_COLOURS[""]


def procedural_atlas(bones, order, tw, th):
    """The skin the game paints when a model has no PNG: every cube's box-UV
    face rect filled with its bone colour and outlined, on a declared-size
    canvas. Mirrors VulkanRenderer::createEntityTextureArray's fallback so the
    editor shows what the game would show. Returns (width, height, rgba)."""
    width = max(1, int(tw))
    height = max(1, int(th))
    rgba = bytearray(width * height * 4)
    for name in order:
        bone = bones[name]
        if bone.get("neverRender", False):
            continue
        base, border = fallback_colour(name)
        for cube in bone.get("cubes", []):
            rects = face_rects(cube.get("uv", [0, 0]), cube.get("size", [0, 0, 0]))
            for rx, ry, rw, rh in rects.values():
                x0, y0 = round(rx), round(ry)
                x1, y1 = round(rx + rw), round(ry + rh)
                for y in range(y0, y1):
                    for x in range(x0, x1):
                        if not (0 <= x < width and 0 <= y < height):
                            continue
                        edge = x in (x0, x1 - 1) or y in (y0, y1 - 1)
                        colour = border if edge else base
                        i = (y * width + x) * 4
                        rgba[i:i + 4] = bytes((*colour, 255))
    return width, height, bytes(rgba)


# ---- project / model resolution ---------------------------------------------

def repo_root():
    """Walk up from this module until a directory containing resources/ is
    found (the project root)."""
    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        if (parent / "resources" / "animation").is_dir():
            return parent
    return here.parent.parent


def find_geo(project_root, model):
    return project_root / "resources" / "animation" / f"{model}.geo.json"


def find_texture(project_root, model):
    """Locate the model's entity skin PNG, or None if absent."""
    entity = (project_root / "resources" / "vanilla" / "1.16.1" /
              "textures" / "minecraft" / "entity")
    for cand in [entity / model / f"{model}.png", entity / f"{model}.png"]:
        if cand.exists():
            return cand
    if entity.is_dir():
        hits = sorted(entity.rglob(f"{model}.png"))
        if hits:
            return hits[0]
    return None


def resolve_geo(project_root, model):
    path = find_geo(project_root, model)
    if path.exists():
        return path
    available = available_models(project_root)
    sys.exit(f"No geometry for '{model}' at {path}\n"
             f"Available models: {', '.join(available)}")


def resolve_texture(project_root, model):
    path = find_texture(project_root, model)
    if path is None:
        sys.exit(f"No texture for '{model}' under "
                 f"resources/vanilla/1.16.1/textures/minecraft/entity. "
                 f"Pass one with --texture.")
    return path


def available_models(project_root):
    animation = project_root / "resources" / "animation"
    names = []
    for path in sorted(animation.glob("*.geo.json")):
        stem = path.stem  # e.g. "pig.geo"
        names.append(stem[:-4] if stem.endswith(".geo") else stem)
    return names


def staged_runtime_roots(project_root):
    """The staged runtime resource copies (build*/game/resources) that a running
    game reads instead of the source resources/. The tool mirrors saves into all
    of them so whichever copy the game uses is up to date on next launch."""
    roots = []
    for pattern in ("build*/game/resources", "build/*/game/resources"):
        for cand in sorted(project_root.glob(pattern)):
            if (cand / "animation").is_dir() and cand not in roots:
                roots.append(cand)
    return roots


# ---- self-test ---------------------------------------------------------------

def selftest():
    """Pin the box-UV rects to the exact values tests/box_uv_test.cpp asserts."""
    # Body cube: uv [28, 8], size [8, 8, 16].
    rects = face_rects((28.0, 8.0), (8.0, 8.0, 16.0))
    expected = {
        "east":  (28.0 + 16.0 + 8.0, 8.0 + 16.0, 16.0, 8.0),   # +X
        "west":  (28.0,              8.0 + 16.0, 16.0, 8.0),   # -X
        "up":    (28.0 + 16.0 + 8.0, 8.0,       8.0, 16.0),    # +Y (right cap, vanilla UP)
        "down":  (28.0 + 16.0,       8.0,       8.0, 16.0),    # -Y (left cap, vanilla DOWN)
        "back":  (28.0 + 32.0 + 8.0, 8.0 + 16.0, 8.0, 8.0),    # +Z
        "front": (28.0 + 16.0,       8.0 + 16.0, 8.0, 8.0),    # -Z
    }
    for name, want in expected.items():
        got = rects[name]
        assert all(abs(g - w) < 1e-4 for g, w in zip(got, want)), \
            f"{name} rect {got} != {want}"

    # Top-row caps tile side by side; middle row is contiguous and totals
    # 2 * (sx + sz) from the net origin. Vanilla order: down (left) then up (right).
    up, down = rects["up"], rects["down"]
    assert abs(down[0] + down[2] - up[0]) < 1e-4
    west, front, east, back = (rects[k] for k in ("west", "front", "east", "back"))
    assert abs(west[0] + west[2] - front[0]) < 1e-4
    assert abs(front[0] + front[2] - east[0]) < 1e-4
    assert abs(east[0] + east[2] - back[0]) < 1e-4
    assert abs(back[0] + back[2] - 28.0 - 2.0 * (8.0 + 16.0)) < 1e-4

    # Leg cube: uv [0, 16], size [4, 6, 4].
    leg = face_rects((0.0, 16.0), (4.0, 6.0, 4.0))
    assert leg["west"] == (0.0, 20.0, 4.0, 6.0)

    # Bedrock euler order: Z, then Y, then X (matrix Rz @ Ry @ Rx), matching
    # animation::rotationMatrix — tests/box_uv_test.cpp pins the same vector.
    # Rotating +Z by (90, 90, 0) exposes the order: X goes first and sends +Z to
    # -Y, where Y then leaves it. The opposite composition (Rx @ Ry @ Rz) would
    # land on +X instead.
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        turned = rot_matrix([90.0, 90.0, 0.0]) @ np.array([0.0, 0.0, 1.0])
        assert np.allclose(turned, [0.0, -1.0, 0.0], atol=1e-6), \
            f"rotation order drifted from Rz@Ry@Rx: {turned}"

    # The shipped pig: geometry declares 64x32 and the PNG really is 64x32.
    root = repo_root()
    w, h = png_dimensions(root / "resources" / "vanilla" / "1.16.1" /
                          "textures" / "minecraft" / "entity" / "pig" / "pig.png")
    assert (w, h) == (64, 32), f"pig.png is {w}x{h}, expected 64x32"

    # The parsed pig geometry resolves through the box-UV net inside 64x32.
    geo, tw, th, ident = parse_geo(find_geo(root, "pig"))
    assert (tw, th) == (64, 32) and ident == "geometry.pig"
    bones, order = bone_index(geo)
    for key, rects in cube_face_rects(bones, order, tw, th).items():
        for rx, ry, rw, rh in rects.values():
            assert rx >= 0 and ry >= 0 and rx + rw <= tw + 1e-4 and ry + rh <= th + 1e-4, \
                f"{key} net escapes the {tw}x{th} texture: {(rx, ry, rw, rh)}"

    # The generated fallback skin is a valid PNG spanning exactly the declared
    # box-UV space, so the editor can serve it where the game paints one.
    fw, fh, pixels = procedural_atlas(bones, order, tw, th)
    assert (fw, fh) == (tw, th) and len(pixels) == tw * th * 4
    assert png_dimensions_bytes(encode_png(fw, fh, pixels)) == (tw, th)
    # Every face rect is painted, so no cube reads as a hole in the preview.
    body = face_rects(bones["body"]["cubes"][0]["uv"], bones["body"]["cubes"][0]["size"])
    bx, by, _, _ = body["front"]
    assert pixels[(int(by) * fw + int(bx)) * 4 + 3] == 255, "fallback skin left a face empty"

    # A geometry without a texture declaration falls back to the loader's 16x16,
    # never to a tool-local guess.
    empty = parse_geo_document({"minecraft:geometry": [{"description": {}, "bones": []}]})
    assert empty[1:3] == (DEFAULT_TEXTURE_WIDTH, DEFAULT_TEXTURE_HEIGHT)

    # Re-serialising a shipped model reproduces it byte for byte, so saving from
    # the editor cannot reformat a file it did not mean to change.
    for path in sorted((root / "resources" / "animation").glob("*.geo.json")):
        original = path.read_text()
        assert dumps_geo(json.loads(re.sub(r"//.*", "", original))) == original, \
            f"{path.name} does not round-trip through dumps_geo"

    print("entity_uv_lib selftest OK (box-UV rects match tests/box_uv_test.cpp)")


if __name__ == "__main__":
    selftest()
