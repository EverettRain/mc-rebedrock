/* mc-rebedrock entity texture editor.
 *
 * The box-UV and rasterisation math here must stay byte-for-byte in agreement
 * with:
 *   - tools/entity_uv_lib.py::face_rects   (Python "single source of truth")
 *   - src/animation/SkeletalModel.cpp::boxUvFaceRect  (the game)
 *   - resources/shaders/src/item_entity.vert (the box-UV shader block)
 * The page runs a mapping self-check against the server's reference_rects and
 * shows a badge, so drift is surfaced instead of silently diverging.
 *
 * The whole pipeline mirrors what the game does, so the preview is evidence and
 * not an approximation:
 *   - UVs live in *declared* texel space (texture_width/height) and are
 *     normalised by it, exactly like item_entity.vert; the PNG is whatever
 *     resolution it happens to be and gets sampled at u/tw * pngWidth.
 *   - sampling is NEAREST with REPEAT wrap, like the entity sampler, so a net
 *     that runs off the texture tiles here the same way it tiles in game.
 *   - texels below item_entity.frag's alpha cutoff are discarded, and shading
 *     uses the frag shader's fixed world-space light.
 *   - bones rotate Rz*Ry*Rx, cubes rotate about their own pivot, `inflate`
 *     grows the box without moving the net, and `neverRender` bones are
 *     transform-only — matching animation::rotationMatrix and drawWorldEntities.
 */
"use strict";

/* ---------------------------------------------------------------- box-UV -- */

const FACE_NAMES = ["front", "back", "east", "west", "up", "down"];

// face name -> [label, colour] for the net overlay (matches NET_STYLE in the
// Python lib / original preview tool).
const FACE_STYLE = {
  front: ["F", "#50c878"], back: ["B", "#c85a5a"], east: ["E", "#5a96e6"],
  west: ["W", "#e6be50"], up: ["U", "#c878dc"], down: ["D", "#78c8dc"],
};

// face name -> (normal axis, (TL, TR, BR, BL) cube-corner codes).
const FACE_NORMALS = {
  front: [0, 0, -1], back: [0, 0, 1], east: [1, 0, 0],
  west: [-1, 0, 0], up: [0, 1, 0], down: [0, -1, 0],
};
const FACE_CORNERS = {
  front: [[1, 1, 0], [0, 1, 0], [0, 0, 0], [1, 0, 0]],
  back:  [[0, 1, 1], [1, 1, 1], [1, 0, 1], [0, 0, 1]],
  east:  [[1, 1, 1], [1, 1, 0], [1, 0, 0], [1, 0, 1]],
  west:  [[0, 1, 0], [0, 1, 1], [0, 0, 1], [0, 0, 0]],
  up:    [[0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0]],
  down:  [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]],
};

// box-UV face rect for one cube, in declared texels: must equal
// animation::boxUvFaceRect (face 0..5 = east, west, up, down, back, front).
function faceRect(faceName, uv, size) {
  const u = uv[0], v = uv[1];
  const sx = size[0], sy = size[1], sz = size[2];
  switch (faceName) {
    case "east": return [u + sz + sx, v + sz, sz, sy];        // +X
    case "west": return [u, v + sz, sz, sy];                  // -X
    case "up": return [u + sz, v, sx, sz];                    // +Y
    case "down": return [u + sz + sx, v, sx, sz];             // -Y
    case "back": return [u + 2 * sz + sx, v + sz, sx, sy];    // +Z
    default: return [u + sz, v + sz, sx, sy];                 // -Z (front)
  }
}

// Whole net spans 2*(sx+sz) wide, sy+2*sz tall, anchored at uv. Used for
// reporting, not for constraining anything.
function cubeNetExtent(size) {
  return [2 * (size[0] + size[2]), size[1] + 2 * size[2]];
}

/* ------------------------------------------------------------ 3D matrices -- */

function mat3Mul(a, b) {
  const r = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      for (let k = 0; k < 3; k++) r[i][j] += a[i][k] * b[k][j];
  return r;
}
function mat3Apply(m, v) {
  return [
    m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
    m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
  ];
}
function mat3ApplyAround(m, v, pivot) {
  const p = mat3Apply(m, [v[0] - pivot[0], v[1] - pivot[1], v[2] - pivot[2]]);
  return [p[0] + pivot[0], p[1] + pivot[1], p[2] + pivot[2]];
}
function mat4Mul(a, b) {
  const r = [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  for (let i = 0; i < 4; i++)
    for (let j = 0; j < 4; j++)
      for (let k = 0; k < 4; k++) r[i][j] += a[i][k] * b[k][j];
  return r;
}
function mat4Apply(m, v) {
  const x = m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3];
  const y = m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3];
  const z = m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3];
  return [x, y, z];
}
function mat3Of(m4) {
  return [[m4[0][0], m4[0][1], m4[0][2]], [m4[1][0], m4[1][1], m4[1][2]],
          [m4[2][0], m4[2][1], m4[2][2]]];
}

// Bedrock euler rotation (degrees) about the origin. The axes stack Z, then Y,
// then X, so the matrix is Rz*Ry*Rx and a point turns about X first — the same
// composition as animation::rotationMatrix and entity_uv_lib.rot_matrix.
// Composing it the other way round poses multi-axis bones differently from the
// game, which is invisible on single-axis models and wrong on everything else.
function rotMatrix(deg) {
  const rx = deg[0] * Math.PI / 180, ry = deg[1] * Math.PI / 180, rz = deg[2] * Math.PI / 180;
  const cx = Math.cos(rx), sx = Math.sin(rx);
  const cy = Math.cos(ry), sy = Math.sin(ry);
  const cz = Math.cos(rz), sz = Math.sin(rz);
  const mx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]];
  const my = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]];
  const mz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]];
  return mat3Mul(mat3Mul(mz, my), mx);
}

