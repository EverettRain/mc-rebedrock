#!/usr/bin/env python3
"""Block-model geometry/UV library for the offline preview (RN-13-3).

Two producers of the same thing — a list of textured quads — so they can be
drawn side by side:

  * ``quads_from_dump``   : what OUR baker produced, read from the JSON that
    ``mc_rebedrock_block_model_dump`` prints (src/world/ElementModelBaker.hpp ->
    src/world/FaceBakery.hpp).
  * ``quads_from_vanilla``: what the vanilla model json says, baked here through
    a Python transcription of JE ``FaceBakery`` / ``CuboidFace.UVs`` /
    ``FaceInfo``. Transcribed from the Java source, NOT from our C++ — that is
    the point: if the two pictures agree, two independent readings of the same
    json agree, and the C++ is exonerated.

Textures are read from a resource pack the user supplies on the command line.
Nothing from Mojang is bundled, cached or written into this repository.
"""
import json
import math
from pathlib import Path

import numpy as np

# JE net.minecraft.core.Direction.values() order. Every table below is authored
# against it, exactly as src/world/FaceBakery.hpp is.
FACINGS = ("down", "up", "north", "south", "west", "east")

FACING_UNIT = {
    "down": (0.0, -1.0, 0.0),
    "up": (0.0, 1.0, 0.0),
    "north": (0.0, 0.0, -1.0),
    "south": (0.0, 0.0, 1.0),
    "west": (-1.0, 0.0, 0.0),
    "east": (1.0, 0.0, 0.0),
}

# JE FaceInfo: per facing, the four vertices as (x, y, z) picks of min(0)/max(1).
FACE_INFO = {
    "down": ((0, 0, 1), (0, 0, 0), (1, 0, 0), (1, 0, 1)),
    "up": ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
    "north": ((1, 1, 0), (1, 0, 0), (0, 0, 0), (0, 1, 0)),
    "south": ((0, 1, 1), (0, 0, 1), (1, 0, 1), (1, 1, 1)),
    "west": ((0, 1, 0), (0, 0, 0), (0, 0, 1), (0, 1, 1)),
    "east": ((1, 1, 1), (1, 0, 1), (1, 0, 0), (1, 1, 0)),
}

# CardinalLighting.DEFAULT, the falloff `"shade": false` opts out of. Mirrors
# resources/shaders/src/include/lightmap.glsl's cardinalShade so the preview
# shows the same relative brightness the game does.
CARDINAL_SHADE = {"down": 0.5, "up": 1.0, "north": 0.8, "south": 0.8, "west": 0.6, "east": 0.6}


def default_face_uv(frm, to, facing):
    """JE FaceBakery.defaultFaceUV: project the box onto the face plane."""
    if facing == "down":
        return (frm[0], 16.0 - to[2], to[0], 16.0 - frm[2])
    if facing == "up":
        return (frm[0], frm[2], to[0], to[2])
    if facing == "north":
        return (16.0 - to[0], 16.0 - to[1], 16.0 - frm[0], 16.0 - frm[1])
    if facing == "south":
        return (frm[0], 16.0 - to[1], to[0], 16.0 - frm[1])
    if facing == "west":
        return (frm[2], 16.0 - to[1], to[2], 16.0 - frm[1])
    return (16.0 - to[2], 16.0 - to[1], 16.0 - frm[2], 16.0 - frm[1])


def vertex_u(uv, index):
    """CuboidFace.UVs.getVertexU."""
    return uv[2] if index not in (0, 1) else uv[0]


def vertex_v(uv, index):
    """CuboidFace.UVs.getVertexV."""
    return uv[3] if index not in (0, 3) else uv[1]


def axis_matrix(axis, degrees):
    """A single-axis rotation, same handedness as FaceBakery::axisMatrix."""
    r = math.radians(degrees)
    c, s = math.cos(r), math.sin(r)
    if axis == "x":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], float)
    if axis == "y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], float)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], float)


def _nearest_facing(vector):
    best, best_dot = "up", -2.0
    for name, unit in FACING_UNIT.items():
        dot = float(np.dot(vector, unit))
        if dot > best_dot:
            best, best_dot = name, dot
    return best


def _recalculate_winding(positions, uvs, facing):
    """JE FaceBakery.recalculateWinding."""
    lo = positions.min(axis=0)
    hi = positions.max(axis=0)
    for vertex, info in enumerate(FACE_INFO[facing]):
        target = np.array([hi[a] if info[a] else lo[a] for a in range(3)], float)
        for i in range(vertex, 4):
            if np.linalg.norm(positions[i] - target) < 1e-4:
                if i != vertex:
                    positions[[vertex, i]] = positions[[i, vertex]]
                    uvs[[vertex, i]] = uvs[[i, vertex]]
                break


