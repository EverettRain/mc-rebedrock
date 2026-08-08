#!/usr/bin/env python3
"""mc-rebedrock texture editor — local single-page web app (stdlib only).

Run:
    python3 tools/texture_editor/server.py
Then open http://127.0.0.1:8765 in a browser.

The page edits the Bedrock ``.geo.json`` texture configuration for any model
under ``resources/animation/`` — cube box-UV net origins (``uv``), the declared
``texture_width``/``texture_height``, and per-cube ``mirror`` — with a live 3D
preview that mirrors the game's box-UV mapping (the pig is the default example:
resources/animation/pig.geo.json + the vanilla pig.png). It exists precisely so
texture-face-mapping and PNG-tiling mistakes can be fixed in the browser without
touching or recompiling the C++.

The editor reports; it does not overrule. UVs are never clamped, rounded or
otherwise adjusted behind the developer's back — a net that runs off the texture
is a legal mapping (the entity sampler repeats) and is saved as typed, with a
diagnostic attached. Diagnostics travel next to the result as ``notes``; the
only refusals are structurally invalid documents and unknown model names.

Saving writes the source file under ``resources/animation/`` (with a ``.bak``
backup) and mirrors it into every staged runtime copy (``build*/game/resources``)
so whichever copy the game reads is up to date on next launch. The text is
rendered in the project's own layout (uvlib.dumps_geo), so a one-texel change is
a one-line diff.

No third-party Python packages are required; the box-UV geometry, the PNG header
reader and the procedural fallback skin all come from tools/entity_uv_lib.py
(stdlib only).
"""
import argparse
import json
import mimetypes
import re
import shutil
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))

import entity_uv_lib as uvlib  # noqa: E402

DEFAULT_PORT = 8765
STATIC_DIR = HERE / "static"
WRITE_LOCK = threading.Lock()
_MODEL_NAME = re.compile(r"^[A-Za-z0-9_\-]+$")