const boneWorldCache = new Map();
function boneWorld(bonesByName, name) {
  if (boneWorldCache.has(name)) return boneWorldCache.get(name);
  const bone = bonesByName[name];
  const pivot = bone.pivot || [0, 0, 0];
  const rot = rotMatrix(bone.rotation || [0, 0, 0]);
  const rp = mat3Apply(rot, pivot);
  const local = [
    [rot[0][0], rot[0][1], rot[0][2], pivot[0] - rp[0]],
    [rot[1][0], rot[1][1], rot[1][2], pivot[1] - rp[1]],
    [rot[2][0], rot[2][1], rot[2][2], pivot[2] - rp[2]],
    [0, 0, 0, 1],
  ];
  const world = (bone.parent && bonesByName[bone.parent])
    ? mat4Mul(boneWorld(bonesByName, bone.parent), local)
    : local;
  boneWorldCache.set(name, world);
  return world;
}

/* ------------------------------------------------------------------ state -- */

const state = {
  models: [],
  modelName: null,
  doc: null,            // live parsed geometry document (mutated by edits)
  geo: null,            // doc["minecraft:geometry"][0]
  tw: 64, th: 32,       // declared texture dimensions
  texImage: null,       // HTMLImageElement
  texW: 0, texH: 0,     // actual PNG dimensions (0 = no texture)
  texData: null,        // Uint8ClampedArray RGBA at natural size
  referenceRects: null, // from the server's Python implementation
  selected: null,       // {boneName, cubeIndex}
  selectedFace: null,   // the net-position rect that was clicked (a FACE_NAMES key)
  previewFaces: null,   // cached 3D face list
  viewYaw: -35, viewPitch: 22,
  zoom: 1,
  dirty: false,
  netScale: 10,
  netDrag: null,        // {startX, startY, startUv}
};

const $ = (id) => document.getElementById(id);

/* --------------------------------------------------------- geometry utils -- */

// Read-only view of every cube. It must never write to the document: anything
// defaulted here would be serialised back into the user's .geo.json on save,
// turning a rendering convenience into an unrequested edit. Absent fields are
// substituted locally instead, the same way the C++ loader defaults them.
function cubeVec(cube, key, fallback) {
  const value = cube[key];
  if (!Array.isArray(value)) return fallback;
  return fallback.map((d, i) => (typeof value[i] === "number" ? value[i] : d));
}

// Per-face box-UV overrides — a "faces" extension on a cube. Standard box-UV
// hands each geometric face the rect at its own net position; a `faces` entry
// re-routes a rect to a different face (e.g. the back rect serving the front,
// i.e. a "B" rect relabelled "F") and/or rotates its sampling 180°. The game
// does not read this extension yet; the preview honours it so the corrected
// mapping can be seen and saved.
function faceLabel(faces, netPos) {
  if (!faces) return netPos;
  const v = faces[netPos];
  if (!v) return netPos;
  return (typeof v === "string" ? v : v.as) || netPos;
}
function faceSource(faces, faceName) {
  // The net position whose rect is used by geometric face `faceName`.
  if (!faces) return faceName;
  for (const p of FACE_NAMES) {
    const v = faces[p];
    if (v && (typeof v === "string" ? v : v.as) === faceName) return p;
  }
  return faceName;
}
function faceRotation(faces, netPos) {
  if (!faces) return 0;
  const v = faces[netPos];
  return (v && typeof v === "object" && v.rotate === 180) ? 180 : 0;
}
// The one place a "faces" override is written. Defaults (as === netPos, no
// rotation) are dropped, so an untouched cube never gains a faces object.
function setFaceOverride(cube, netPos, as, rotate) {
  const curAs = faceLabel(cube.faces, netPos);
  const curRot = faceRotation(cube.faces, netPos);
  if (as === curAs && rotate === curRot) return false;
  if (as === netPos && rotate !== 180) {
    if (cube.faces) {
      delete cube.faces[netPos];
      if (Object.keys(cube.faces).length === 0) delete cube.faces;
    }
    return true;
  }
  if (!cube.faces) cube.faces = {};
  cube.faces[netPos] = { as, rotate };
  return true;
}

function iterateCubes(geo) {
  const out = [];
  const bones = geo.bones || [];
  for (const bone of bones) {
    (bone.cubes || []).forEach((cube, index) => {
      const size = cubeVec(cube, "size", [0, 0, 0]);
      const uv = cubeVec(cube, "uv", [0, 0]);
      const faces = {};
      for (const fn of FACE_NAMES) faces[fn] = faceRect(fn, uv, size);
      out.push({
        boneName: bone.name, bone, cube, index, size,
        uv, mirror: !!cube.mirror, neverRender: !!bone.neverRender, faces,
      });
    });
  }
  return out;
}

function selectCube(boneName, cubeIndex, faceName) {
  if (!boneName && cubeIndex == null) { state.selected = null; state.selectedFace = null; return; }
  state.selected = { boneName, cubeIndex };
  // Dragging/nudging a cube calls selectCube without a face, which keeps the
  // previously clicked rect selected; only a fresh click supplies a face.
  if (faceName !== undefined) state.selectedFace = faceName;
  syncSelInputs();
  renderAll();
}