def bake_face(frm, to, facing, face, element_rotation, state_matrix):
    """JE FaceBakery.bakeQuad, for one declared face of one element.

    `frm`/`to` are in 0..16 model units; `face` is the json face dict plus a
    resolved `sprite`. Returns a quad dict, or None if the face is degenerate.
    """
    uv = face.get("uv") or default_face_uv(frm, to, facing)
    quadrant = int(face.get("rotation", 0)) // 90
    positions = np.zeros((4, 3), float)
    uvs = np.zeros((4, 2), float)
    element_m = None
    origin = np.array([8.0, 8.0, 8.0], float) / 16.0
    if element_rotation:
        element_m = axis_matrix(element_rotation["axis"], float(element_rotation["angle"]))
        origin = np.array(element_rotation.get("origin", [8, 8, 8]), float) / 16.0
    middle = np.array([0.5, 0.5, 0.5], float)
    for i, info in enumerate(FACE_INFO[facing]):
        v = np.array([(to[a] if info[a] else frm[a]) / 16.0 for a in range(3)], float)
        if element_m is not None:
            v = origin + element_m @ (v - origin)
        v = middle + state_matrix @ (v - middle)
        positions[i] = v
        corner = (i + quadrant) & 3
        uvs[i] = (vertex_u(uv, corner) / 16.0, vertex_v(uv, corner) / 16.0)

    normal = np.cross(positions[1] - positions[0], positions[2] - positions[0])
    if not np.isfinite(normal).all() or np.linalg.norm(normal) < 1e-9:
        return None
    baked_facing = _nearest_facing(normal)
    if element_m is None:
        _recalculate_winding(positions, uvs, baked_facing)
    return {
        "facing": baked_facing,
        "position": positions,
        "uv": uvs,
        "sprite": face["sprite"],
        "shade": bool(face.get("shade", True)),
    }


# --- vanilla model json -------------------------------------------------------

def _model_path(pack_root, reference):
    name = reference.split(":")[-1]
    return Path(pack_root) / "assets" / "minecraft" / "models" / (name + ".json")


def load_model(pack_root, reference):
    """Resolve a model reference through its `parent` chain, merging textures."""
    textures = {}
    elements = None
    ambient_occlusion = True
    seen = set()
    while reference is not None:
        path = _model_path(pack_root, reference)
        if reference in seen or not path.exists():
            break
        seen.add(reference)
        data = json.loads(path.read_text())
        for key, value in data.get("textures", {}).items():
            textures.setdefault(key, value)
        if elements is None and "elements" in data:
            elements = data["elements"]
        if "ambientocclusion" in data:
            ambient_occlusion = bool(data["ambientocclusion"])
        reference = data.get("parent")
    return {"textures": textures, "elements": elements or [],
            "ambientocclusion": ambient_occlusion}


def resolve_texture(textures, reference):
    """`#side` -> `#texture` -> `block/oak_trapdoor`, following the aliases."""
    seen = 0
    while isinstance(reference, str) and reference.startswith("#") and seen < 16:
        reference = textures.get(reference[1:], reference)
        seen += 1
    return (reference or "").split(":")[-1]


def quads_from_vanilla(pack_root, model_reference, y_degrees=0.0, x_degrees=0.0):
    """Bake a vanilla block model json (plus its blockstate x/y rotation)."""
    model = load_model(pack_root, model_reference)
    # A blockstate's `x`/`y` turn the model the OPPOSITE way round from a
    # right-handed rotation about that axis, so both are negated here. Straight
    # from the source rather than from the wiki's word "clockwise":
    # Variant.SimpleModelState.asModelState -> Quadrant.fromXYZAngles ->
    # BLOCK_ROT_Y_90 = ROT_90_Y_NEG = (P321, invertX) = (x,y,z) -> (-z, y, x), so
    # y=90 carries SOUTH to WEST; BLOCK_ROT_X_90 = ROT_90_X_NEG = (P132, invertZ)
    # = (x,y,z) -> (x, z, -y), so x=90 carries UP to NORTH. `axis_matrix` takes
    # south to east at +90 and up to south at +90, i.e. the other way round both
    # times. This is the same "engine yaw = 360 - vanilla y" rule
    # ElementModelBaker.hpp states for the C++ side; getting it backwards here
    # would accuse a correct model of being mirrored east/west.
    state_matrix = axis_matrix("y", -y_degrees) @ axis_matrix("x", -x_degrees)
    quads = []
    for element in model["elements"]:
        frm = [float(v) for v in element["from"]]
        to = [float(v) for v in element["to"]]
        rotation = None
        if "rotation" in element:
            rotation = {"axis": element["rotation"]["axis"],
                        "angle": element["rotation"]["angle"],
                        "origin": element["rotation"].get("origin", [8, 8, 8])}
        for facing, face in element.get("faces", {}).items():
            declared = dict(face)
            declared["sprite"] = resolve_texture(model["textures"], face.get("texture"))
            declared["shade"] = element.get("shade", True)
            baked = bake_face(frm, to, facing, declared, rotation, state_matrix)
            if baked is not None:
                quads.append(baked)
    return quads


