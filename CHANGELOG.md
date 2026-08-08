# ReBedrock Changelog

All notable player-facing changes are recorded here. The project follows a
simple versioned history while it is in beta.

## Unreleased

### Added

- Decorative stone variants round out the stone family: polished granite,
  polished diorite and polished andesite craft from a 2x2 block of their parent
  stone (four of the polished product), and smooth stone smelts from stone in
  the furnace — the same shapes and recipes as 1.16.1. All four register into
  the Building Blocks creative tab and save palettes.
- `/spawnpoint [<x> <y> <z>]` sets a personal respawn point the way 1.16.1's
  SpawnPointCommand does: with no position the command uses your current block,
  `~` axes resolve relative to you, and death respawns there before falling
  back to the world spawn. The result persists with the save (format 10).
- Iron buckets now fill and pour: an empty bucket scoops a still-water source
  into a full water bucket, and a water bucket empties back into the empty
  bucket when poured. Creative keeps the bucket for ever — it never spends the
  empty one nor the full one — and its bucket ray stops only at a still source,
  so flowing water is walked past and the block behind it stays breakable.
- Dropped items gained simple block collision (they land, lean on walls and
  never pass through), drift toward the player with a visible magnet pickup
  animation before being collected, and identical items within the same cell
  merge into a single group (capped at the stack size) so a pile of drops
  renders as one icon. Once resting they brake at the floor's slipperiness, so
  a thrown drop does not slide half a biome.

### Changed

- Eating now plays the chewing loop through the meal: the `generic.eat` sound
  fires every fourth tick once the eat is past its first seven ticks, with the
  final burst and burp on completion — matching LivingEntity's consumption
  effects instead of a single sound when the meal starts.
- Inventory block thumbnails use vanilla 1.16.1's plain per-face luminance
  (up 1.0, west 0.6, east 0.8) with no colour bias, plus a per-corner ambient
  occlusion gradient so the cube edges read slightly darker than the faces.

### Fixed

- The backpack preview figure now turns with the cursor look through its bone
  hierarchy (head and arms as children of the body), so the whole body rotates
  rigidly around its pivots instead of every part spinning around its own
  centre.
- Shift-clicking a crafting result quick-moves the whole batch into the main
  grid before the hotbar, merging into existing stacks before empty slots —
  the quick-move pass now merges first then fills, and the click keeps crafting
  while the ingredients last and the result finds a home.
- Dark oak trees spread a proper vanilla canopy: the 2x2 trunk leans near the
  top under a four-layer crown whose widest layer is 8x8, with 1-in-3 branch
  columns carrying their own small foliage nodes. The crown hangs off the trunk
  top log, not a layer above it.
- A dead mob drops its loot in every game mode — the creative gate that
  silently swallowed the drops is gone, matching vanilla's death-time loot roll.

## ReBedrock beta3

### Added

- A "High" smooth-lighting tier (`lighting.smooth=high`), implementing the
  vanilla 1.16.1 per-block ambient occlusion: full opaque cubes darken corners
  to 0.2, a corner whose diagonal is enclosed probes two cells up in the face
  normal (the overhang), and the corner averages the two in-plane sides, the
  selected corner and the block across the face. Stronger contact shadows and
  smoother gradients than the standard tier.
- A Vertical Sync option on the Video Settings page (`render.vsync`, persisted
  in options.properties). On, the swapchain presents with FIFO — the
  presentation engine waits for the display refresh, so the frame rate locks to
  the monitor with no tearing and costs no CPU. Off keeps MAILBOX's
  drop-on-demand presentation for uncapped frames. Toggling it rebuilds the
  swapchain with the matching present mode.
- A generic game-rules registry. Each rule is one row in a compile-time table
  (name, type, default, bounds), and `/gamerule` parsing, save/load and change
  notification all derive from it — adding a rule no longer touches the save
  format, the command handler or the renderer. `randomTickSpeed` is joined by
  `doDaylightCycle` (freezes the day/night cycle while `/time set` still works,
  like 1.16.1) and `keepInventory` (keeps the inventory through death).
  `/gamerule <rule>` now queries the current value, and rule names accept
  `minecraft:` prefixes and any case.
- Game rules are persisted as a sparse, self-describing block in `world.dat`
  (save format 9): only rules that differ from their default are written, every
  entry carries its name/type/length/value, and the block has its own version
  and size so an older build skips what it cannot read. A format-8 save's
  `randomTickSpeed` header field is migrated into the block on load.
- Java 1.16.1's random-tick simulation. Every game tick, `randomTickSpeed`
  random blocks are drawn per loaded non-empty 16×16×16 section (3 by default,
  matching vanilla) and blocks with a random tick react: a grass block spreads
  to plain dirt in light and reverts to dirt when covered by water or left
  dark, and saplings grow into trees under open sky. `/gamerule randomTickSpeed
  <n>` sets the rate (0 disables random ticks; the value is clamped at 1000 so
  a typo cannot stall the render thread) and is saved with the world like
  difficulty. Leaf decay now follows the gamerule too. A batch of saplings that
  mature together grow at most two per tick, so a forest appears over a few
  seconds instead of stalling a frame.
- The vanilla `ui.button.click` sound plays on button presses. Menu buttons
  (title, options, world list, pause, death) and the create/rename confirm
  click with the 1.16.1 `random/click_stereo` clip the way
  `AbstractButtonWidget#playDownSound` does, played at the listener so the
  master-category click is always audible. Only genuine buttons click — the
  inventory/container slots, creative tabs and the delete slot stay silent,
  and the view-distance and master-volume sliders keep their own drag
  feedback.