function syncSelInputs() {
  const sel = state.selected;
  const s = $("sel-bone"), g = $("sel-geometry"), u = $("sel-uv-u"), v = $("sel-uv-v");
  const faceAs = $("sel-face-as"), faceRot = $("sel-face-rot");
  const cube = selectedCube();
  if (!sel || !cube) {
    s.textContent = "—"; g.textContent = "—";
    u.value = ""; v.value = ""; $("sel-mirror").checked = false;
    u.disabled = v.disabled = $("sel-mirror").disabled = true;
    faceAs.disabled = faceRot.disabled = true;
    return;
  }
  const uv = cubeVec(cube, "uv", [0, 0]);
  const size = cubeVec(cube, "size", [0, 0, 0]);
  const [netW, netH] = cubeNetExtent(size);
  s.textContent = `${sel.boneName} #${sel.cubeIndex}`;
  g.textContent = `o[${cubeVec(cube, "origin", [0, 0, 0])}] s[${size}] 网 ${netW}×${netH}`
    + (cube.inflate ? ` inflate ${cube.inflate}` : "");
  // Show the stored numbers exactly. Rounding them for display used to feed the
  // rounded value straight back into the document on the next edit.
  u.value = String(uv[0]); v.value = String(uv[1]);
  $("sel-mirror").checked = !!cube.mirror;
  u.disabled = v.disabled = $("sel-mirror").disabled = false;

  // Face (rect) relabel / rotate, shown only after clicking a specific UV rect.
  const hasFace = state.selectedFace && FACE_NAMES.includes(state.selectedFace);
  faceAs.disabled = faceRot.disabled = !hasFace;
  if (hasFace) {
    faceAs.value = faceLabel(cube.faces, state.selectedFace);
    faceRot.checked = faceRotation(cube.faces, state.selectedFace) === 180;
  } else {
    faceAs.value = "front";
    faceRot.checked = false;
  }
}

function findBoneIndex(name) {
  return (state.geo.bones || []).findIndex((b) => b.name === name);
}

function selectedCube() {
  if (!state.selected || !state.geo) return null;
  const boneIndex = findBoneIndex(state.selected.boneName);
  if (boneIndex < 0) return null;
  const cubes = state.geo.bones[boneIndex].cubes || [];
  return cubes[state.selected.cubeIndex] || null;
}

// The one place a uv is written. No clamping to the declared texture: the
// entity sampler repeats, so an off-texture net is a legal (if usually
// unintended) mapping, and the editor reports it instead of overriding it.
function setCubeUv(cube, u, v) {
  if (!Number.isFinite(u) || !Number.isFinite(v)) return false;
  const before = cubeVec(cube, "uv", [0, 0]);
  if (before[0] === u && before[1] === v) return false;
  cube.uv = [u, v];
  return true;
}

/* --------------------------------------------------------------- API load -- */

