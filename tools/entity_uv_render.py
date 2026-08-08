#!/usr/bin/env python3
"""Software box-UV renderer for mc-rebedrock (numpy + Pillow).

Ported 1:1 from the original tools/entity_uv_preview.py so the CLI preview keeps
its exact behaviour; the geometry/box-UV math now lives in entity_uv_lib.py (the
single source of truth) and is reused here. The web editor's frontend (app.js)
re-implements the same rasteriser in JS so its live 3D preview matches the game.
"""
import numpy as np
from PIL import Image

from entity_uv_lib import (
    ALPHA_CUTOFF, FACES, NET_STYLE, bone_index, build_faces, face_rects, parse_geo,
)

# item_entity.frag's fixed entity light, applied to the *world* normal (the
# camera orbit must not drag the highlight around with it).
SHADER_LIGHT = np.array([-0.45, 0.85, 0.30])
SHADER_LIGHT /= np.linalg.norm(SHADER_LIGHT)
SHADER_AMBIENT = 0.42
SHADER_DIFFUSE = 0.58


def render(faces, texture, yaw, pitch, res=360, bg=(26, 28, 34)):
    """Rasterise `faces` (from entity_uv_lib.build_faces) against `texture`
    (a Pillow RGBA image) from the given yaw/pitch viewpoint."""
    from entity_uv_lib import rot_matrix
    view = rot_matrix([pitch, yaw, 0])
    tex = np.asarray(texture.convert("RGBA"), float)
    thh, tww = tex.shape[0], tex.shape[1]

    tf = []
    allpts = []
    for pts, uvs, normal in faces:
        vp = pts @ view.T
        nv = view @ normal
        tf.append((vp, uvs, nv, normal))
        allpts.append(vp)
    allpts = np.concatenate(allpts, 0)
    lo, hi = allpts.min(0), allpts.max(0)
    center = (lo + hi) / 2
    span = float((hi[:2] - lo[:2]).max()) * 1.15 or 1.0
    scale = res / span

    def project(p):
        sx = (p[:, 0] - center[0]) * scale + res / 2
        sy = res / 2 - (p[:, 1] - center[1]) * scale
        return np.stack([sx, sy], 1)

    img = np.zeros((res, res, 3), float)
    img[:] = bg
    zbuf = np.full((res, res), np.inf)

    for vp, uvs, nv, normal in tf:
        if nv[2] > 0.02:  # back-facing (viewer looks down -Z after projection)
            continue
        # Same lighting term as item_entity.frag, on the world normal.
        unit = normal / (np.linalg.norm(normal) or 1.0)
        shade = SHADER_AMBIENT + SHADER_DIFFUSE * max(0.0, float(np.dot(unit, SHADER_LIGHT)))
        scr = project(vp)
        depth = vp[:, 2]
        for tri in ((0, 1, 2), (0, 2, 3)):
            _raster_tri(img, zbuf, scr[list(tri)], depth[list(tri)],
                        uvs[list(tri)], tex, tww, thh, shade)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def _raster_tri(img, zbuf, scr, depth, uv, tex, tww, thh, shade):
    x0 = max(int(np.floor(scr[:, 0].min())), 0)
    x1 = min(int(np.ceil(scr[:, 0].max())), img.shape[1] - 1)
    y0 = max(int(np.floor(scr[:, 1].min())), 0)
    y1 = min(int(np.ceil(scr[:, 1].max())), img.shape[0] - 1)
    if x1 < x0 or y1 < y0:
        return
    (ax, ay), (bx, by), (cx, cy) = scr
    denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(denom) < 1e-9:
        return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    px, py = xs + 0.5, ys + 0.5
    w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denom
    w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denom
    w2 = 1 - w0 - w1
    inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
    if not inside.any():
        return
    z = w0 * depth[0] + w1 * depth[1] + w2 * depth[2]
    u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
    v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
    # NEAREST + REPEAT, like the entity sampler: floor to a texel, then wrap.
    # Clamping instead would hide out-of-bounds nets that the game tiles.
    tx = np.mod(np.floor(u * tww).astype(int), tww)
    ty = np.mod(np.floor(v * thh).astype(int), thh)
    texel = tex[ty, tx]
    opaque = inside & (texel[..., 3] >= ALPHA_CUTOFF)
    sub = zbuf[y0:y1 + 1, x0:x1 + 1]
    win = opaque & (z < sub)
    if not win.any():
        return
    sub[win] = z[win]
    dst = img[y0:y1 + 1, x0:x1 + 1]
    dst[win] = texel[..., :3][win] * shade


def render_net(bones, order, tw, th, texture, scale=12):
    """Draw the texture with every cube face's box-UV rect outlined and
    labelled, so a developer can see exactly which texels each face samples."""
    from PIL import ImageDraw
    base = texture.convert("RGBA").resize((tw * scale, th * scale), Image.NEAREST)
    sheet = Image.new("RGBA", base.size, (20, 20, 24, 255))
    sheet.alpha_composite(base)
    draw = ImageDraw.Draw(sheet)
    for name in order:
        for cube in bones[name].get("cubes", []):
            rects = face_rects(cube.get("uv", [0, 0]), np.array(cube["size"], float))
            for fname, (rx, ry, rw, rh) in rects.items():
                if rw <= 0 or rh <= 0:
                    continue
                label, colour = NET_STYLE[fname]
                x0, y0 = rx * scale, ry * scale
                x1, y1 = (rx + rw) * scale, (ry + rh) * scale
                draw.rectangle([x0, y0, x1 - 1, y1 - 1], outline=colour, width=1)
                draw.text((x0 + 2, y0 + 1), label, fill=colour)
    return sheet.convert("RGB")
