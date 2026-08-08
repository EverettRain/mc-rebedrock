# Unified data-driven animation

One runtime animates **everything** in the game — blocks (a chest lid), dropped
items, the player, NPCs and mobs — from the same JSON files, using the Bedrock
geometry and animation schema. Because the project reimplements Bedrock, its own
asset format is the natural single source of truth: a `*.geo.json` describes the
shape and bone hierarchy, a `*.animation.json` describes how bones move, and the
game feeds per-frame inputs through a small Molang expression context.

This code builds as **`mc_rebedrock_animation`**, a standalone static library
that depends only on glm and the standard library — no gameplay and no rendering
dependencies — so it can be reused and unit-tested on its own (`tests/` links the
library alone). Game/render code consumes it through thin adapters.

```
resources/animation/*.geo.json    ──►  SkeletalModel    (bones + cubes)      ┐
resources/animation/*.animation.json ► AnimationLibrary (clips of channels)  ├─► Animator ─► SkeletonPose ─► bone matrices ─► renderer
game state (walking, look, lid…)  ──►  MolangContext    (query.* / variable.*)┘
```

## Modules

| File | Responsibility |
|------|----------------|
| `core/Json.{hpp,cpp}` | Dependency-free JSON reader (objects, arrays, escapes, `//` comments). |
| `animation/Molang.{hpp,cpp}` | Compiles/evaluates Bedrock Molang expressions. Pure expressions constant-fold. |
| `animation/SkeletalModel.{hpp,cpp}` | Loads a `minecraft:geometry` document into bones + cubes with a resolved parent hierarchy. |
| `animation/AnimationClip.{hpp,cpp}` | Loads `animations`: per-bone rotation/position/scale channels, keyframed or expression-driven, with linear/step/catmull-rom/**bezier** interpolation. Also a programmatic keyframe API. |
| `animation/Animator.{hpp,cpp}` | Blends weighted clip layers over a model and resolves a `SkeletonPose` into hierarchical bone matrices. |
| `animation/AnimationAssets.{hpp,cpp}` | Loads geometry + animation files from disk into an `AnimatedModel` bundle. |
| `animation/HingeAnimation.{hpp,cpp}` | Reusable one-DOF hinge preset (chest/door lid) — a Bezier ease-out driven by open progress. |
| `animation/DisplayEntityAnimation.{hpp,cpp}` | Reusable float+spin preset for dropped-item display entities (Molang-authored). |

## Keyframes and Bezier interpolation

Channels interpolate their keyframes with a `lerp_mode`: `linear`, `step`,
`catmullrom`, or `bezier`. Bezier keyframes carry per-keyframe tangents
(`right_tangent` leaving a key, `left_tangent` arriving at the next) expressed as
value-per-second slopes; the segment is evaluated as a cubic Hermite. Flat (zero)
tangents give a symmetric ease-in/ease-out; explicit slopes give ease-out,
overshoot, etc. A cubic ease-out `A·(1-(1-t)³)` is exactly a Bezier segment with
`right_tangent = 3A/duration` at the start and a flat tangent at the end — which
is how the chest lid keeps its original feel.

```json
"lid": { "rotation": {
  "0.0": {"post": [0, 0, 0],   "right_tangent": [-540, 0, 0]},
  "0.5": {"post": [-90, 0, 0], "lerp_mode": "bezier", "left_tangent": [0, 0, 0]}
}}
```

Clips can also be built in C++ without JSON via the keyframe API —
`AnimationChannel::addLinear/addStep/addBezier/addEased` and
`AnimationClip::setLength/setLoop/bone()` — which is how the reusable presets
(`HingeAnimation`, `DisplayEntityAnimation`) are authored.

## Why Molang

Walk cycles, idle sway and look-at are *authored*, not hardcoded. A channel can
be a literal (`[0, 30, 0]`), a keyframe map (`{"0.0": [...], "0.5": [...]}`) or a
Molang string that reads live inputs:

```json
"rightLeg": { "rotation": ["math.cos(query.anim_time * 360) * 40 * variable.walk_amount", 0, 0] }
```

The game supplies inputs via `Animator::context()`:
`query.anim_time` (clip time, set automatically), plus authored `variable.*`
values such as `variable.walk_amount`, `variable.look_yaw`, `variable.lid_angle`.

Supported: `+ - * /`, unary `-`/`!`, comparisons, `&& || ?:`, `;` sequences,
`math.sin/cos` (degrees), `abs sqrt floor ceil round trunc exp ln pow min max
mod clamp lerp lerprotate`, `math.pi`, and the `q.`/`v.` aliases. Missing
lookups resolve to `0`, matching Bedrock.

## Blending

`Animator` stacks layers additively — the way Bedrock composes animations.
Rotations and positions sum (scaled by each layer's weight); scale composes
multiplicatively toward 1. So a looping `walk` at weight 1 plus a procedural
`look` override at weight 1 combine without either being authored to know about
the other. See `tests/animation_assets_test.cpp` for the player walk+look blend,
the data-driven chest lid, and the quadruped walk cycle all going through this
one runtime.

## Coordinate / rotation convention

Model space is Bedrock's: 16 units per block, Y up. Bone rotations are applied
in Z → Y → X order about the bone pivot, in degrees, matching Minecraft's bone
stack. `SkeletonPose::worldMatrix` walks the parent chain so children inherit
their parent's transform (the chest knob rides the lid; the player head rides
the body).

## Adding a new animated thing

1. Author `resources/animation/<name>.geo.json` (bones + cubes).
2. Author `resources/animation/<name>.animation.json` (clips).
3. `auto bundle = animation::loadAnimatedModel(geo, {anim});`
4. Each frame: set `animator.context()` inputs, queue clips with
   `addLayer`/`playSingle`, call `evaluate()`, and draw each cube with
   `pose.worldMatrix(boneIndex)`.

## Migrated legacy animators

The two pre-existing animators now run entirely on this library while keeping
their original public interfaces (so the renderer's draw code is unchanged):

- **`PlayerModelAnimator`** (inventory/creative preview) blends the
  `animation.player.walk` / `idle` / `look` clips over the player geometry with
  an `Animator`, then projects the resulting bone rotations into the flat
  `PlayerModelPose` the preview renderer consumes. Walk/idle/look are now
  authored Molang, not hardcoded trig.
- **`ModelAnimationSystem`** (first-person held item) samples the
  `animation.held_item.break` / `use` / `eat` clips through `AnimationChannel`;
  swing progress is the clip's normalised time. `break`/`use` are one-shot swings
  that end with their clip; `eat` is a *held* action whose lifetime gameplay owns
  (the 32-tick meal, or until the button is released), so its clip holds the last
  frame rather than looping or lapsing back to the rest pose. It is authored as
  Molang rather than keyframes because vanilla's
  `HeldItemRenderer#applyEatOrDrinkTransformation` is a closed form in the
  remaining use time — `h = 1 - g^27` for the lift and `abs(cos(f/4·π))·0.1` for
  the chewing bob — which reads far better as that function than as sampled keys.

Both compile in a built-in copy of their clips (so unit tests stay hermetic and
the game animates even with no resource pack) and call `load()` at startup to
override from `resources/animation/{player,held_item}.*.json`. The renderer wires
this up once in its constructor; a missing/broken file silently falls back to the
built-in clips.

- **Chest lid** now takes its lift angle from `HingeAnimation` (a Bezier ease-out
  whose tangents reproduce the previous `1-(1-t)³` easing to 1e-4), instead of a
  hardcoded `std::pow` in the renderer. This migration also fixed a long-standing
  bug: the lid visibly *changed size* as it rotated, because the item vertex
  shader scaled the non-uniform lid box **after** rotating it (`(R·local)·dims`),
  which shears a box under non-uniform scale. The shader now scales in the box's
  own axes first, then rotates (`R·(local·dims)`) — rigid for every part, and
  identical to before for the uniform/matrix paths.
- **Dropped items** take their float height and spin from `DisplayEntityAnimation`
  (Molang-authored curves that reproduce the old formula exactly) instead of
  inline trig in the draw loop.

These two use reusable, game-agnostic presets rather than bone hierarchies: a
chest lid and a dropped item are single-DOF, so a full multi-bone skeleton would
be overkill.

## World-space skinned rendering

The item shader has a **world-matrix cuboid mode** (`data.x == 8`): each cube is
drawn by a full world matrix (translation + orientation), scaled before rotation
(rigid), with world-space normals so the fixed scene light stays orientation-
correct. Two consumers:

- **Chest** — each part's world matrix composes `T(center)·Ryaw·Rpitch`, so the
  lid hinges about the chest's *local* axis. Previously the shader applied
  `pitch·yaw` (pitch about world-X), so the lid always opened along one world
  direction and only looked right when the chest happened to face the default
  way.
- **Third-person player** — `PlayerModelAnimator` exposes its `SkeletalModel` and
  `SkeletonPose`; the renderer draws each bone's cubes as
  `modelRoot · worldMatrix(bone) · T(cubeCentre)`, i.e. genuine multi-bone
  skeletal rendering. F5 cycles first / third-back / third-front, the camera
  booms in against walls, and the whole view keeps the first-person bobbing. The
  `animation.player.look` clip now rotates the **head only**; the renderer keeps a
  lagged body yaw so the head leads a turn and drags the body only past its limit
  (~55°). Sneaking blends in an `animation.player.sneak` clip (torso leans and
  lowers; legs are root-level bones so they stay planted), and walk/sneak blend
  weights are **eased** in `PlayerModelAnimator::update` so states fade instead of
  snapping. This is the path real NPCs and mobs will reuse; the remaining piece
  for arbitrary mobs is UV-atlas skinning (this player reuses the pre-baked
  per-face texture layers).

## Performance

Measured with `-O2` on this machine (2 M iterations, single model per frame):

| Path | Old procedural | New data-driven |
|------|---------------:|----------------:|
| player preview `update()` (walk+idle+look blend, 6 bones) | ~3 ns | ~410 ns |
| held-item `update()` (3 channels) | ~1 ns | ~80 ns |

The data-driven path is absolutely more expensive (Molang evaluation, bone
lookups and a pose buffer), but it runs on a *handful* of models per frame: the
one preview model costs ~0.0025 % of a 16.6 ms frame, and 100 mobs at the player
cost would be ~41 µs (~0.25 %). So there is **no meaningful frame-time
regression** — the trade is a tiny, bounded CPU cost for full data-driven
flexibility.

The largest single cost was `Animator::evaluate()` cloning the whole
`MolangContext` per layer; it now copies once per frame (a ~23 % speedup). If
very large mob counts ever need it, the next wins are: reuse the `SkeletonPose`
buffer across frames, bind clip bones to model indices once at load, and intern
`variable.*` names to avoid per-sample string hashing.