async function api(path) {
  const res = await fetch(path);
  const ct = res.headers.get("content-type") || "";
  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`;
    if (ct.includes("json")) { const j = await res.json(); if (j.error) msg = j.error; }
    throw new Error(msg);
  }
  return ct.includes("json") ? res.json() : res;
}

async function loadModels() {
  const { models } = await api("/api/models");
  state.models = models;
  const sel = $("model-select");
  sel.innerHTML = "";
  for (const m of models) {
    const opt = document.createElement("option");
    opt.value = m; opt.textContent = m;
    sel.appendChild(opt);
  }
  const wanted = sel.dataset.wanted || (models.includes("pig") ? "pig" : models[0]);
  if (wanted) { sel.value = wanted; await loadModel(wanted); }
}

async function loadModel(name) {
  setStatus(`加载 ${name} …`);
  state.modelName = name;
  const info = await api(`/api/model?name=${encodeURIComponent(name)}`);
  state.doc = info.document;
  state.geo = info.document["minecraft:geometry"][info.geometry_index || 0];
  state.tw = info.declared.texture_width;
  state.th = info.declared.texture_height;
  state.referenceRects = info.reference_rects;
  $("tex-width").value = state.tw;
  $("tex-height").value = state.th;
  $("info-geo").textContent = info.geo_relative;
  $("info-declared").textContent = `${state.tw} × ${state.th}`;
  $("info-texture").textContent = info.texture.relative;
  $("info-png").textContent = `${info.texture.width} × ${info.texture.height}`;
  showNotes(info.notes);
  state.texW = 0; state.texH = 0; state.texData = null; state.texImage = null;
  state.selected = null;
  selectCube(null, null);
  state.dirty = false;
  $("save-btn").disabled = false;

  // Always a texture: where the model has no PNG the server paints the same
  // procedural box-UV skin the renderer falls back to.
  await new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = resolve;
    img.onerror = () => reject(new Error("texture decode failed"));
    img.src = `/api/texture?name=${encodeURIComponent(name)}`;
    state.texImage = img;
  });
  state.texW = state.texImage.naturalWidth;
  state.texH = state.texImage.naturalHeight;
  const off = document.createElement("canvas");
  off.width = state.texW; off.height = state.texH;
  const octx = off.getContext("2d", { willReadFrequently: true });
  octx.imageSmoothingEnabled = false;
  octx.drawImage(state.texImage, 0, 0);
  state.texData = octx.getImageData(0, 0, state.texW, state.texH).data;

  state.previewFaces = null; // rebuild with fresh geometry
  renderAll();
  runSelfCheck();
  setStatus(`已加载 ${name}。拖拽中网格子上的面即可平移该 cube 的 box-UV 网。`, "ok");
}

/* ---------------------------------------------------------------- net view -- */

function netGrid() {
  // The net panel shows the PNG at its native pixel size, so every pixel is
  // visible; box-UV rects (declared texels) are scaled onto it by texW/tw and
  // texH/th — the same mapping the game samples, so a rect at declared texel u
  // covers PNG pixels floor(u*texW/tw). With no PNG (procedural fallback) the
  // grid is the declared size and the scale is 1.
  return [state.texW > 0 ? state.texW : state.tw, state.texH > 0 ? state.texH : state.th];
}

function renderNet() {
  if (!state.geo) return;
  const canvas = $("net-canvas"), ctx = canvas.getContext("2d");
  const [gw, gh] = netGrid();
  const scale = Math.max(3, Math.min(820 / gw, 540 / gh) | 0);
  state.netScale = scale;
  canvas.width = gw * scale;
  canvas.height = gh * scale;
  ctx.imageSmoothingEnabled = false;
  ctx.fillStyle = "#14161b";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  if (state.texImage && state.texW > 0) {
    ctx.drawImage(state.texImage, 0, 0, canvas.width, canvas.height);
  }

  // Declared texture boundary on the PNG: the box-UV grid covers [0,0,tw,th]
  // declared texels = PNG pixels [0,0,tw*uScale,th*vScale]. A PNG wider/taller
  // than the declaration has pixels outside the dashed box that a within-grid
  // net never samples (though the game can still tile into them via REPEAT).
  const uScale = state.texW > 0 ? state.texW / state.tw : 1;
  const vScale = state.texH > 0 ? state.texH / state.th : 1;
  if (state.texW > 0 && (state.texW !== state.tw || state.texH !== state.th)) {
    ctx.strokeStyle = "rgba(230,190,80,0.9)";
    ctx.setLineDash([6, 4]);
    ctx.strokeRect(0, 0, state.tw * uScale * scale, state.th * vScale * scale);
    ctx.setLineDash([]);
  }

  const cubes = iterateCubes(state.geo);
  const escaped = new Set();
  for (const cb of cubes) {
    const selected = state.selected &&
      cb.boneName === state.selected.boneName && cb.index === state.selected.cubeIndex;
    const overrides = cb.cube.faces;
    for (const fn of FACE_NAMES) {
      const [rx, ry, rw, rh] = cb.faces[fn]; // declared-texel rect at net position fn
      if (rw <= 0 || rh <= 0) continue;
      const as = faceLabel(overrides, fn);
      const rot = faceRotation(overrides, fn) === 180;
      const style = FACE_STYLE[as] || FACE_STYLE[fn];
      const [label, colour] = style;
      // A rect outside the declaration still renders in game (the sampler
      // repeats); flag it here rather than refusing to draw or moving it back.
      const escapes = rx < 0 || ry < 0 || rx + rw > state.tw + 1e-4 || ry + rh > state.th + 1e-4;
      if (escapes) escaped.add(`${cb.boneName}#${cb.index}`);
      const px = rx * uScale * scale, py = ry * vScale * scale;
      const pw = rw * uScale * scale, ph = rh * vScale * scale;
      ctx.strokeStyle = colour;
      ctx.lineWidth = selected ? 2.5 : 1;
      ctx.setLineDash(escapes ? [4, 3] : []);
      ctx.strokeRect(px, py, pw, ph);
      ctx.setLineDash([]);
      ctx.fillStyle = colour;
      ctx.font = `${selected ? 11 : 9}px ui-monospace, Menlo, monospace`;
      ctx.fillText(selected ? `${label}${rot ? "↻" : ""} ${cb.boneName}#${cb.index}` : `${label}${rot ? "↻" : ""}`,
                   px + 2, py + 10);
    }
  }
  const dims = (state.texW > 0 && (state.texW !== state.tw || state.texH !== state.th))
    ? `${gw} × ${gh} px @${scale}x（声明网格 ${state.tw}×${state.th}）`
    : `${gw} × ${gh} px @${scale}x`;
  const out = escaped.size
    ? ` · ${escaped.size} 个 cube 越界（虚线，游戏里会绕回）：${[...escaped].join(", ")}`
    : "";
  $("net-dims").textContent = dims + out;
}

function texelAtEvent(event) {
  const canvas = $("net-canvas");
  const rect = canvas.getBoundingClientRect();
  const x = (event.clientX - rect.left) * (canvas.width / rect.width);
  const y = (event.clientY - rect.top) * (canvas.height / rect.height);
  // canvas px -> PNG px -> declared texel (the space `uv` and the rects live in)
  const uScale = state.texW > 0 ? state.texW / state.tw : 1;
  const vScale = state.texH > 0 ? state.texH / state.th : 1;
  return [(x / state.netScale) / uScale, (y / state.netScale) / vScale];
}

function cubeAtTexel(tx, ty) {
  if (!state.geo) return null;
  const cubes = iterateCubes(state.geo);
  for (let i = cubes.length - 1; i >= 0; i--) {
    const cb = cubes[i];
    for (const fn of FACE_NAMES) {
      const [rx, ry, rw, rh] = cb.faces[fn];
      if (tx >= rx && tx < rx + rw && ty >= ry && ty < ry + rh) return { cb, face: fn };
    }
  }
  return null;
}

function moveNet(event) {
  const cube = selectedCube();
  if (!cube || !state.netDrag) return;
  const [tx, ty] = texelAtEvent(event);
  // Offset from where the drag started, in whole texels, applied to the uv the
  // cube had at pointer-down: dragging can only ever produce values the user
  // actually dragged to, and it never accumulates float drift.
  const du = Math.round(tx - state.netDrag.startX);
  const dv = Math.round(ty - state.netDrag.startY);
  const [u0, v0] = state.netDrag.startUv;
  if (!setCubeUv(cube, u0 + du, v0 + dv)) return;
  selectCube(state.selected.boneName, state.selected.cubeIndex);
  markDirty();
}