class EditorServer:
    def __init__(self, project_root):
        self.root = project_root.resolve()
        self.animation_dir = self.root / "resources" / "animation"
        self.staged = uvlib.staged_runtime_roots(self.root)

    # ---- model helpers -------------------------------------------------------

    def models(self):
        return uvlib.available_models(self.root)

    def geo_path(self, model):
        """The source .geo.json for a model name. Rejects anything that is not a
        plain model name so a request can never address a path of its own
        choosing (the tool writes files, including into the staged runtime
        copies)."""
        if not _MODEL_NAME.match(model or ""):
            raise ValueError(f"invalid model name: {model!r}")
        return uvlib.find_geo(self.root, model)

    def model_info(self, model):
        """The editor state the frontend needs for one model."""
        path = self.geo_path(model)
        doc = uvlib.load_json(path)
        geo, tw, th, identifier = uvlib.parse_geo_document(doc, path)
        bones, order = uvlib.bone_index(geo)
        rects = uvlib.cube_face_rects(bones, order, tw, th)

        texture = None
        tex_path = uvlib.find_texture(self.root, model)
        if tex_path is not None:
            w, h = uvlib.png_dimensions(tex_path)
            texture = {
                "relative": str(tex_path.relative_to(self.root)),
                "width": w,
                "height": h,
                "generated": False,
            }
        else:
            # No skin on disk: the game paints one through boxUvFaceRect, and so
            # does the editor, instead of inventing flat colours of its own.
            texture = {
                "relative": f"（无 PNG，使用与游戏相同的程序化贴图 {tw}×{th}）",
                "width": tw,
                "height": th,
                "generated": True,
            }

        return {
            "name": model,
            "geo_relative": str(path.relative_to(self.root)),
            "document": doc,
            # The runtime loader takes the first geometry when no identifier is
            # requested (SkeletalModel::loadGeometry), so that is the one the
            # editor edits and previews.
            "geometry_index": 0,
            "declared": {"texture_width": tw, "texture_height": th},
            "identifier": identifier,
            "texture": texture,
            "reference_rects": rects,
            "notes": self._notes(geo, tw, th, texture),
        }

    @staticmethod
    def _notes(geo, tw, th, texture):
        """Advisory diagnostics. None of these block anything: the game samples
        whatever the file says, so the editor reports and previews it rather
        than overriding the developer's numbers."""
        notes = []
        if texture and not texture["generated"]:
            w, h = texture["width"], texture["height"]
            if (w, h) != (tw, th):
                exact = w % tw == 0 and h % th == 0 and w // tw == h // th
                if exact:
                    notes.append(
                        f"PNG 为 {w}×{h}，是声明尺寸 {tw}×{th} 的 {w // tw} 倍整数放大 —— "
                        f"映射仍然逐面对齐（游戏按声明尺寸归一化 UV）。")
                else:
                    notes.append(
                        f"PNG 实际尺寸 {w}×{h} 与几何声明的 {tw}×{th} 不成整数倍 —— "
                        f"每个 texel 会落在非整数像素上，出现半个像素的错位/分块。"
                        f"把声明尺寸改成 PNG 的整除比例，或换一张贴图。")

        escaping = []
        for bone in geo.get("bones", []):
            for index, cube in enumerate(bone.get("cubes", [])):
                rects = uvlib.face_rects(cube.get("uv", [0, 0]), cube.get("size", [0, 0, 0]))
                for rx, ry, rw, rh in rects.values():
                    if rx < -1e-4 or ry < -1e-4 or rx + rw > tw + 1e-4 or ry + rh > th + 1e-4:
                        escaping.append(f"{bone.get('name', '?')}#{index}")
                        break
        if escaping:
            notes.append(
                f"以下 cube 的 box-UV 网越出 {tw}×{th} 声明范围：{', '.join(escaping)}。"
                f"实体采样器是 REPEAT，越界部分会绕回贴图另一侧（预览已按同样方式呈现）。")
        return notes

    def fallback_texture(self, model):
        """PNG bytes for the procedural skin the renderer paints when a model
        has no texture file (same colours, same box-UV rects)."""
        geo, tw, th, _identifier = uvlib.parse_geo(self.geo_path(model))
        bones, order = uvlib.bone_index(geo)
        return uvlib.encode_png(*uvlib.procedural_atlas(bones, order, tw, th))

    def format_model(self, raw_body):
        """The exact bytes ``save_model`` would write, without writing them, so
        a downloaded copy and a saved file are the same text."""
        return uvlib.dumps_geo(json.loads(raw_body))

    def save_model(self, model, raw_body):
        """Validate + write an edited geometry document back to disk.

        Returns (ok, payload, http_status). Writes the source file under
        resources/animation/ and mirrors into every staged runtime copy.
        """
        try:
            path = self.geo_path(model)
        except ValueError as exc:
            return False, {"ok": False, "error": str(exc)}, 400
        if not path.exists():
            # Saving only ever updates a model the project already has; it is an
            # editor, not a way to conjure new geometry files into the tree.
            return False, {"ok": False,
                           "error": f"没有这个模型：{model}（可用：{', '.join(self.models())}）"}, 404

        try:
            doc = json.loads(raw_body)
        except (ValueError, TypeError) as exc:
            return False, {"ok": False, "error": f"invalid JSON: {exc}"}, 400

        if not isinstance(doc, dict) or "minecraft:geometry" not in doc:
            return False, {"ok": False,
                           "error": "document must contain 'minecraft:geometry'"}, 400
        geometries = doc["minecraft:geometry"]
        if not isinstance(geometries, list) or not geometries:
            return False, {"ok": False, "error": "'minecraft:geometry' must be a non-empty array"}, 400
        # Structural validation only — every geometry in the file, not just the
        # edited one, because the whole document is rewritten.
        for index, entry in enumerate(geometries):
            if not isinstance(entry, dict) or not isinstance(entry.get("description"), dict) \
                    or not isinstance(entry.get("bones"), list):
                return False, {
                    "ok": False,
                    "error": f"geometry[{index}] 需要 'description' 与 'bones' 数组",
                }, 400
            for bone in entry["bones"]:
                if not isinstance(bone, dict) or not isinstance(bone.get("name"), str):
                    return False, {"ok": False,
                                   "error": f"geometry[{index}] 里有未命名的 bone"}, 400
                for cube in bone.get("cubes", []):
                    for key, length in (("uv", 2), ("size", 3), ("origin", 3)):
                        value = cube.get(key)
                        if value is None:
                            continue
                        if not isinstance(value, list) or len(value) < length or \
                                not all(isinstance(n, (int, float)) for n in value):
                            return False, {
                                "ok": False,
                                "error": (f"{bone['name']} 的 cube.{key} 必须是 {length} 个数字："
                                          f"{value!r}"),
                            }, 400

        geo = geometries[0]
        tw, th = uvlib.declared_texture_size(geo)

        # Written in the project's own layout, so a one-texel uv change shows up
        # as a one-line diff instead of a whole-file reformat.
        targets = self._write_all(model, uvlib.dumps_geo(doc))
        bones, order = uvlib.bone_index(geo)
        rects = uvlib.cube_face_rects(bones, order, tw, th)
        # A net outside the declared texture is legal (the sampler repeats), so
        # it comes back as a warning next to the saved file, never as a refusal.
        return True, {"ok": True, "targets": targets, "reference_rects": rects,
                      "notes": self._notes(geo, tw, th, None)}, 200

    def _write_all(self, model, encoded):
        """Write the file to the source tree and every staged runtime copy."""
        targets = []
        candidates = [self.geo_path(model), *[r / "animation" / f"{model}.geo.json"
                                              for r in self.staged]]
        for path in candidates:
            entry = {"path": str(path), "written": False}
            try:
                parent = path.parent
                if not path.exists() and not parent.is_dir():
                    # A staged copy that never staged this model: skip silently.
                    continue
                with WRITE_LOCK:
                    parent.mkdir(parents=True, exist_ok=True)
                    if path.exists():
                        shutil.copy2(path, str(path) + ".bak")
                    path.write_text(encoded, encoding="utf-8")
                entry["written"] = True
            except OSError as exc:
                entry["error"] = str(exc)
            targets.append(entry)
        return targets