def blockstate_variant(pack_root, block, state):
    """The `{model, x, y}` a vanilla blockstate json selects for `state`.

    `state` is a dict of property -> value strings. Matches a variant key whose
    every `key=value` clause is satisfied; an empty key ("") matches anything.
    """
    path = Path(pack_root) / "assets" / "minecraft" / "blockstates" / (block + ".json")
    data = json.loads(path.read_text())
    variants = data.get("variants")
    if variants is None:
        raise SystemExit(f"{block}.json is multipart; pass --vanilla-model explicitly")
    for key, value in variants.items():
        clauses = [c for c in key.split(",") if c]
        if all(state.get(c.split("=")[0]) == c.split("=")[1] for c in clauses):
            entry = value[0] if isinstance(value, list) else value
            return {"model": entry["model"], "x": float(entry.get("x", 0)),
                    "y": float(entry.get("y", 0))}
    raise SystemExit(f"no variant of {block} matches {state}")


# --- our own baked model ------------------------------------------------------

def quads_from_dump(dump):
    """The quads `mc_rebedrock_block_model_dump` printed, in this file's shape."""
    return [{"facing": q["facing"],
             "position": np.array(q["position"], float),
             "uv": np.array(q["uv"], float),
             "sprite": q["sprite"],
             "shade": bool(q["shade"])}
            for q in dump["quads"]]


# --- comparison ---------------------------------------------------------------

def _quad_key(quad):
    """A quad's identity independent of corner order: facing + its corner set."""
    corners = sorted(tuple(round(float(c), 5) for c in point) for point in quad["position"])
    return (quad["facing"], tuple(corners))


def compare_quads(ours, reference):
    """Pair two bakes of the same model by geometry and diff their UVs.

    The numeric counterpart of the side-by-side picture, and the stronger of the
    two: a rasterised comparison can tie-break differently on an edge-on view and
    report a fraction of a pixel of difference that means nothing, while this
    says exactly which face of which box carries which corner of the sprite.

    Returns a list of human-readable problems; empty means the two bakes are the
    same model, corner for corner.
    """
    problems = []
    remaining = {}
    for quad in reference:
        remaining.setdefault(_quad_key(quad), []).append(quad)
    for quad in ours:
        key = _quad_key(quad)
        candidates = remaining.get(key)
        if not candidates:
            problems.append(f"{quad['facing']} face at {np.round(quad['position'].mean(0), 4)} "
                            f"has no counterpart in the vanilla model")
            continue
        partner = candidates.pop()
        for i in range(4):
            if not np.allclose(quad["position"][i], partner["position"][i], atol=1e-5):
                problems.append(
                    f"{quad['facing']} face: corner {i} at "
                    f"{np.round(quad['position'][i], 4)} vs "
                    f"{np.round(partner['position'][i], 4)} (winding differs)")
            if not np.allclose(quad["uv"][i], partner["uv"][i], atol=1e-5):
                problems.append(
                    f"{quad['facing']} face at {np.round(quad['position'].mean(0), 4)}: "
                    f"corner {i} samples uv {np.round(quad['uv'][i] * 16, 3)} "
                    f"but vanilla samples {np.round(partner['uv'][i] * 16, 3)}")
    for key, leftovers in remaining.items():
        for quad in leftovers:
            problems.append(f"vanilla has a {quad['facing']} face at "
                            f"{np.round(quad['position'].mean(0), 4)} that we do not draw")
    return problems


# --- rendering ----------------------------------------------------------------

def load_sprite(pack_root, name):
    """A block sprite from the user's resource pack, as an RGBA numpy array.

    An animated sprite (a tall strip with a .mcmeta) is cut down to frame 0, the
    same frame the block atlas bakes for a non-fluid animated block.
    """
    from PIL import Image
    if not name:
        return None
    path = Path(pack_root) / "assets" / "minecraft" / "textures" / (
        name if "/" in name else "block/" + name)
    path = path.with_suffix(".png")
    if not path.exists():
        return None
    image = Image.open(path).convert("RGBA")
    if image.height > image.width and image.height % image.width == 0:
        image = image.crop((0, 0, image.width, image.width))
    return np.asarray(image, float)