function nudgeSelected(dx, dy) {
  const cube = selectedCube();
  if (!cube) return;
  const uv = cubeVec(cube, "uv", [0, 0]);
  if (!setCubeUv(cube, uv[0] + dx, uv[1] + dy)) return;
  selectCube(state.selected.boneName, state.selected.cubeIndex);
  markDirty();
}

/* --------------------------------------------------------- 3D preview (JS rasteriser) -- */

function buildPreviewFaces() {
  boneWorldCache.clear();
  const bonesByName = {};
  const order = [];
  for (const b of (state.geo.bones || [])) { bonesByName[b.name] = b; order.push(b.name); }
  const faces = [];
  for (const name of order) {
    const bone = bonesByName[name];
    const world = boneWorld(bonesByName, name);
    const world3 = mat3Of(world);
    // `neverRender` bones only carry a transform for their children, exactly as
    // drawWorldEntities treats them.
    if (bone.neverRender) continue;
    for (const cube of (bone.cubes || [])) {
      const origin = cubeVec(cube, "origin", [0, 0, 0]);
      const size = cubeVec(cube, "size", [0, 0, 0]);
      // `inflate` grows the drawn box around its centre; the box-UV net keeps
      // following the declared size, so the same texels just stretch further.
      const inflate = typeof cube.inflate === "number" ? cube.inflate : 0;
      const drawOrigin = origin.map((v) => v - inflate);
      const drawSize = size.map((v) => v + 2 * inflate);
      const mirror = !!cube.mirror;
      const crot = cube.rotation ? rotMatrix(cube.rotation) : null;
      const cpivot = cubeVec(cube, "pivot",
        [origin[0] + size[0] / 2, origin[1] + size[1] / 2, origin[2] + size[2] / 2]);
      for (const fn of FACE_NAMES) {
        // The "faces" extension may re-route this geometric face to a different
        // net-position rect and/or rotate its sampling 180°.
        const src = faceSource(cube.faces, fn);
        const [rx, ry, rw, rh] = faceRect(src, cubeVec(cube, "uv", [0, 0]), size);
        let uvs = [[rx, ry], [rx + rw, ry], [rx + rw, ry + rh], [rx, ry + rh]];
        if (mirror) uvs = uvs.map((uv) => [2 * rx + rw - uv[0], uv[1]]);
        if (faceRotation(cube.faces, src) === 180) {
          uvs = uvs.map((uv) => [2 * rx + rw - uv[0], 2 * ry + rh - uv[1]]);
        }
        const pts = FACE_CORNERS[fn].map((corner) => {
          let p = [drawOrigin[0] + corner[0] * drawSize[0], drawOrigin[1] + corner[1] * drawSize[1],
                   drawOrigin[2] + corner[2] * drawSize[2]];
          if (crot) p = mat3ApplyAround(crot, p, cpivot);
          return mat4Apply(world, p);
        });
        const local = crot ? mat3Apply(crot, FACE_NORMALS[fn]) : FACE_NORMALS[fn];
        const normal = mat3Apply(world3, local);
        faces.push({ pts, uvs, normal, faceName: fn, boneName: bone.name });
      }
    }
  }
  return faces;
}

const PREVIEW_RES = 360;
// item_entity.frag's entity lighting, applied to the world normal so orbiting
// the camera does not drag the highlight around with it.
const LIGHT = normalize([-0.45, 0.85, 0.30]);
const AMBIENT = 0.42;
const DIFFUSE = 0.58;
// The frag shader discards entity texels with alpha < 0.1 (of 1.0).
const ALPHA_CUTOFF = 0.1 * 255;

function normalize(v) {
  const l = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / l, v[1] / l, v[2] / l];
}