- Basic 1.16.1 mouse gestures in the inventory and containers. Shift-click
  (SlotActionType.QUICK_MOVE) moves a stack between a chest/crafting/furnace
  slot and the player inventory in either direction — a furnace routes a
  smeltable item to its input and anything burnable to its fuel slot. And
  dragging (SlotActionType.QUICK_CRAFT) works like vanilla: pick a stack up and
  sweep it over slots while holding the button; a left-drag shares the stack
  out as evenly as possible, a right-drag drops a single item into each slot.
  Double-clicking a slot (SlotActionType.PICKUP_ALL) gathers every matching
  stack in the screen into the cursor, the quick way to tidy scattered items.
- A burning furnace now shows it: the front face switches to 1.16.1's
  `furnace_front_on` texture and the block glows with the lit furnace's level-13
  light while fuel burns, then reverts when the fire goes out. The lit state is
  transient — it is not saved, and breaking a burning furnace drops the plain
  furnace item.
- A cow species, ported from 1.16.1 `CowEntity` and `QuadrupedEntityModel`: the
  second CREATURE species, reusing the animal AI with no shared-code changes. It
  binds the vanilla `entity/cow/cow.png` skin, runs a 1.16.1-style gait (0.6662
  leg frequency, 1.4 rad swing, diagonal leg pairing), drops 1–3 raw beef and
  0–2 leather, and has its own spawn egg. The spawn-area demo herd is now 2 pigs
  + 2 cows, and a raw beef item (with a vanilla `item/beef.png` icon) joins the
  food catalogue.