class _Handler(BaseHTTPRequestHandler):
    server_version = "mc-texture-editor/1.0"

    @property
    def app(self):
        return self.server.app  # type: ignore[attr-defined]

    # ---- routing -------------------------------------------------------------

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        query = self.path[len(path):].lstrip("?")
        params = _parse_query(query)
        try:
            if path == "/" or path == "/index.html":
                self._send_file(STATIC_DIR / "index.html")
            elif path in ("/app.js", "/app.css"):
                self._send_file(STATIC_DIR / path.lstrip("/"))
            elif path == "/api/models":
                self._send_json({"models": self.app.models()})
            elif path == "/api/model":
                self._send_json(self.app.model_info(params["name"]))
            elif path == "/api/texture":
                self._send_texture(params["name"])
            else:
                self._send_json({"error": f"no route for {path}"}, status=404)
        except KeyError as exc:
            self._send_json({"error": f"missing parameter: {exc}"}, status=400)
        except ValueError as exc:
            self._send_json({"error": str(exc)}, status=400)
        except Exception as exc:  # surface unexpected errors to the browser
            self._send_json({"error": f"{type(exc).__name__}: {exc}"}, status=500)

    def do_POST(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        params = _parse_query(self.path[len(path):].lstrip("?"))
        try:
            if path in ("/api/model", "/api/format"):
                length = int(self.headers.get("Content-Length", 0) or 0)
                body = self.rfile.read(length).decode("utf-8")
                if path == "/api/format":
                    self._send_text(self.app.format_model(body), "application/json")
                    return
                ok, payload, status = self.app.save_model(params["name"], body)
                self._send_json(payload, status=status)
                return
            self._send_json({"error": f"no route for {path}"}, status=404)
        except KeyError as exc:
            self._send_json({"error": f"missing parameter: {exc}"}, status=400)
        except ValueError as exc:
            self._send_json({"error": str(exc)}, status=400)
        except Exception as exc:
            self._send_json({"error": f"{type(exc).__name__}: {exc}"}, status=500)

    def log_message(self, fmt, *args):
        sys.stderr.write("[texture-editor] " + (fmt % args) + "\n")

    # ---- responders ----------------------------------------------------------

    def _send_file(self, path):
        path = path.resolve()
        if not (str(path).startswith(str(STATIC_DIR.resolve())) and path.is_file()):
            self._send_json({"error": "not found"}, status=404)
            return
        ctype, _ = mimetypes.guess_type(str(path))
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype or "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _send_text(self, text, content_type, status=200):
        data = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", f"{content_type}; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _send_json(self, obj, status=200):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _send_texture(self, name):
        if not _MODEL_NAME.match(name):
            self._send_json({"error": "invalid model name"}, status=400)
            return
        if not self.app.geo_path(name).exists():
            self._send_json({"error": f"no model named {name}"}, status=404)
            return
        tex = uvlib.find_texture(self.app.root, name)
        # No skin on disk: hand back the same procedurally painted net the
        # renderer falls back to, so the preview keeps showing what the game
        # would draw instead of a stand-in of the editor's own invention.
        data = tex.read_bytes() if tex is not None else self.app.fallback_texture(name)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


def _parse_query(query):
    params = {}
    for part in query.split("&"):
        if not part:
            continue
        key, _, value = part.partition("=")
        params[key] = value
    return params


def _main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1",
                    help="listen address (default 127.0.0.1; local-only dev tool)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"listen port (default {DEFAULT_PORT}; "
                         f"or set MC_TEXTURE_EDITOR_PORT)")
    ap.add_argument("--root", default=None,
                    help="override the project root (default: auto-detected)")
    args = ap.parse_args()

    root = Path(args.root) if args.root else uvlib.repo_root()
    app = EditorServer(root)
    if not app.animation_dir.is_dir():
        sys.exit(f"project root has no resources/animation: {app.animation_dir}")

    server = ThreadingHTTPServer((args.host, args.port), _Handler)
    server.app = app  # type: ignore[attr-defined]
    url = f"http://{args.host}:{args.port}/"
    print(f"mc-rebedrock texture editor  ->  {url}")
    print(f"models: {', '.join(app.models())}")
    print(f"staged runtime copies to mirror into: "
          f"{', '.join(str(r) for r in app.staged) or '(none detected)'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    _main()