function renderPreview() {
  const canvas = $("preview-canvas"), ctx = canvas.getContext("2d");
  canvas.width = PREVIEW_RES; canvas.height = PREVIEW_RES;
  if (!state.geo || !state.geo.bones || state.geo.bones.length === 0) {
    ctx.fillStyle = "#14161b"; ctx.fillRect(0, 0, PREVIEW_RES, PREVIEW_RES); return;
  }
  if (!state.previewFaces) state.previewFaces = buildPreviewFaces();
  if (state.previewFaces.length === 0) {
    ctx.fillStyle = "#14161b"; ctx.fillRect(0, 0, PREVIEW_RES, PREVIEW_RES); return;
  }

  const view = rotMatrix([state.viewPitch, state.viewYaw, 0]);
  const transformed = [];
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity,
      minZ = Infinity, maxZ = -Infinity;
  for (const f of state.previewFaces) {
    const vp = f.pts.map((p) => mat3Apply(view, p));
    const nv = mat3Apply(view, f.normal);
    if (nv[2] > 0.02) continue; // back-facing (viewer looks down -Z after projection)
    for (const p of vp) {
      if (p[0] < minX) minX = p[0]; if (p[0] > maxX) maxX = p[0];
      if (p[1] < minY) minY = p[1]; if (p[1] > maxY) maxY = p[1];
      if (p[2] < minZ) minZ = p[2]; if (p[2] > maxZ) maxZ = p[2];
    }
    transformed.push({ f, vp, nv });
  }
  if (transformed.length === 0) {
    ctx.fillStyle = "#14161b"; ctx.fillRect(0, 0, PREVIEW_RES, PREVIEW_RES); return;
  }

  // Perspective camera, like the game's. (The preview was orthographic, which
  // squashed faces seen at an angle into slanted parallelograms — the texture
  // read as "unfolded" into rhombi/triangles, with face edges turning into
  // diagonals.) The model is centred on the view axis `dist` in front of the
  // camera with its -Z side nearest — the same side the backface cull above
  // keeps — so the front of the model (the pig's face) is the large, occluding
  // part, exactly as a real camera would see it. z-sorting stays "nearer -Z
  // wins", the same ordering the orthographic depth buffer used.
  const centerX = (minX + maxX) / 2, centerY = (minY + maxY) / 2, centerZ = (minZ + maxZ) / 2;
  const rx = (maxX - minX) / 2, ry = (maxY - minY) / 2, rz = (maxZ - minZ) / 2;
  const radius = Math.max(rx, ry, rz) || 1;
  const dist = radius * 3;
  const near = dist - rz; // depth of the model's nearest (-Z) side
  const focal = (PREVIEW_RES * 0.45) * near / Math.max(rx, ry, 1) * state.zoom;
  const cx = PREVIEW_RES / 2, cy = PREVIEW_RES / 2;
  const depthOf = (p) => dist + (p[2] - centerZ);
  const project = (p) => {
    const d = depthOf(p);
    return [cx + focal * (p[0] - centerX) / d, cy - focal * (p[1] - centerY) / d];
  };

  const imgData = ctx.createImageData(PREVIEW_RES, PREVIEW_RES);
  const px = imgData.data;
  const zbuf = new Float32Array(PREVIEW_RES * PREVIEW_RES).fill(Infinity);

  for (const { f, vp } of transformed) {
    const n = f.normal;
    const nlen = Math.hypot(n[0], n[1], n[2]) || 1;
    const diffuse = Math.max(0, (n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2]) / nlen);
    const shade = AMBIENT + DIFFUSE * diffuse;
    const scr = vp.map(project);
    const depth = vp.map(depthOf);
    // The quad splits along the TL->BR diagonal; each half carries its own three
    // UV corners AND its own three depths. (Passing the whole 4-element arrays to
    // both calls made the second triangle — vertices TL,BR,BL — read TR/BR's UVs
    // and depths for its BR/BL vertices: a twisted mapping whose texture ran
    // parallel to the diagonal instead of the face edges.)
    rasterTri(px, zbuf, PREVIEW_RES, scr[0], scr[1], scr[2],
              [depth[0], depth[1], depth[2]], [f.uvs[0], f.uvs[1], f.uvs[2]], shade);
    rasterTri(px, zbuf, PREVIEW_RES, scr[0], scr[2], scr[3],
              [depth[0], depth[2], depth[3]], [f.uvs[0], f.uvs[2], f.uvs[3]], shade);
  }
  ctx.putImageData(imgData, 0, 0);

  // Optional box-UV net overlay on the preview.
  if ($("show-net").checked) {
    ctx.save();
    for (const { f, vp } of transformed) {
      const scr = vp.map(project);
      ctx.strokeStyle = FACE_STYLE[f.faceName][1];
      ctx.lineWidth = 1;
      ctx.globalAlpha = 0.9;
      ctx.beginPath();
      ctx.moveTo(scr[0][0], scr[0][1]);
      for (let i = 1; i < 4; i++) ctx.lineTo(scr[i][0], scr[i][1]);
      ctx.closePath();
      ctx.stroke();
    }
    ctx.restore();
  }
}

function rasterTri(px, zbuf, res, a, b, c, depth, uvs, shade) {
  const [ax, ay] = a, [bx, by] = b, [cx, cy] = c;
  const x0 = Math.max(0, Math.floor(Math.min(ax, bx, cx)));
  const x1 = Math.min(res - 1, Math.ceil(Math.max(ax, bx, cx)));
  const y0 = Math.max(0, Math.floor(Math.min(ay, by, cy)));
  const y1 = Math.min(res - 1, Math.ceil(Math.max(ay, by, cy)));
  if (x1 < x0 || y1 < y0) return;
  const denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
  if (Math.abs(denom) < 1e-9) return;
  const [za, zb, zc] = depth;
  const [ua, va] = uvs[0], [ub, vb] = uvs[1], [uc, vc] = uvs[2];
  if (!state.texData) return;
  // Declared texel -> normalised (the shader divides by texture_width/height)
  // -> PNG pixel. When the PNG matches the declaration this is the identity.
  const uScale = state.texW / state.tw;
  const vScale = state.texH / state.th;
  for (let py = y0; py <= y1; py++) {
    for (let pxx = x0; pxx <= x1; pxx++) {
      const pxp = pxx + 0.5, pyp = py + 0.5;
      const w0 = ((by - cy) * (pxp - cx) + (cx - bx) * (pyp - cy)) / denom;
      const w1 = ((cy - ay) * (pxp - cx) + (ax - cx) * (pyp - cy)) / denom;
      const w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      // Perspective-correct interpolation, as the game's GPU does for its
      // varyings: interpolate (u/z, v/z, 1/z) and divide. Affine interpolation
      // would pull a diagonal seam across every foreshortened face once the
      // camera is perspective.
      const w0z = w0 / za, w1z = w1 / zb, w2z = w2 / zc;
      const invZ = w0z + w1z + w2z;
      if (!(invZ > 0)) continue;
      const z = 1 / invZ;
      const idx = py * res + pxx;
      if (z >= zbuf[idx]) continue;
      const u = (w0z * ua + w1z * ub + w2z * uc) / invZ;
      const v = (w0z * va + w1z * vb + w2z * vc) / invZ;
      // NEAREST + REPEAT, like the entity sampler: floor to a pixel, then wrap
      // instead of clamping, so an off-texture net tiles here as it does in game.
      const tx = wrap(Math.floor(u * uScale), state.texW);
      const ty = wrap(Math.floor(v * vScale), state.texH);
      const ti = (ty * state.texW + tx) * 4;
      if (state.texData[ti + 3] < ALPHA_CUTOFF) continue;
      const r = state.texData[ti], g = state.texData[ti + 1], bl = state.texData[ti + 2];
      zbuf[idx] = z;
      const o = idx * 4;
      px[o] = Math.min(255, r * shade);
      px[o + 1] = Math.min(255, g * shade);
      px[o + 2] = Math.min(255, bl * shade);
      px[o + 3] = 255;
    }
  }
}