- A crop-farming system, following 1.16.1's blocks, loot and crafting. Four new
  blocks — farmland (tilled with a hoe), and the wheat, carrots and potatoes
  crops — plus wheat seeds, wheat, carrot and potato items. A hoe right-clicks
  dirt, grass and podzol into farmland (coarse dirt back into plain dirt, the
  vanilla HOE_LOOKUP map) and wears one durability point per till. Seeds,
  carrot and potato plant on farmland; the crops grow on the random tick, need
  light 9 in the block above, and grow up to three times faster ringed by moist
  farmland (1.16.1's getAvailableMoisture and the 25/moisture growth odds), so
  `/gamerule randomTickSpeed` scales the rate like every other random tick.
  Mature wheat drops wheat plus a binomial roll of seeds, carrots and potatoes
  drop their produce, and an unripe crop returns a seed; tall grass drops wheat
  seeds 1/8 of the time. Three wheat in a row craft bread. Breaking farmland
  yields dirt and pops any crop standing on it, with the crop's loot rolled
  from the age it had reached.
- Farmland moisture, per 1.16.1's FarmlandBlock: water within four blocks
  hydrates the soil (the moisture jumps to 7 and the top texture wets), and dry
  farmland dries one level at a time, reverting to plain dirt once it reaches
  zero and nothing is planted on it.
- Farmland trampling (FarmlandBlock#onLandedUpon): landing on tilled soil with
  more than half a block of fall has a chance — `nextFloat() < fallDistance -
  0.5`, so a one-block fall breaks it half the time — to turn the farmland back
  to dirt and pop the crop above it. The fall distance is tracked per player
  the way Entity#fallDistance is; walking never tramples.
- Dropped items are 3D. Non-block items (tools, materials, food) drop as the
  held item's single-layer slab model — the icon as a thin 3D card with
  extruded edges — spinning about Y, instead of a flat camera-facing sprite,
  the way 1.16.1's ItemEntityRenderer draws the same ItemRenderer model in
  GROUND transform. Block items keep their miniature cube.
- Crops select like their stage: the raycast box grows from 2/16 (a sprout) to
  a full block as the crop ages (1.16.1's CropBlock.SHAPES), and the selection
  outline follows it, so you aim at the plant rather than the empty air above
  it.

### Changed

- Smooth Lighting is now a three-way setting — Off / Minimum / Maximum. The
  mesh is baked at the active quality (the packed vertex carries one AO set),
  so switching tiers remeshes the loaded world; Off keeps the last baked mesh
  and just disables the smooth light channels in the shader.
- Chunk meshing samples through a per-request O(1) snapshot of the chunk and
  its neighbours' blocks and light instead of a chunk-map hash lookup for every
  corner (~13 per corner before). Standard-quality meshes are byte-identical.
- A Controls page now groups the control options — View Bobbing (moved out of
  the video page) and the new Auto-Jump toggle — and the Video Settings page
  lays its settings out in two centred columns with the "Done" button on its
  own centred row beneath, so the ten-entry page stays centred on screen and
  the bottom button is no longer dropped off the render queue.
- A dedicated Language page, following 1.16.1's LanguageScreen: a dark
  scrollable list that spans the canvas left-to-right like the save-selection
  screen's list rows, each language shown in its own script, with the Force
  Unicode Font toggle and the Done button. The language and Force Unicode
  options moved here out of the Options page, and the video settings' remaining
  hardcoded labels (anti-aliasing, anisotropic filtering, dynamic lighting,
  vertical sync) now translate along with the rest.
- Selecting English no longer turns the Chinese entry in the language list into
  question marks: the font's unicode pages are built from the language screen's
  display names as well as the active language, so the CJK glyphs stay loaded
  even while a pure-ASCII language is in use.
- Farmland is no longer a full block: its mesh, collision box and selection box
  are the vanilla 15/16 shape, so the player stands a sixteenth lower on it and
  the soil reads as the tilled field it is.
- Crops render as the vanilla `crop` blockstate model — four orthogonal thin
  planes at the quarter offsets (x=4/16, x=12/16, z=4/16 and z=12/16), with the
  plant's base sinking to the farmland surface — instead of the two diagonal
  planes of a `cross`.

### Fixed

- Block-break dust now falls instead of drifting up into the camera. The
  particle gravity constant was 20× too weak: vanilla applies 0.04
  blocks/tick² (16 blocks/s² in per-second terms), but the code subtracted
  only 0.8 blocks/s² per second, so every particle kept its upward kick and
  flew outward — and the dust cloud climbing toward the eye blocked the view
  of the block behind a broken one, making it look like the ray had to wait
  for the particles to clear before the next block could be mined. Both are
  fixed by the correct gravity: the dust sheds its kick, settles on the floor
  and drops out of the line of sight.
- Old saves with well-watered farmland or mature crops load again. Crops and
  farmland store their age/moisture (0-7) in the per-cell orientation byte, but
  the world.dat loader only accepted the six enumerated `BlockOrientation`
  facings, so a save containing a moisture-6/7 farmland block (or an age-6/7
  crop) failed to open with "invalid block edit". The loader now accepts the
  full 0-7 state range.
- A grounded player can again step up onto a rise while walking diagonally.
  The player movement only resolved each axis in turn and never re-attempted a
  wall-blocked horizontal move from a step up, so hugging a block's edge and
  trying to move onto the diagonally-placed block got stuck and slid off. The
  horizontal move now mirrors 1.16.1's `adjustMovementForCollisions`: the
  dominant axis resolves first, and a blocked grounded move is retried from
  vanilla's 0.6 step. The player never lifts a full block on their own — the
  0.6 step only clears stairs and partial blocks — so a one-block rise still
  needs a jump.
- The first pass at that step let the player climb a full block by walking,
  which is not how a Java 1.16.1 player moves; it was removed again. In its
  place a Bedrock-style auto-jump is available: with Auto-Jump on in Controls,
  walking forward into a one-block rise jumps automatically (it never fires
  against a two-high wall or a missing headroom). It is off by default, keeping
  the vanilla feel.
- Tree crowns no longer stop flat at the chunk border. Chunk generation runs
  in isolation, so a tree near the edge used to drop every leaf past the 0..15
  box — a large share of trees came out clipped. The generation writer now
  holds those blocks back and the streamer applies them to the neighbouring
  chunk when it is published, so the crown is finished across the seam. The
  oak/birch foliage was rebuilt to 1.16.1's actual `BlobFoliagePlacer`: the
  ball hangs three rows off the trunk top (the foliage height, not the full
  trunk length) with the radius widening by one every two rows — the earlier
  pass stretched the ball down the whole trunk into an oversized pyramid.
- Deep-water shading no longer reads as self-illuminated at night: the depth
  darkening is applied relative to the already-lit colour instead of mixing in a
  fixed bright colour that held its brightness after dark.
- The held hand/block now follows the ambient light at the player's eye (and the
  entity/held-item shader picks up the cool night sky tint), so what you hold
  darkens with the world instead of staying under fixed light.
- The ocean surface no longer reads as a murky grey sheet: vanilla's
  `water_still`/`water_flow` textures are grayscale, and the blue now comes from
  an applied ocean water-colour tint, so the surface is blue instead of grey —
  and no longer turns whitish at night.
- Night sky light is now cool-tinted like vanilla (blue moonlight instead of
  achromatic light), so desert sand reads pale at night instead of keeping its
  yellow cast.
- Streaming no longer leaves permanent holes when the mesh backlog overflows:
  a section evicted from the pending queue before it reached the GPU is now
  re-requested and re-delivered, instead of staying missing until a placed block
  forces a remesh. The pending cap stays bounded (2048) so memory pressure is
  unchanged.
- The "Maximum" smooth-lighting tier no longer snaps at block boundaries: its
  corner AO now averages the four surrounding cells symmetrically (vanilla's
  {1.0, 0.8, 0.6, 0.4, 0.2} ladder), so adjacent blocks agree exactly on shared
  corners and the gradient stays continuous. Night and cave brightness were
  calibrated toward vanilla (night sky minimum 0.08→0.03, illumination floor
  0.035→0.02).
- Sections whose GPU occlusion-query result was two frames stale no longer pop
  out as holes when the camera turns quickly. Accumulated rotation or
  translation past a small threshold now drops the stale "occluded" states so
  the revealed sections draw and re-query immediately, the per-frame
  fast-motion threshold was lowered to cover smooth fast pans, and the stream
  request centre leads toward the view direction so area revealed by turning is
  loaded ahead.
- Two sound assets the game plays were missing from the staged runtime and
  spammed `Missing sound asset:` on every use. Footsteps rolled six
  variations for every block family even though 1.16.1's `step/` directories
  are uneven — sand has five clips, gravel and cloth four — so walking on sand
  kept asking for a `step/sand6.ogg` that does not exist; the step rolls now
  match the real clip counts. And the pig hurt/death clips (`mob/pig/say*`,
  `mob/pig/death`) were never copied into the game directory because the
  runtime staging only shipped the dig/step/liquid/random/damage sound groups;
  the pig's folder is now staged too.
- Clicking an inventory or container slot to pick up or move an item played the
  button click sound. In 1.16.1 only `AbstractButtonWidget` buttons click;
  slots, the creative tabs and the delete slot are silent, so the click now
  fires only for the menu buttons.
- Sand and gravel stopped falling. A chunk-load pass that scheduled fall checks
  for freshly generated gravity blocks queued *every* sand cell in the chunk,
  and a desert chunk holds thousands of supported ones; with the shared queue
  draining 64 per tick, the genuinely floating blocks were buried behind a
  backlog that never drained. The pass is gone entirely: as in 1.16.1,
  generation writes blocks without ever calling `FallingBlock#onBlockAdded`, so
  floating sand the surface pass leaves behind hangs in place until a neighbour
  change activates it — and costs no simulation time while the player is
  elsewhere.
- Random ticks no longer stall the frame. The scan used to re-resolve the chunk
  hash map for every one of the tens of thousands of draws a high
  `randomTickSpeed` performs per tick, eating most of a frame at speed 100; it
  now reads the drawn cells straight out of the held section (a ~26× cut in
  scan time, so speed 100 costs about 0.4 ms/tick instead of a frame). The
  streaming worker no longer recomputes the full sky column for edits that
  cannot change light — grass reverting to dirt or greening over used to run a
  256-block relight per conversion, which saturated the worker and pushed a
  grown tree's mesh seconds behind its collision box. Both only relight when
  the block's opacity or emission actually changes. And the random-tick systems
  now spend a bounded budget per game tick: at most 32 grass/spreadable
  conversions and 128 leaf-decay checks, plus the existing two tree growths.
  Without the budget a high speed let the spread outrun the worker→mesh→GPU
  pipeline, which stalled player block edits behind the flood and piled GPU
  mesh allocations on until the device ran out of memory; the leftover
  conversions simply wait for later ticks.
- Grass spread no longer churns in place. The spread probes reach one block
  below the surface, and without a light check on the target they flipped that
  buried dirt to grass, which then died for being covered — an endless
  dirt↔grass loop that starved real spread (the grass "stopped spreading" once
  the conversion budget was introduced) and fed the pipeline pointless edits.
  A dirt cell now only greens over if it can hold grass itself: the cell above
  it must be uncovered and lit, the same canSpread condition that keeps grass
  alive in vanilla.
- Streaming a new area no longer hits the frame as hard. One generation batch
  covers 24 chunks, and each new chunk drags up to eight re-meshed neighbours
  with it, so a single batch can hand the render thread hundreds of section
  meshes at once; they were uploaded at 24 per frame for seconds, sustaining a
  low frame rate while the region filled in. The streaming upload budget now
  adapts to the actual frame time: when the GPU is being ground down by a
  streaming batch it drops to six sections per frame so the load damps, and
  when the frame time recovers it returns to sixteen so regions fill in fast
  again — a short hysteresis band keeps the budget from oscillating.
- The frame-rate limit now lands on its target. `sleep_until` wakes 1-2ms late
  on macOS, so a 120fps cap paced the loop at ~100fps — every frame overshot by
  the OS wake latency. The limiter sleeps most of the budget and busy-waits the
  final two milliseconds, so 120 now means 120.
- Blocks next to the shortened farmland keep their side faces. Face culling
  treated farmland as a full opaque cube, so a neighbour's face toward it was
  dropped and the exposed 1/16 sliver above the farmland's top showed a
  see-through gap; a truncated neighbour no longer culls the face against it.
- The cow's model now matches the 1.16.1 `CowEntityModel`. It was built from the
  pig's torso (10×16×8 at box-UV 28,8), so the body came out too short and
  narrow and its net sampled the wrong region of `cow.png`. The torso is now the
  cow's own 12×18×10 at UV 18,4; the head is the vanilla 8×8×6 sitting at world
  y 16..24 (it used to be an 8×8×8 sunk below the body top); the horns and the
  udder the Java model adds as children of the head and torso are present; and
  the legs use the vanilla ±4 pivots with the front pair at z −6. The fix also
  includes the same horn/udder bones in the compiled-in built-in model, so the
  creature stays correct when the resource files are missing.
- The cow and pig no longer wear their belly texture on their backs. The
  body's box-UV faces were mapped for a non-flipped renderer, but 1.16.1 draws
  the same torso in a Y-flipped frame, so the face that landed on the back
  sampled the belly rect — on the cow that put the udder's pink patch at the
  body's front-top ("back of the neck") — and the belly sampled the back rect.
  The `front↔back` / `up↔down` face overrides now re-route the rects so the back
  reads the back texture and the belly the belly texture, exactly like vanilla;
  the pig and the cow share the fix, in the resources and the built-in models.
- The pig walks with the vanilla gait instead of its legs half a stride behind.
  The pig's walk clip swung every leg 180° out of phase with
  `QuadrupedEntityModel#setAngles`, at a 30° swing and one leg cycle per block —
  the pig appeared to shuffle its legs as if the whole set was rotated about the
  body. The clip now matches the cow's: the 0.6662 leg frequency, the 1.4 rad
  (80.2°) swing and the vanilla diagonal pairing, one leg cycle per 1.5 blocks
  travelled.

## ReBedrock beta2

### Added

- A title-screen panorama that follows 1.16.1's `CubeMap` renderer: the six
  panorama faces (`panorama_0` … `panorama_5`) form a unit cube viewed from
  inside at the vanilla 85° field of view, so the wide-angle perspective gives
  each face its natural distortion and parallax. The camera turns slowly — a
  full 360° yaw every five minutes, so every one of the four side faces stays
  centred in front of the view for well over a minute, while a gentle pitch
  sweep dips down to the ground face and up to the sky face and a faint
  vanilla-style sine sway keeps the motion organic. The faces load at native
  1024×1024 resolution into their own texture array and are sampled through a
  dedicated linear sampler.
- A survival experience bar between the hotbar and the status bars, drawn from
  the vanilla 1.16.1 `icons.png` sprites at their original position
  (`Gui#renderExperienceBar`), centred over the hotbar seven logical pixels
  above it. The experience system is not wired up yet, so the bar always shows
  a static placeholder fill instead of reading player progress.
- A session log at `config/rebedrock.log`. Every line written to the console is
  also appended to the file (flushed line by line), so a Vulkan initialisation
  failure or any other fatal error that makes the console flash and close still
  leaves a diagnosable record behind — including the "Fatal error: …" line and
  any validation-layer messages.
- Simplified Chinese interface. The language and a vanilla-style "Force Unicode
  Font" toggle live on the Options page and persist in
  `config/options.properties` (`text.language`, `text.forceUnicodeFont`).
  Translations are read from the 1.16.1 language files, so menus, containers,
  creative tabs and every block/item name localize together. Text is rendered
  through the legacy unicode font (`glyph_sizes.bin` plus the
  `unicode_page_XX.png` pages), uploaded as a font texture array that only holds
  the pages the active language needs.
- Full survival and creative gameplay. Survival players have health, hunger,
  saturation and exhaustion following Java 1.16.1: sprinting and jumping burn
  food, a full food bar regenerates health, an empty one starves, falls past
  three blocks hurt, and staying underwater drowns once the air supply runs out.
  Death drops the inventory, shows the "You Died!" screen and offers Respawn or
  Title Screen. Creative players stay invulnerable and never get hungry.
- Health, hunger and air bars are drawn from the vanilla `icons.png` sprites at
  their original positions, with the damage flash and a red screen tint on hit.
- Block attachment rules. Torches need a sturdy face to sit on or hang from, and
  grass, flowers and saplings need dirt-like soil. Losing that support pops the
  block off with its drop, and one break cascades into the next.
- Player health, food, saturation and air supply are stored in the save file
  (format v4, still able to read v1 through v3).
- A real content registry for blocks and items. Each one is declared by a single
  chained `BlockProperties::of(...)` / `ItemProperties::of(...)` statement in
  `src/world/Block.hpp` and `src/gameplay/Item.hpp` — textures, strength, render
  layer, model, collision, light, support, soil, gravity, facing, stack size —
  and the parallel switch statements that used to carry those traits now read
  the tables. A compile-time check rejects a table that is out of enum order,
  misses an entry or repeats an identifier.
- Namespaced identifiers. Blocks and items are keyed by `rebedrock:oak_planks`
  and `rebedrock:book`, with the `minecraft:` name kept as an alias so 1.16.1
  translations and assets still resolve; lookups accept the registry key, the
  alias or the bare name. States that share one vanilla name, such as the four
  wall torches, now have distinct registry keys of their own.
- World saves (format v6) store blocks and items as palettes of namespaced
  identifiers instead of raw enum ordinals, so content can be added, removed or
  reordered without rewriting existing worlds. Formats 1 through 5 still load
  through frozen tables of the old ordinals, and an identifier this build does
  not know loads as air or an empty stack rather than refusing to open the
  world.
- Block loot tables. A broken block can now yield a different item, several
  stacks at once, or nothing at all, with per-entry chances rolled from the
  world seed. Oak leaves no longer drop themselves: they roll a sapling (5%),
  one or two sticks (2%) and an apple (0.5%). Gravel gives flint 10% of the
  time and gravel otherwise, grass blocks and podzol give dirt, bookshelves
  give three books, emerald ore gives an emerald, and glass drops nothing
  without silk touch. Blocks broken by losing their support or being washed
  away roll the same tables.
- The remaining wood sets. Jungle, acacia and dark oak logs and planks join
  oak, spruce and birch, each converting its log into four planks like the
  originals. Every registered plank counts as the same "any planks" ingredient,
  so the stick, crafting table and chest recipes accept any mixture.
- The full tool rack. Swords, axes, shovels and hoes in all five materials
  (wooden, stone, iron, golden, diamond) join the existing pickaxes, each
  carrying its Java 1.16.1 harvest level, mining speed, attack damage/speed and
  durability in a tool table. Axes are the right tool for wood, shovels for
  dirt, sand and gravel, and hoes for leaves, all craftable through their
  vanilla recipes.
- `/give <item|index> [count]`: hands over any registered block or item by
  namespaced key, vanilla alias or bare name — or by creative-catalog index —
  and spills whatever the inventory cannot hold onto the ground at the player's
  feet.
- Eating. Apple and bread restore hunger and saturation (4 food / 2.4 and
  5 food / 6.0, following Java 1.16.1 FoodStats) after the vanilla 32-tick use
  duration, with a first-person eating animation that raises the held item
  toward the mouth and the vanilla eating and burp sounds.
- Creature fall damage follows 1.16.1: a mob that falls more than three blocks
  takes `ceil(fallDistance - 3)` health on landing (`LivingEntity#computeFallDamage`)
  through the same invulnerability window as a hit, with water cancelling a fall.
  The rule lives in the shared `EntitySystem` tick — the simulation layer every
  species runs through — rather than in any per-species AI.
- Wandering mobs climb one-block-high obstacles: the step-up (`Entity#maxUpStep`)
  now lifts the body all the way to the top of a full block, so a pig or zombie
  walks over a single-block rise instead of milling against its face forever.
- Landing cancels an active creative flight the way
  `ClientPlayerEntity#tickMovement` does. Double-tapping space still takes off
  from the ground, because the toggle jump leaves it on the same tick.

### Changed

- A world now opens with two-phase loading the way 1.16.1 enters a world with a
  small initial area and streams the rest: the load screen waits only for a
  small chunk area around the player, then the full render distance fills in
  progressively during play instead of blocking entry on the whole
  `(2·radius+1)²` area. At a render distance of 12 this cut the time to the
  playable world from about 12.7 s to 2.2 s (5.8×), and the gap only grows with
  the render distance.
- Chunk generation is now parallel. The expensive part of a world load — noise,
  carvers, surface and features — runs on a small pool of worker threads (each
  owning its own generator) instead of a single thread, so a large render
  distance loads in wall-clock time closer to the chunk count divided by the
  cores. Meshing already ran in parallel; generation was the serial bottleneck.
- Blocks are registered and wielded as their own `BlockItem` Item subclasses the
  way 1.16.1's `Items` registry holds `Items.STONE` for `Blocks.STONE`: every
  block has a plain `BlockItem`, and the torch is the `StandingAndWallBlockItem`
  that carries its standing and wall variants. Block identifiers resolve as
  items too, so `/give` and the save palette treat a block like vanilla does.
  The interaction routes placement through the held item's own placement state —
  the torch item decides wall-vs-floor from the clicked face, a plain block item
  places its block — instead of a hardcoded block switch. A block stack still
  keeps its block as its identity, so old saves, mined drops and crafted stacks
  all behave unchanged.
- Right-clicking now routes through 1.16.1's `Item#useOn`: the held item
  resolves the outcome by its own class (a block item places, a spawn egg
  spawns, the buckets collect or pour water), and the interaction loop applies
  the world-edit and audio side effects from the answer instead of comparing the
  item in a chain of `if`s. The container a clicked block opens — crafting
  table, furnace or chest — is read off the block's own registry entry
  (`container`), so no screen in the interaction loop names a block.
- Leaves are wielded as their own `LeavesBlockItem` like 1.16.1, the class that
  marks hand-placed leaves PERSISTENT so they never decay. The flag is stored in
  the block's orientation state, which the item resolves at placement; the
  block-property `placementOrientation` no longer special-cases leaves.
- The tiled `options_background` behind the menus (title sub-screens, the world
  list and the load screen) now keeps the vanilla 32-pixel tile at the current
  GUI scale — every tile is `32 × guiScale` pixels — instead of a fixed 64
  pixels. At GUI scales above 2 the tiles were visibly too small and dense.
- Every menu screen shares the one optimized dirt backdrop, and the singleplayer
  save rows sit on 1.16.1's dark list panel: `options_background` tiled under a
  solid (32,32,32) tint, half the menu dirt's (64,64,64), so the list reads as a
  deep near-black band of dirt. The flat dark overlay over the title-screen
  options is gone, so that screen shows the plain optimized dirt.
- The dirt and any other tinted GUI sprite are tinted in sRGB space, the way
  1.16.1 multiplies the raw texel by the vertex colour. The sRGB swapchain had
  been tinting in linear space, which left every dark tint noticeably brighter
  than vanilla — most visibly the menu dirt.
- New worlds start with an empty inventory instead of a pre-filled one.
- Wood-type blocks follow 1.16.1's mineable/axe tag: logs, planks, bookshelves,
  crafting tables, chests, pumpkins and melons drop for any hand, and an axe only
  mines them faster. Vanilla wood never requires a correct tool, so survival's
  punch-a-tree-to-craft-an-axe loop works.
- Block placement direction now follows the vanilla rules: logs take the axis of
  the clicked face, and furnaces and chests face back at the player based on the
  look direction alone rather than the player's position relative to the block.
- Clicking a replaceable block such as tall grass builds into that cell instead
  of the cell next to it, and torches fall back through the wall and floor
  variants the way `StandingAndWallBlockItem` does.
- Flowers, saplings and torches are no longer replaceable by placement. Flowing
  water still washes them away, and now leaves their drop behind.
- Pickaxe tiers follow the vanilla requirements: iron ore and lapis ore need a
  stone pickaxe, redstone and emerald ore an iron one, and only the stone family
  vanilla marks requiresCorrectToolForDrops (stone, cobblestone, stone bricks,
  granite/diorite/andesite, coal ore) needs a pickaxe to drop its loot. Sandstone,
  bricks, quartz, netherrack and furnaces drop for a bare hand — the pickaxe just
  mines them faster, exactly like 1.16.1.

- Holding the attack button in creative keeps breaking blocks, and holding the
  use button keeps placing them, on the vanilla 5- and 4-tick delays instead of
  needing one click per block.
- Block-break particles now match 1.16.1. A full cube sheds the vanilla
  4×4×4 = 64 dust pieces (fewer for the torch and cross-plant outline shapes),
  at the vanilla velocities, 0.8-blocks-per-second gravity, per-tick drag and
  4–40-tick lifetimes, with a random texture sub-tile per particle. Each one
  also samples the block light at its own position, so the dust dims in caves
  and lights up next to a torch exactly like the block it came from instead of
  staying full-bright everywhere.
- Difficulty moved into each world save. It lives in the world's level data the
  way 1.16.1 keeps it in `level.dat` and is no longer a global options entry;
  a new world starts on Normal, the difficulty button only appears on the
  in-world options page, and it edits the open world's setting in place.

### Fixed

- A wall torch floated off its wall in two ways. First, placed at an angle —
  onto a replaceable plant or a non-sturdy block — it could attach to a wall
  behind the placement cell and lean back toward the player; the fallback wall
  search now walks toward the wall the player is looking at (1.16.1's
  `getNearestLookingDirections`) instead of a fixed north/south/west/east sweep.
  Second, the model's root was inset 0.38 of a cell from the wall face, leaving
  a two-pixel gap that read as floating; the root now sits flush against the
  wall, matching the 1.16.1 `WallTorchBlock` AABB that runs to the block face
  (the shared `kWallTorchInset` drives both the mesh and the selection box).
- The title panorama's wide-angle distortion was concentrated in one corner
  instead of radiating from the centre. The cube's view ray was normalised per
  vertex before interpolation, which bends the perspective projection so the
  undistorted sweet spot lands off-centre; the raw perspective ray is now
  interpolated and normalised per pixel, so the centre stays straight and the
  distortion grows evenly toward the edges.
- The title panorama rendered upside down: the cube's 180-degree X flip points
  the screen-up ray at the ground face, so the panorama view now reflects
  vertically to stand upright (left and right were already correct).
- The survival health, food and air bars vanished while the player inventory was
  open. They now stay on screen below the inventory panel, and the chat input box
  no longer spans the full width of the screen to darken the hotbar underneath —
  it stretches only as wide as the typed text, the way vanilla does.
- The held item's name above the hotbar always showed, including the literal
  "Air" label for an empty hand. It now follows `GuiIngame#renderSelectedItemName`:
  empty hands show nothing, and a real item's name appears when the selection
  changes and fades out over two seconds.
- The five axe recipes only matched the blade facing left. They were declared as
  padded 3×3 patterns with mirror matching off, so the mirrored layout (blade
  facing right) never matched, and a padded 3×3 even locked the mirror to a
  fixed offset. The recipes are now declared the vanilla 2-wide shape
  `XX / XI / I` with mirror matching on, so the axe crafts with the blade facing
  either way at any grid position, the way vanilla matches every shaped recipe.
- The Options page lost its "Done" button when the Force Unicode Font toggle was
  added: the button list gained a sixth entry but the page still counted five,
  so the last button was neither drawn nor clickable.
- Creatures, dropped items, falling blocks, chests and the third-person player
  drew *after* the translucent terrain pass, which keeps depth writes off, so
  nothing water or glass rendered could hide them: a pig at the bottom of a lake
  or behind a glass wall was painted over the top of it, at full brightness. They
  now render in the entity stage between the cutout and translucent terrain, the
  same place vanilla puts them, so the depth buffer resolves both directions —
  water in front of a creature blends over it, water behind it is rejected.
  Particles keep their post-translucent slot, again matching vanilla.
- Entities ignored the world's lighting entirely: a pig, a dropped item, falling
  sand, a chest or the third-person player all kept the same fixed three-quarter
  key light at midnight, deep in an unlit cave, or standing in torchlight. Each
  one now takes one lightmap sample per frame at the block its body occupies —
  the same per-entity sample vanilla uses — and shades with the terrain's own
  terms: the vanilla light curve, sky light scaled by the day/night cycle, the
  warm block-light tint, the moving light sources and the horizon fog. The fixed
  normal-based face shading stays on top of it, exactly as vanilla layers
  `DiffuseLighting` over the lightmap. Any block entity added later gets the same
  treatment by passing its sample to the shared world-cuboid draw.

- The first-person eating animation followed 1.16.1's
  `HeldItemRenderer#applyEatOrDrinkTransformation` in name only. Its offsets were
  applied *inside* the item's own space — after the resting hand's -45° swing
  rotation, the first-person display transform and its 0.68 scale — so instead of
  rising to the mouth the food drifted off in an arbitrary direction, kept the
  swing tilt vanilla drops while an item is in use, and could only yaw because the
  pose carried a single rotation angle. Eating now runs the vanilla stack: the
  lift `1 - g^27` and the Y 90° / Z 10° / X 30° tilt in camera space, then the
  equip offset, plus the four-tick chewing bob that starts seven ticks in. The
  clip also holds its last frame instead of looping, so the food no longer snapped
  back down to the hand when the render clock ran ahead of the 32-tick meal.
- Eating in creative mode consumed the food: a creative player holding right-click
  spent a stack the way a survival one would. Java 1.16.1 creative players run the
  full meal without gaining hunger or spending the item, so the finished meal now
  skips both the hunger restore and the consumption in creative, while survival
  still eats as before.
- Entity box-UV coordinates are normalized by the geometry's declared
  `texture_width`/`texture_height` instead of the loaded skin's pixel size, the
  way Bedrock does. A skin authored at a higher resolution than the declaration
  used to sample only the top-left corner of itself; it now lands face-for-face.
  The procedural fallback skin is painted in that same declared space.
- Per-cube `rotation`/`pivot` and `inflate` were parsed out of the geometry and
  then dropped when drawing entities and the player, so a rotated cube rendered
  axis-aligned and an overlay layer z-fought the layer underneath. Both are now
  applied, with `inflate` growing the box without moving its box-UV net, and
  `neverRender` bones contribute their transform without drawing.
- `tools/texture_editor` previewed a mapping the game never produced: it composed
  bone rotations in the opposite euler order, clamped UVs where the entity
  sampler repeats, cut transparency at a different alpha and lit the model in
  view space. All four now follow the runtime, so the preview is evidence and
  not an approximation. The editor also stopped writing values nobody asked for
  — defaulted `uv`/`size` fields injected into saved documents, uv rounded to
  one decimal, uv clamped to the declared texture, and a whole-file JSON reformat
  on every save (a one-texel change is now a one-line diff). Saving a net that
  runs off the texture is reported instead of refused.
- Zero-hardness blocks such as grass, flowers, saplings and torches flashed the
  first crack stage before breaking. Mining damage is now evaluated on the tick
  the swing starts, exactly like `Minecraft#continueAttack`, so those blocks pop
  instantly, and the crack overlay only appears past a tenth of the dig.
- The held item only swung once, when a break or place actually completed. It
  now swings for the whole time the attack or use button is down, restarting the
  arc every three ticks the way `LivingEntity#swing` does, so mining a block in
  survival shows the continuous swing instead of a single flick at the end.
- Sprint jumping was roughly 8.7 blocks/second instead of the vanilla 7.1. The
  take-off tick applied air drag; vanilla samples the friction before moving, so
  a jump still pays the ground friction on the tick it leaves the ground.
- Attached blocks that lose their support — a torch whose wall is mined, a
  flower whose dirt is dug — silently vanished. They now break the way
  `World.breakBlock(pos, true)` does in vanilla: the break sound, the
  block-break particles and the loot-table drop all fire, and the drop appears
  in creative as well as survival, matching the mode-independent neighbour-break
  path.
- The F3 block-light readout showed 0 while standing, even next to a torch. The
  debug sample rounded the resting feet — which sit a collision-epsilon below the
  cell boundary — down into the solid block underneath, whose block light is 0 by
  construction; it now reads the block the player stands in, like vanilla's
  floor-of-feet sample.
- Falling sand no longer leaves its neighbours hanging. Emptying a sand cell
  notifies all six neighbours the way `FallingBlock#getStateForNeighborUpdate`
  does, so an unsupported floating patch collapses as a whole and a torch
  standing on the sand pops instead of floating. (Like vanilla, sand generated
  in a chunk stays put until a neighbour change activates it — the fall only
  starts once the world edits near it.)

## ReBedrock beta1

### Added

- Vulkan world rendering, chunk streaming, smooth sky/block lighting and a
  day/night cycle.
- Vanilla-style celestial sky: the sun and an eight-phase moon rendered from the
  original 1.16.1 `sun.png` / `moon_phases.png`, following the sky angle.
- Vanilla underwater fog: submerging fades the view smoothly with exponential
  (EXP2) water fog instead of a fixed-distance wall.
- Survival and creative world modes, inventory/crafting, chests, furnaces,
  commands, chat history and persistent saves.
- macOS and Windows launch binaries with a self-contained `game` runtime
  directory.
- Debug overlay values for version, FPS, coordinates, sky light and block
  light.
- Dedicated video settings, configurable frame cap, anti-aliasing,
  anisotropic filtering and view bobbing.
- Isolated command-line block rendering scenes for visual regression work.
- Unified data-driven animation system: Bedrock-style `*.geo.json` /
  `*.animation.json` files with a Molang expression evaluator drive blocks, the
  player and mobs from one runtime. Ships example player, chest and quadruped
  assets under `resources/animation/`.

### Changed

- Incremental asynchronous lighting and chunk remeshing substantially reduce
  block-edit latency.
- The first-person held-item swing, inventory player preview, chest lid and
  dropped-item float/spin now run on the unified data-driven animation library
  (authored JSON + Molang, with Bezier keyframe easing) instead of hardcoded
  curves, with no measurable frame-time impact. The animation code is now a
  standalone `mc_rebedrock_animation` library independent of gameplay and
  rendering.
- F5 cycles the camera between first person, third person behind and third person
  in front. The third-person player is rendered as a multi-bone skinned model
  driven by the same animation library, using a new world-space cuboid shader
  path. The third-person camera pulls in to avoid clipping through walls, shares
  the first-person view bobbing, and the player's head leads the turn — dragging
  the body only once it passes its rotation limit.
- The third-person player crouches when sneaking (Shift), and animation states
  (walk, sneak) now ease in and out over a few frames instead of snapping on and
  off.
- The block selection outline traces each block's real shape: torches, plants and
  chests show a fitted marker instead of a full-block cube.
- First-person hands and held items use world-space 3D rendering.
- Directional blocks retain their placement orientation in save files.
- Menu and loading backgrounds use a consistent Minecraft-style dirt tile.
- Water seen from above now varies transparency strongly with depth: shallow
  water is clear to the seabed and deep water turns opaque murky blue.

### Fixed

- The chest lid no longer changes size while it opens: the item shader scaled
  non-uniform cuboids after rotating them, shearing the box; it now scales before
  rotating.
- The chest lid now opens along the chest's own facing direction. Its rotation
  was applied about a fixed world axis, so the hinge looked correct only when the
  chest happened to face the default orientation; each part is now transformed by
  a full world matrix that binds the rotation to the chest's local frame.
- The chest lid now pivots rigidly on its hinge edge instead of sliding across
  the opening: the lid box's rotation and its position along the hinge arc used
  opposite signs, so the hinge edge slid a full face; it is now one rigid
  rotation about the hinge line.
- The chest's top and bottom faces (parallel to the XZ plane) were swapped, so
  the lid showed its rimmed underside on top; the +Y/-Y faces are now seated the
  right way up.
- Player skin UV alignment, chest persistence and inventory drops, torch
  geometry, fluid scheduling, item interpolation and glass break audio.
- Horizontal log model orientation, furnace inventory previews, and chest
  entity/preview face textures and latch alignment.