def render(quads, sprites, yaw, pitch, res=360, bg=(26, 28, 34), shaded=True):
    """Rasterise `quads` (cell-local 0..1 positions) from a yaw/pitch viewpoint."""
    from PIL import Image
    view = axis_matrix("x", pitch) @ axis_matrix("y", yaw)
    image = np.zeros((res, res, 3), float)
    image[:] = bg
    zbuf = np.full((res, res), np.inf)

    centre = np.array([0.5, 0.5, 0.5], float)
    scale = res / 1.55

    def project(points):
        sx = points[:, 0] * scale + res / 2
        sy = res / 2 - points[:, 1] * scale
        return np.stack([sx, sy], 1)

    # The camera sits on +Z looking toward -Z, so a face is visible exactly when
    # its transformed normal points back at it (+Z) and `depth` counts away from
    # the eye. Getting this backwards silently shows the far side of the model:
    # the "top" view then draws the DOWN face, and a defect on the up face --
    # a diode's `#top` sprite not switching to its lit variant, say -- reads as
    # "no difference".
    for quad in quads:
        texture = sprites.get(quad["sprite"])
        if texture is None:
            continue
        viewed = (quad["position"] - centre) @ view.T
        normal = np.cross(viewed[1] - viewed[0], viewed[2] - viewed[0])
        if normal[2] < 0.0:  # back-facing after projection
            continue
        shade = 1.0
        if shaded and quad["shade"]:
            shade = CARDINAL_SHADE[quad["facing"]]
        screen = project(viewed)
        depth = -viewed[:, 2]
        for tri in ((0, 1, 2), (0, 2, 3)):
            _raster(image, zbuf, screen[list(tri)], depth[list(tri)],
                    quad["uv"][list(tri)], texture, shade)
    return Image.fromarray(np.clip(image, 0, 255).astype(np.uint8))


def _raster(image, zbuf, screen, depth, uv, texture, shade):
    height, width = texture.shape[0], texture.shape[1]
    x0 = max(int(np.floor(screen[:, 0].min())), 0)
    x1 = min(int(np.ceil(screen[:, 0].max())), image.shape[1] - 1)
    y0 = max(int(np.floor(screen[:, 1].min())), 0)
    y1 = min(int(np.ceil(screen[:, 1].max())), image.shape[0] - 1)
    if x1 < x0 or y1 < y0:
        return
    (ax, ay), (bx, by), (cx, cy) = screen
    denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(denominator) < 1e-9:
        return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    px, py = xs + 0.5, ys + 0.5
    w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denominator
    w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denominator
    w2 = 1 - w0 - w1
    inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
    if not inside.any():
        return
    z = w0 * depth[0] + w1 * depth[1] + w2 * depth[2]
    u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
    v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
    tx = np.mod(np.floor(u * width).astype(int), width)
    ty = np.mod(np.floor(v * height).astype(int), height)
    texel = texture[ty, tx]
    visible = inside & (texel[..., 3] >= 128) & (z < zbuf[y0:y1 + 1, x0:x1 + 1])
    if not visible.any():
        return
    zbuf[y0:y1 + 1, x0:x1 + 1][visible] = z[visible]
    image[y0:y1 + 1, x0:x1 + 1][visible] = texel[..., :3][visible] * shade


def render_net(quads, sprites, scale=14):
    """The sprite with every face's UV rect outlined and labelled by facing.

    This is the view that makes an orientation defect legible: two models with
    identical rects but different corner order draw the same box and different
    letters.
    """
    from PIL import Image, ImageDraw
    names = sorted({q["sprite"] for q in quads if sprites.get(q["sprite"]) is not None})
    if not names:
        return None
    tiles = []
    for name in names:
        texture = sprites[name]
        height, width = texture.shape[0], texture.shape[1]
        base = Image.fromarray(texture.astype(np.uint8)).resize(
            (width * scale, height * scale), Image.NEAREST)
        sheet = Image.new("RGB", base.size, (20, 20, 24))
        sheet.paste(base.convert("RGB"), (0, 0))
        draw = ImageDraw.Draw(sheet)
        for quad in quads:
            if quad["sprite"] != name:
                continue
            uv = quad["uv"]
            xs = uv[:, 0] * width * scale
            ys = uv[:, 1] * height * scale
            draw.polygon(list(zip(xs, ys)), outline=(255, 210, 90))
            # The FIRST corner, marked: a rect drawn from a different corner is
            # the same rectangle and a different texture orientation.
            draw.ellipse([xs[0] - 3, ys[0] - 3, xs[0] + 3, ys[0] + 3], fill=(255, 80, 80))
            draw.text((xs.mean() - 12, ys.mean() - 6), quad["facing"][:2].upper(),
                      fill=(255, 255, 255))
        tiles.append((name, sheet))
    total = sum(t.width for _, t in tiles) + 8 * (len(tiles) + 1)
    tallest = max(t.height for _, t in tiles)
    out = Image.new("RGB", (total, tallest + 24), (15, 16, 20))
    draw = ImageDraw.Draw(out)
    x = 8
    for name, tile in tiles:
        draw.text((x, 4), name, fill=(220, 220, 220))
        out.paste(tile, (x, 20))
        x += tile.width + 8
    return out