/* ------------------------------------------------------------ self-check -- */

function runSelfCheck() {
  const badge = $("self-check");
  if (!state.geo || !state.referenceRects) {
    badge.textContent = "—"; badge.className = "badge";
    return;
  }
  const mismatches = [];
  const cubes = iterateCubes(state.geo);
  for (const cb of cubes) {
    const key = `${cb.boneName}#${cb.index}`;
    const ref = state.referenceRects[key];
    if (!ref) { mismatches.push(`${key} 缺失参考`); continue; }
    for (const fn of FACE_NAMES) {
      const got = cb.faces[fn];
      const want = ref[fn];
      if (!want || Math.abs(got[0] - want[0]) > 1e-4 || Math.abs(got[1] - want[1]) > 1e-4 ||
          Math.abs(got[2] - want[2]) > 1e-4 || Math.abs(got[3] - want[3]) > 1e-4) {
        mismatches.push(`${key}.${fn}`);
      }
    }
  }
  if (mismatches.length === 0) {
    badge.textContent = "映射与游戏一致 ✓";
    badge.className = "badge ok";
    badge.title = "前端 faceRect() 与服务器参考实现（boxUvFaceRect / entity_uv_lib）完全一致";
  } else {
    badge.textContent = `映射偏差 ${mismatches.length}`;
    badge.className = "badge bad";
    badge.title = mismatches.join(", ");
  }
}

/* ------------------------------------------------------------------ save -- */

function buildDocText() {
  // Preserve every field the user did not touch: only uv / mirror / texture
  // dimensions are ever mutated in state.doc, so a plain re-serialise keeps the
  // rest (format_version, pivot/rotation/parent/inflate, extra geometry entries).
  return JSON.stringify(state.doc, null, 2) + "\n";
}

function markDirty() {
  state.dirty = true;
  state.previewFaces = null; // UV/mirror edits change the 3D preview too
  renderAll();
  runSelfCheck();
  setStatus("有未保存的修改。", "warn");
}

async function save() {
  setStatus("保存中 …");
  try {
    const res = await fetch(`/api/model?name=${encodeURIComponent(state.modelName)}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: buildDocText(),
    });
    const result = await res.json();
    if (!res.ok || !result.ok) throw new Error(result.error || `${res.status}`);
    state.referenceRects = result.reference_rects;
    state.dirty = false;
    showNotes(result.notes);
    const written = result.targets.filter((t) => t.written).length;
    setStatus(
      `已保存：源文件 + ${written} 个运行时副本（共 ${result.targets.length} 个目标）。` +
      `下次启动游戏即生效。`, "ok");
    runSelfCheck();
  } catch (err) {
    setStatus(`保存失败：${err.message}`, "bad");
  }
}

// Downloads the very bytes "保存到项目" would write — the server formats it, so
// an exported copy and a saved file can never drift apart in layout.
async function download() {
  if (!state.doc) return;
  try {
    const res = await fetch("/api/format", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: buildDocText(),
    });
    if (!res.ok) throw new Error(`${res.status}`);
    const link = $("download-link");
    link.href = URL.createObjectURL(new Blob([await res.text()], { type: "application/json" }));
    link.download = `${state.modelName}.geo.json`;
    link.click();
    setStatus(`已导出 ${state.modelName}.geo.json（与保存写入的内容一致）。`, "ok");
  } catch (err) {
    setStatus(`导出失败：${err.message}`, "bad");
  }
}

/* ------------------------------------------------------------------ misc -- */

function setStatus(msg, kind) {
  const el = $("status");
  el.textContent = msg;
  el.className = kind ? `status ${kind}` : "status";
}

// Advisory diagnostics from the server. They describe what the game will do
// with the current numbers; they never gate an edit or a save.
function showNotes(notes) {
  const host = $("notes");
  host.textContent = "";
  for (const note of notes || []) {
    const div = document.createElement("div");
    div.className = "note";
    div.textContent = `⚠ ${note}`;
    host.appendChild(div);
  }
}

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
// Positive modulo: VK_SAMPLER_ADDRESS_MODE_REPEAT for negative texels too.
function wrap(v, n) { return n > 0 ? ((v % n) + n) % n : 0; }

function renderAll() {
  renderNet();
  renderPreview();
}

function setView(name) {
  const views = {
    front: [0, 0], back: [180, 0], left: [90, 0], right: [-90, 0],
    top: [0, 89], "3q": [-35, 22],
  };
  if (!views[name]) return;
  [state.viewYaw, state.viewPitch] = views[name];
  renderPreview();
}

/* -------------------------------------------------------------- listeners -- */

function bindEvents() {
  $("model-select").addEventListener("change", (e) => {
    loadModel(e.target.value).catch((err) => setStatus(`加载失败：${err.message}`, "bad"));
  });

  $("save-btn").addEventListener("click", save);
  $("download-btn").addEventListener("click", download);
  $("download-link").addEventListener("click", (e) => e.stopPropagation());

  const texW = $("tex-width"), texH = $("tex-height");
  const applyDims = () => {
    // A texture size has to be a positive integer, but nothing else about it is
    // the editor's business: an out-of-range entry is rejected and the previous
    // value restored, never silently coerced into something else.
    const tw = texW.valueAsNumber, th = texH.valueAsNumber;
    const valid = Number.isInteger(tw) && Number.isInteger(th) && tw >= 1 && th >= 1;
    if (!state.geo || !valid) {
      texW.value = state.tw; texH.value = state.th;
      if (!valid) setStatus("纹理尺寸必须是 ≥ 1 的整数，已恢复原值。", "bad");
      return;
    }
    if (tw === state.tw && th === state.th) return;
    state.geo.description = state.geo.description || {};
    state.geo.description.texture_width = tw;
    state.geo.description.texture_height = th;
    state.tw = tw; state.th = th;
    $("info-declared").textContent = `${tw} × ${th}`;
    markDirty();
  };
  texW.addEventListener("change", applyDims);
  texH.addEventListener("change", applyDims);

  const selU = $("sel-uv-u"), selV = $("sel-uv-v");
  const applyUv = () => {
    const cube = selectedCube();
    if (!cube) return;
    // Whatever the developer types is what gets stored — no clamping to the
    // declared texture, no rounding.
    if (!setCubeUv(cube, selU.valueAsNumber, selV.valueAsNumber)) {
      syncSelInputs();
      return;
    }
    selectCube(state.selected.boneName, state.selected.cubeIndex);
    markDirty();
  };
  selU.addEventListener("change", applyUv);
  selV.addEventListener("change", applyUv);
  $("sel-mirror").addEventListener("change", () => {
    const cube = selectedCube();
    if (!cube) return;
    cube.mirror = $("sel-mirror").checked;
    state.previewFaces = null;
    selectCube(state.selected.boneName, state.selected.cubeIndex);
    markDirty();
  });

  // ---- per-rect relabel (face assignment) and 180° rotation ----
  const selFaceAs = $("sel-face-as"), selFaceRot = $("sel-face-rot");
  const applyFaceOverride = () => {
    const cube = selectedCube();
    if (!cube || !state.selectedFace) return;
    const as = selFaceAs.value;
    const rotate = selFaceRot.checked ? 180 : 0;
    if (!setFaceOverride(cube, state.selectedFace, as, rotate)) { syncSelInputs(); return; }
    state.previewFaces = null;
    selectCube(state.selected.boneName, state.selected.cubeIndex, state.selectedFace);
    markDirty();
  };
  selFaceAs.addEventListener("change", applyFaceOverride);
  selFaceRot.addEventListener("change", applyFaceOverride);

  // ---- net canvas: click to select, drag to move the cube's whole net ----
  const netCanvas = $("net-canvas");
  netCanvas.addEventListener("pointerdown", (e) => {
    const [tx, ty] = texelAtEvent(e);
    const hit = cubeAtTexel(tx, ty);
    if (hit) {
      selectCube(hit.cb.boneName, hit.cb.index, hit.face);
      netCanvas.setPointerCapture(e.pointerId);
      state.netDrag = { startX: tx, startY: ty, startUv: hit.cb.uv.slice() };
    } else {
      selectCube(null, null);
    }
  });
  netCanvas.addEventListener("pointermove", (e) => {
    if (!state.netDrag) return;
    moveNet(e);
  });
  const endDrag = (e) => { if (state.netDrag) { state.netDrag = null; } };
  netCanvas.addEventListener("pointerup", endDrag);
  netCanvas.addEventListener("pointercancel", endDrag);

  // ---- arrow-key fine nudge ----
  window.addEventListener("keydown", (e) => {
    const tag = (document.activeElement && document.activeElement.tagName) || "";
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    const step = e.shiftKey ? 4 : 1;
    if (e.key === "ArrowLeft") { nudgeSelected(-step, 0); e.preventDefault(); }
    else if (e.key === "ArrowRight") { nudgeSelected(step, 0); e.preventDefault(); }
    else if (e.key === "ArrowUp") { nudgeSelected(0, -step); e.preventDefault(); }
    else if (e.key === "ArrowDown") { nudgeSelected(0, step); e.preventDefault(); }
  });

  // ---- 3D preview orbit ----
  const preview = $("preview-canvas");
  let orbiting = null;
  preview.addEventListener("pointerdown", (e) => {
    orbiting = { x: e.clientX, y: e.clientY };
    preview.setPointerCapture(e.pointerId);
  });
  preview.addEventListener("pointermove", (e) => {
    if (!orbiting) return;
    state.viewYaw -= (e.clientX - orbiting.x) * 0.6;
    state.viewPitch = clamp(state.viewPitch + (e.clientY - orbiting.y) * 0.6, -89, 89);
    orbiting = { x: e.clientX, y: e.clientY };
    renderPreview();
  });
  preview.addEventListener("pointerup", () => { orbiting = null; });
  preview.addEventListener("pointercancel", () => { orbiting = null; });
  preview.addEventListener("wheel", (e) => {
    e.preventDefault();
    state.zoom = clamp(state.zoom * (e.deltaY > 0 ? 0.9 : 1.1), 0.35, 3);
    renderPreview();
  }, { passive: false });

  document.querySelectorAll(".views button").forEach((btn) => {
    btn.addEventListener("click", () => setView(btn.dataset.view));
  });
  $("show-net").addEventListener("change", renderPreview);
}

/* ------------------------------------------------------------------ init -- */

async function init() {
  bindEvents();
  try {
    await loadModels();
  } catch (err) {
    setStatus(`初始化失败：${err.message}。是否已启动 python3 tools/texture_editor/server.py？`, "bad");
  }
}

init();
