# ReBedrock Changelog

[English](CHANGELOG_EN.md) | [简体中文](CHANGELOG_CH.md)

All notable player-facing changes are recorded here. The project follows a
simple versioned history while it is in beta.

## ReBedrock 26.1beta1

### Fixed

- Block textures are laid out from the vanilla model files face by face. Top and
  bottom faces used to be a quarter turn off, and shaped blocks (slabs, stairs,
  walls, doors, trapdoors, buttons, pressure plates, fence gates) had their top
  face flipped and their +X and -Z sides mirrored. Directional sprites — the
  crafting table top, hay bales, pumpkins, log end grain, the brick courses on a
  brick slab — now match vanilla.
- Blocks that turn with their facing (observer, piston, furnace) now carry their
  textures rigidly as they turn. The old rotation was calibrated so that facing
  up looked right, but vanilla's reference orientation is facing north, so every
  other facing was a quarter turn out.
- The observer's top texture is no longer upside down (its model declares that
  face with an inverted uv rect).
- The anvil's base, waist, neck and striking face: vanilla's model rotates 13 of
  its 21 faces and all 13 were being dropped, so the metal ran the wrong way on
  the sides and both caps.
- The comparator's front redstone torch is no longer stretched vertically (it is
  shorter than the two behind it and was using the taller torches' sprite range).
- The narrow pair of faces on a lever's base is no longer stretched sideways.
- Repeaters, comparators, levers, enchanting tables and anvils are baked once at
  startup instead of once per block per chunk rebuild, which removes an invisible
  hitch when placing or breaking blocks near them (about 25% off the rebuild in
  an equivalent test).
- Anvil renames now survive a save. Renaming worked for the rest of the session and then
  reverted on the next load — the bytes on disk were always correct; the name table was
  being cleared *after* the save had been parsed, which wiped the names it had just read.
- Anvils, enchanting tables and slabs no longer turn the grass block under them to dirt.
  Whether a block smothers grass now follows its actual shape (does it fill the cell —
  vanilla's test) rather than its render category. An anvil is a small base, a slab is
  half a cell, and vanilla lets grass live under both. A double slab still kills it.
- Whether a face between two blocks gets drawn is now decided by the neighbour's
  actual shape instead of guessed from its render category. Four artefacts go
  away together: a torch (or stairs, wall, door, grass) beside water no longer
  opens a see-through hole at the boundary, an anvil no longer sits over a ring
  of holes, the seam beside an enchanting table is closed, and the hidden faces
  under a double slab or farmland are no longer drawn for nothing.
- A dirt path is 15/16 tall again, as in vanilla: walking onto one and aiming at
  one no longer treat it as a full block.
- The anvil's rename box now shows the item's current name (it used to be blank
  for anything that had never been renamed), and taking the result without
  touching the box no longer charges a level for a rename that did not happen.
- Fixed text sitting low and to the right in borderless input boxes. Vanilla's
  borderless fields neither inset nor centre their text — their frame comes from
  the screen's own art. The anvil's rename box and the chat line are both of
  that kind, so both are fixed.

- Fixed the anvil screen's name box showing as a red bar. The red is a
  placeholder in the background texture that the screen is supposed to cover
  with the text-field sprite, and the screen was not drawing it. The field shows
  greyed out: renaming is not implemented yet.
- Past 40 levels the anvil now says "Too Expensive!" the way vanilla does,
  instead of going on showing a price that can never be paid. (Creative is not
  subject to the wall, as in vanilla.) The red cross over the output slot now
  also appears when two items simply cannot be combined.

- Item tooltips were not drawn at all: the previous batch factored the box
  drawing into a stub that did nothing but call itself, so hovering an item on
  the inventory, creative and container screens showed nothing. The box is now
  actually drawn.
- Tooltips no longer run off the screen: hovering near the right or bottom edge
  flips the box to the other side of the cursor, and long lines now wrap.
- Item tooltips now list enchantments. Until now a tooltip was just the item's
  name, so there was no way to tell an enchanted tool from a plain one — or to
  see whether an enchant had worked at all. Container screens (chest, furnace,
  crafting table, enchanting table) also had no item tooltip whatsoever, so a
  tool hovered inside one did not even show its name. A held item that gains an
  enchantment re-shows its name above the hotbar, too.
- Fixed the enchanting table's option tooltips, of which only the last was ever
  visible: they were drawn inside the per-option loop, so the first two were
  painted over by the options drawn after them.
- Fixed hotbar items showing no tooltip in creative mode once any content tab was
  open (the inventory tab was always fine): that row of slots only computed its
  highlight and never handed the hovered item to the tooltip.
- Tooltips now use vanilla's two backdrop sprites (the translucent dark fill and
  its gradient border) instead of a flat coloured rectangle, with vanilla's line
  spacing, padding and above-right placement. Both sprites are read from the
  resource pack, so a pack can restyle them.
- **The whole picture is now composited in vanilla's colour space.** Colours across
  the world and the interface were systematically too bright and too washed out:
  · Dyed sheep and other dyed things return to vanilla's colours. The darker the dye
    the worse it was — a black sheep rendered as #5F5F65 instead of #1D1D21, a grey
    one as #90979A instead of #474F52 (up to 73/255 off per channel); white was
    almost exact, which is why it only showed once something was dyed.
  · Per-face block shading returns to vanilla: bottom faces 0x80 (was 0xBC), east/west
    0xAD (was 0xD7), so blocks read as solid again instead of flat. Biome tint, fog
    and the warm cast of block light likewise.
  · Every non-white piece of interface text returns to vanilla's value (a container
    title's dark grey 0x404040 showed as a light 0x89, an enchantment line's 0xAA as
    0xD5), and translucent dark overlays no longer read as more transparent.
- The item in your hand is no longer clipped by a block right in front of your face
  (the interface pass has its own depth buffer, as in vanilla).

### Added

- The enchanting table now sends Standard Galactic Alphabet glyphs flying in from nearby
  bookshelves — more shelves, more glyphs — converging on the book on top and brightening
  as they arrive. Vanilla has always done this; this build did not. The glyph textures are
  read from the resource pack, so a texture pack can replace them.
- The anvil can rename things now. Put an item in the left slot, type in the box,
  and it costs one level; clearing the box strips the name for the same one. A
  renamed item shows its new name in tooltips and above the hotbar, and no longer
  stacks with unnamed ones. A pure rename does NOT raise what the item costs to
  work again, and is not subject to the Too Expensive wall — gear repaired to
  death can still be named. Names are up to 50 characters and take non-ASCII.

- Text boxes are now real text boxes. Until now the two places you could type —
  the world name on the create/edit world screen, and the chat and command line —
  each had their own half-finished handler that could only append a character or
  backspace one off the end, and neither would accept anything outside plain
  ASCII, so an accented letter or a Chinese character simply did not arrive even
  though the font could draw it. Both now share one widget with a real cursor:
  arrow keys, Home and End, Shift+arrows and Ctrl+A to select, Ctrl+C/V/X for the
  clipboard, Delete, Ctrl+arrow to jump a word and Ctrl+Backspace to delete one,
  and horizontal scrolling so a long name no longer runs out of its box. A
  backspace removes a whole character rather than one byte of it, so multi-byte
  text edits correctly. The anvil's rename box is wired to the same widget but
  stays greyed out — renaming still needs somewhere to store a custom name on the
  item.

- The anvil, in its three wear states, and the block of iron it is crafted from.
  Place it, right-click, and you get two input slots and an output: repair a tool
  with its own material (a quarter of its durability per ingot, charging only the
  ingots it actually uses), combine two of the same item (durability adds plus a
  12% bonus, and their enchantments merge — equal levels step up by one, unequal
  ones take the higher, and a conflicting one is refused but still charged for),
  or spend an enchanted book on a piece of gear. Every trip through the anvil
  makes the next one more expensive, until at 40 levels the item is Too
  Expensive and can never be worked again. Renaming is **not** in yet: it needs
  somewhere to keep a custom name on the item, which is a storage decision of its
  own.
- Mending. A damaged item carrying Mending, worn or held, takes experience orbs
  before your level bar does — two durability per point — and only what is left
  over becomes experience.

- Item tooltips now say what an item actually is:
  - the **name is coloured by rarity**, and an enchanted item moves up one step
    (a plain tool's name turns aqua once enchanted);
  - **attribute lines** — attack damage and attack speed in the main hand for
    weapons and tools, armor and armor toughness for armor;
  - **curses are listed in red** (Binding, Vanishing), so they stand apart from
    ordinary enchantments at a glance;
  - with the F3 debug screen open, two more lines: **durability** (only once the
    item is actually damaged, remaining / maximum) and the item id.
  The " xN" that used to follow the name is gone — the count is already drawn in
  the corner of the slot itself. The held-item name popup picks up the rarity
  colour too.

- The enchanting table. Craft it (four obsidian, two diamonds and a book),
  place it, and right-click to open a screen with an item slot, a lapis lazuli
  slot and three offers. Each offer shows its level requirement, a
  Standard-Galactic phrase and — on hover — one of the enchantments it would
  apply plus what it costs. Buying an offer takes `slot + 1` levels and
  `slot + 1` lapis (the number on the bar is the level you must HAVE, not what
  you pay), applies every enchantment that offer rolled, and rerolls your
  enchantment seed so the next three offers differ. Bookshelves raise what the
  table can offer: the two 5x5 rings at the table's level and one above it
  count, and only when the cell between the shelf and the table is clear, so a
  shelf behind a wall is worth nothing. Whatever is left in the two slots comes
  back when the screen closes.
- Enchanted books. Put a book in the enchanting table and what comes out is an
  enchanted *book* — a different item, which no longer stacks with plain books
  or reads as a crafting ingredient. Creative's Ingredients tab carries one per
  enchantment, each at that enchantment's maximum level. (Nothing consumes an
  enchanted book yet; spending one on gear waits for the anvil.)
- Eleven game rules that gate mechanics this build already had but never exposed:
  `fall_damage`, `fire_damage`, `drowning_damage` and `natural_health_regeneration`
  (the last gates both food-driven and peaceful healing, but never starvation),
  `block_drops`, `mob_drops` (loot and experience alike), `spawn_mobs`,
  `fire_spread_radius_around_player` (26.1's replacement for the retired
  `doFireTick`: `-1` anywhere, `0` nowhere, otherwise a block radius around the
  player), and the three command budgets `max_block_modifications`,
  `max_command_forks` and `max_command_sequence_length`, whose values were
  previously hardcoded at exactly these defaults.
- `/time` is now 26.1's full command: `set` (an absolute tick, or a
  `day`/`noon`/`night`/`midnight` marker), `add`, `pause`, `resume`, `rate` and
  `query time|gametime`, each of which can be aimed at one named clock with
  `/time of <clock> …`. Pausing a clock freezes only that clock — mining,
  cooldowns and every other timer keep running. The time argument accepts the
  `d`/`s`/`t` unit suffixes (`/time add 2d`).
- `/effect give <targets> <effect> [<seconds>] [<amplifier>]` and
  `/effect clear [<targets>] [<effect>]`, working on players and creatures alike.
- `/enchant <targets> <enchantment> [<level>]`, which enchants the held item and
  refuses a level past the enchantment's maximum or an item it cannot go on.
- `/setworldspawn [<pos>]`, the world-wide counterpart to `/spawnpoint`.
- Sheep now come in all 16 dyeable colours: the wool renders in the sheep's own
  colour, right-clicking with a matching dye recolours it, and shearing exposes the
  bare body as the wool layer disappears; the colour persists with the sheep.
- A full experience system: levels, progress and total with the 26.1 curve, a HUD
  experience bar and level number, experience-orb entities (gravity, same-value
  merging, magnetic attraction and pickup), and four sources — mob kills (gated by
  recent player damage), ore mining, smelting and breeding — plus a death drop of
  `min(7*level, 100)` (retained when `keep_inventory` is on). The `/experience`
  (`/xp`) command was added.
- Nether and End world generation: nether terrain (multi-noise biomes, surface and
  features) and end terrain (a central island plus outer islands), each dimension
  with its own world height, independent per-dimension ticking (an unloaded
  dimension costs nothing) and per-dimension region storage.
- More shaped blocks: stairs, doors, fence gates, trapdoors, buttons, pressure
  plates, walls and fences — polymorphic shapes, block states, placement and
  interaction, all through the single `BlockShape` collision/raycast/culling path.
- Slabs and stairs can be waterlogged (the `SubmergedFluid` state axis), activating
  the first property-deviation override in the Java interop mapping.
- Audio gained 11 sound categories with individual volumes, a Directional Audio
  toggle, and biome ambient loops, cave mood, situational music and jukebox playback.
- Commands gained an `/execute` subcommand tree (as/at/positioned/if/unless/run,
  with `run` redirecting back to the root), `/help` introspection with smart usage,
  and value-type contextual Tab completion; an Allow Cheats world flag and its
  create-world toggle were added.
- Game version identity metadata (a single `26.1` version source, a `version.json`
  export, a self-describing save version header and world summaries) was added.
- Natural spawning now populates animals at world-generation time.
- A species manifest: new creatures are now a data row instead of a C++ class
  plus a registration line. Three creatures ride it — chicken and sheep (passive)
  and husk (a melee hostile) — resolving by name, carrying the right category and
  attributes, spawning, ticking, dropping loot (chicken feathers) and obeying the
  peaceful-removal rule by category. Like the zombie, their render assets are not
  shipped yet, so they do not appear in-game. The deterministic block loot (stone to
  cobblestone, an ore to its item, glass to nothing, and the rest) moved out of
  the mining system's hand-written handlers into a baked loot floor plus a
  datapack overlay (`data/<namespace>/loot_tables/blocks/*.json`); a build with no
  `data/` still drops on the built-in floor, and a block with no table drops
  itself. Drop results are identical. The loot format is deliberately a direct
  block-to-drops table with no chance, Fortune or Silk-Touch machinery — this
  build has no system to make those vary — and the few genuinely random blocks
  (leaves, gravel, crops) keep their procedural handler.
- Crafting and smelting recipes now come from data instead of a static vector in
  the crafting system. The built-in recipes are a baked constexpr floor resolved
  at load, and a datapack can add or replace recipes through the data codec
  (`data/<namespace>/recipes/*.json`, with `type: "smelting"` selecting the
  furnace shape); a build with no `data/` still crafts on the built-in floor. The
  matcher is unchanged and recipe results are identical — the recipe list simply
  moved out of the evaluator into the data layer, and an overlay recipe naming an
  item this build lacks is skipped rather than resolved wrong.
- Block tags now read their datapack file format through the data codec: a
  single `TagFile` codec (`{replace, values}`, values as bare strings or
  `{id, required}` objects) replaces the hand-rolled JSON walking BlockTags did
  inline. The tag policy is unchanged — `#tag` references still expand, packs
  still merge low-to-high with `replace` truncating, unknown blocks are still
  skipped — and membership still lives in the per-id bitset, so tool rules behave
  exactly as before. Item and entity tags will reuse the same codec.
- A Java-Edition data-side interop layer: a feasibility mapping that ingests
  vanilla `minecraft:` datapack JSON (recipes, loot, tags) into the project's own
  definitions. Tags ingest directly (the vanilla tag file already is the
  project's tag format), crafting/smelting recipes through a thin adapter, and
  the deterministic subset of block loot (a plain drop, or the non-silk branch of
  a silk/non-silk pair); random or conditional loot is reported as needing
  conversion. A completeness check confirms every block and item maps to a
  `minecraft:` name. This is the data half of world interop — the save/world half
  (block-state palette, scheduled ticks, NBT) is separate — and it is a tested
  capability, not yet wired into loading; see the interop report in the wiki.
- A data codec and two-layer baking foundation (`src/data/`) for moving recipes,
  loot and tags out of hand-written C++ into files. A serde-style `Codec<T>`
  encodes a POD to and from JSON with value semantics (no functional pipeline);
  a `DataStore<Def>` lays a baked constexpr built-in floor (in `.rodata`, zero
  startup parse) and merges a datapack overlay on top by name, while a build with
  no `data/` at all still runs on the floor alone. An offline baking generator
  (`tools/bake`) turns source JSON into the constexpr floor through the same
  codec, and `core::Json` gained a `dump()` writer plus a parse counter. This is
  infrastructure the recipe/loot/tag work builds on; no player-facing content
  moved yet.
- A renderer-independent authoritative `GameRuntime` now owns the world, save
  repository, game session, simulation thread, world lock and server command
  tree. World creation/loading/saving/unloading and the 20 TPS tick no longer
  live in the Vulkan client; headless tests can run the complete game chain and
  measure resident server chunk-state/light memory, establishing a concrete
  boundary for the future client/server process split.
- Player input gained a thread-safe semantic command queue. Breaking, attacks,
  block/item use, hotbar changes, slot clicks and chat enter as ordered
  `GameCommand`s and are applied by `PlayerInteraction` during the authoritative
  tick. `PlayerActionState` supplies the tick-owned swing and held-use timeline,
  leaving the renderer to consume its snapshot.
- Player controllers, inventories, vitals, crafting, game modes, input and
  action timelines now live in authoritative `ServerPlayer` records indexed by
  stable `PlayerId`s. The local player uses the same container, establishing a
  concrete data boundary for adding more connected clients later. Player-scoped
  damage, death, respawn, eating, tool durability and death drops now execute by
  `PlayerId`, with two-player isolation coverage ensuring one player's changes
  do not affect another.
- Natural spawning now uses 26.1-style weighted tables split by biome and mob
  category, with built-in vanilla weights and group sizes for pigs, cows and
  zombies. Packs may override an individual biome through
  `data/minecraft/worldgen/biome/<biome>.json`; absent or malformed data safely
  retains the built-in defaults.
- Entity types gained `ON_GROUND`, `IN_WATER`, `IN_LAVA` and
  `NO_RESTRICTIONS` spawn placements. The spawner samples within a column's
  actual height range and validates its support, two-block body space, fluid,
  full entity bounds and three-dimensional player exclusion distance, giving
  caves and future aquatic/lava species a correct placement path.
- The world is 384 blocks tall (y −64..319, 24 sections), matching the modern
  height standard: the world access, lighting, meshing, streaming, save and
  gameplay layers all carry a `kMinY` offset, while the terrain generator keeps
  its 0..255 noise axis and fills the deep stone below.
- Chunk ownership split the runtime's authoritative server world from the
  renderer's client chunk cache: the renderer meshes, lights and samples its own
  world copy, and every streamed batch and simulation edit lands on both, so
  each side can eventually drop what the other needs. The two sides measure
  their chunk memory separately (a resident-bytes budget, ~54 MB each in the
  smoke world).
- Saves are region-based. Chunk edits and creatures no longer live in one
  `world.dat`: each 32×32 chunk region gets its own `region/r.<rx>.<rz>.cache`
  file, and a chunk owns its edits and its herd together. A chunk leaving the
  simulation radius submits its edits and creatures to a background persistence
  queue; writes for the same region are coalesced, and world switches or exit
  wait for them to reach disk. Creatures leave simulation with the chunk and
  return when it streams back in, so nothing a player placed or a herd beyond
  the loaded area is lost to a crash or a save. Old worlds migrate to the
  region layout on their next save.
- `GameCommand`s, gameplay events, and player/world snapshots now share a
  length-framed binary codec. Item stacks, block states and entity types travel
  by registry identity; unknown messages can be skipped by length, while
  truncated or unknown content is rejected safely. Commands, events and the
  server-to-client mirror therefore have a protocol boundary ready for a
  loopback or TCP transport.
- A transport layer was introduced (`src/net/`): a single message stream carries
  commands, snapshots and events over a `MessageChannel`, framed by the shared
  binary codec and routed by tag, plus an in-process loopback pair of channels
  modelled on the vanilla local connection. A headless test drives the full
  command → tick → snapshot round trip through the byte channel against a real
  game session — the groundwork for single-player to run the same
  client/server message path a networked client will.
- Oak, spruce, birch, jungle, acacia, dark-oak, stone, cobblestone,
  stone-brick and smooth-stone slabs were added. Slabs carry bottom/top/double
  block states and half-height meshes, collision and raycast outlines; they
  place from the clicked face, merge with their own kind, and fully support
  saves, doubled drops, the creative catalogue and half-height HUD icons.
- A POSIX TCP `MessageChannel` and listener were added. A dedicated I/O thread
  reassembles fragmented/coalesced TCP data into ordered frames, applies
  backpressure with an 8 MB bounded send queue, and handles disconnects and
  invalid lengths safely. The same command → tick → snapshot chain can therefore
  switch from in-process loopback to a real socket without changing its protocol.
- New connections now perform a protocol handshake before gameplay messages:
  client and server exchange an application identifier and protocol version,
  entering play only on a match. Wrong applications, malformed hellos, version
  mismatches and silent peers produce explicit refusal or timeout outcomes.
- An experimental `mc_rebedrock_dedicated_server` was added without GLFW or
  Vulkan linkage. It creates or loads a world, prepares spawn chunks, runs and
  saves headlessly at 20 TPS, and can accept one handshaken client on a local TCP
  port. This is currently a single-connection skeleton; a graphical remote-connect
  entry point and multi-player ownership remain future work.
- Optional slow-frame and resident-memory diagnostics were added:
  `MC_REBEDROCK_FRAME_TRACE=1` reports overruns by persistence, locking, GPU
  fence, event-drain and other stages, while `MC_REBEDROCK_MEMORY_REPORT=1`
  accounts for server/client chunks, worker chunks, textures, GPU buffers and
  the CPU mesh pool to expose timing and memory regressions in the dual-world
  architecture.
- A runtime content-identity foundation was stood up in `src/core`: strongly
  typed dense `uint16` ids (`BlockId`/`ItemId`/`EntityTypeId`/`BlockEntityTypeId`),
  an `Identifier` interner, and a generic `Registry<Def, Id>` with a
  `Bootstrap → External → Freeze` lifecycle that keeps built-in ids stable,
  admits external (mod/datapack) content afterward and aborts on wrong-phase,
  duplicate-name or invalid-id access. The block table is poured into a frozen
  registry alongside the existing `Block` enum with no behavior change yet,
  laying the groundwork for retiring the 256-entry enum and its switches.
- The block-identity tables now index by `BlockId` and size to the block
  registry rather than the 256-entry `Block` enum, so content can grow past the
  old ceiling: the interned state metadata carries each state's block as a
  `BlockId`, the block-tag table and the save block palette size to
  `blockCount()` (the palette dropped its compile-time 256 cap entirely), and
  the creative catalog is now a view whose identity comes from the registry. The
  `Block` enum stays as a transitional handle over the same ids, so there is no
  behavior or save-format change. Retiring the remaining `switch(block)` chains
  is a later step.
- Block-tag membership now uses a `TagBitset` that spans as many 64-bit words as
  needed, allowing the tag vocabulary to grow past 64 while the hot lookup stays
  one indexed load and bit-and. After built-ins register, the block registry now
  enters an `External` phase before freezing, so mod/datapack blocks can receive
  following ids without moving any stable built-in id.

### Changed

- Removed the "Particle Rain" rain mode; rain is now either "Texture Rain" or
  "Asynchronous Particle Rain". The removed mode consumed the same raindrops and
  produced the same visuals as the asynchronous one — it only issued one draw call
  per drop, a path kept purely to benchmark against the instanced one, and at the
  highest particle level in heavy rain that meant 18,000 draw calls per frame.
  Saves that selected it migrate to Asynchronous Particle Rain.

- Game rules are now named the way 26.1 names them, in `snake_case`:
  `doDaylightCycle` became `advance_time`, `doWeatherCycle` became
  `advance_weather`, and `keepInventory`, `randomTickSpeed` and
  `sendCommandFeedback` became `keep_inventory`, `random_tick_speed` and
  `send_command_feedback`. Worlds saved by an earlier build keep their settings:
  the old names are still understood when a save is read, so nothing silently
  reverts to a default. Only the new names are accepted by `/gamerule` and
  offered in completion.
- `/time set <n>` now sets the absolute time the way 26.1 does, instead of
  folding the value into the current day. `/time set day` (and the other markers)
  still moves forward to that point's next occurrence, so it never winds the
  calendar backwards.
- Player animation moved onto a layered bone-mask + override-blend +
  animation-controller stack, with the world player and first-person view sharing
  it, and its clips were calibrated to the 26.1 specification.
- Save folders are named by the world-name slug instead of appending a
  creation-time string; a numeric suffix is added only on a name collision.
- Entity types now share the same content registry as blocks and items, so a
  creature this build no longer knows — one a removed datapack or mod placed —
  is kept as a placeholder instead of vanishing from a saved world; it round-trips
  by name and comes back as the real creature once the content is re-added. Names
  resolve through the `minecraft:` alias beside the `rebedrock:` id, and a name
  nobody registered is a clean miss (so a command cannot summon a placeholder).
- Entity attributes now come from data. Each creature's health, movement speed,
  attack damage, follow range and knockback resistance are a fixed array with a
  compiled-in floor, and a datapack can override any subset per creature through
  `data/<namespace>/entity_attributes/<species>.json`; unlisted attributes fall
  back to the floor, and a build with no `data/` keeps every creature's built-in
  numbers. Because the AI reads follow range and attack damage through the same
  path, shrinking a creature's follow range in a datapack changes its acquisition
  radius with no code change.
- The Vulkan client now composes `GameRuntime` and delegates world lifecycle,
  persistence, simulation and authoritative commands to it, removing the
  corresponding client-owned implementations. The client retains window/input
  sampling, menus, audio and GPU presentation responsibilities.
- First-person break, place and eat animations no longer keep a render-frame
  clock. They sample gameplay's tick-owned action progress as a pure input, so
  the same operation consumes the same ticks at every frame rate and its visible
  motion stays aligned with the interaction decision.
- Player rendering now consumes an atomically published `PlayerTickSnapshot`
  each gameplay tick and uses the current frame's partial tick to interpolate
  position, stride, speed, field of view, swings and item use. The HUD,
  third-person model and mining overlay obtain vitals, mode, held item, water
  and grounded state from that same snapshot instead of reading player objects
  during simulation. Consecutive swings carry action sequences so a restart
  snaps to its own beginning instead of visibly replaying backwards.
- An atomically published `WorldSnapshot` now supplies the client with time,
  named clocks, weather gradients, relevant game rules and chest-lid state each
  tick. Sky, shadow, precipitation and chest rendering no longer read those
  live gameplay systems directly.
- Player, world and entity render snapshots now expose resident-memory
  accounting guarded by budget regression tests. Dynamic buffers retain and
  reuse their capacity across ticks, preventing the dual-world architecture
  from reintroducing unbounded client memory through per-tick allocation.
- Container and creative interactions are now fully command-driven, and
  container screens render from a per-tick snapshot: inventory, chest, crafting,
  furnace and creative-catalogue slot clicks, shift quick-move, drag
  distribution (QUICK_CRAFT) and double-click gather (PICKUP_ALL) all run
  through the authoritative `GameCommand` queue on the server tick, so the
  renderer no longer mutates live inventory/container state directly. Drag
  targets travel as (slot kind, index) values and the drag preview reads the
  published container display snapshot. The same operation consumes the same
  ticks at any frame rate, laying the path for the client/server process split.
- Natural spawning now spreads simulation-radius-scaled column samples across
  categories every tick and chooses species with integer weights, replacing a
  burst of three surface scans once per second. Block light continues to
  prevent hostile spawning after dark instead of being incorrectly dimmed with
  the sky.
- The render thread no longer holds the shared world lock across the frame.
  Player, world and entity state is atomically published as one immutable
  snapshot bundle; readers pin that bundle while copying and the writer only
  reuses storage with no remaining readers, preventing a slow frame from being
  overwritten as it could be with a fixed double buffer. The renderer reads its
  own client chunk cache and published snapshots exclusively, and the
  GPU fence wait, submit and present hold no lock — so the simulation never
  waits on the GPU. The world lock now covers one tick plus the short
  installation/edit write sections. This removes the intermittent frame stutter
  that appeared when moving: the sim thread is no longer blocked by a frame-wide
  read section spanning vertical sync.
- Chunk generation, initial lighting and mesh building now reuse one bounded
  persistent worker pool in sequence instead of creating threads for every
  stage of every 24-chunk batch. Streaming batches carry shared read-only chunk
  payloads: the worker world, simulation world and client cache share static
  terrain storage, and only a side that edits a chunk performs a whole-chunk
  copy-on-write. This reduces CPU spikes and memory growth while moving. Two
  extra chunk rings beyond the load radius provide unload hysteresis, avoiding
  repeated unloads, reloads and region writes near a boundary.
- Transient Vulkan attachments such as MSAA color and depth now prefer lazy
  allocation. On Apple GPUs with memoryless attachments, targets used only in
  tile memory no longer reserve roughly 281 MB of resident GPU memory.
- Simulation-produced audio, particles, container opens and eating presentation
  now cross a thread-safe event queue. Container hit testing and drag previews
  build geometry-only slots with no storage pointers, so the render thread no
  longer reaches into authoritative inventories or block entities.
- Single-player now runs the in-process loopback message path in both
  directions: every semantic command is encoded before the server tick consumes
  it, while gameplay events and player, world and entity snapshots return into a
  `ClientMirror`. The HUD and world renderer no longer read the authoritative
  `GameSession` directly. World switches clear both channels and the mirror,
  mining/container state travels in snapshots, and a per-tick encoded-size
  regression check constrains the cost of the full mirror.
- Continuous movement, look direction, jump/sprint edges and auto-jump now travel
  through the channel as `MovementInput`, while respawn and game-mode changes use
  separate `SessionCommand`s. The client no longer writes or directly calls the
  authoritative `GameSession`; the server also derives flight and sprint
  permission from game mode and hunger instead of trusting client results.
- Base block geometry now comes from one `BlockShape` source, rather than
  separate special cases in ray hits, selection outlines, player/creature
  collision and placement occupancy. Full blocks and slabs use continuous-height
  columns; torches, plants and chests use exact AABB sets. Chest collision now
  matches its 14×14×14-pixel model, and the same path is ready for multi-box
  stairs, fences and doors.
- Block-state wire encoding now iterates every property declared by the schema
  and sends property-index/value pairs instead of a six-property handwritten
  bitmask; unknown newer properties remain skippable. Because this changed the
  wire layout incompatibly, that stage raised the handshake protocol to 3.
- Block identity on the wire now uses a two-byte dense `BlockId` instead of an
  identifier string of roughly 20 bytes. The handshake sends the server block
  registry as a name list in id order, and the client builds a name-aligned
  peer-id→local-id remap, so external content registered in different orders is
  never misidentified. Single-player loopback shares one registry and keeps the
  identity fast path. This subsequent protocol change raises the current
  handshake version to 4.
- Held and dropped slabs now use a half-height block model with the correct
  half-strip side UVs instead of appearing as a full cube or upright flat item.
  Slab placement and merging on a horizontal face use the ray's precise hit
  height to choose top or bottom.
- The README was rewritten for the current beta implementation, including the
  Java 26.1 resource-pack requirement, dual-platform build/test workflow,
  runtime layout, project structure and known boundaries. Obsolete 1.16.1
  extraction and bundled-resource instructions were removed.
- Save format 18 migration is now verified against real played worlds saved in
  formats 10, 14 and 15: each opens, converts to the current format on its next
  save, and reloads with the player's position, inventory, block edits, weather,
  clocks and entities intact. A `migration_diag` helper repeats that load →
  save → reload round trip on a copy of any world directory, so the conversion
  can be re-checked on any real save.

### Fixed

- Fixed **dropped items** of six-way directional blocks (the observer and friends)
  rendering as a flat 2.5D sprite: the same block was a cube in the hand and in the
  inventory, but a sprite on the ground. "Does this item draw as a cube" was spelled
  out separately in each of the three item render paths (drop, first-person held,
  inventory icon) and the three spellings disagreed — adding six-way directional
  blocks only updated two of them. All three now share one answer, so a new block
  model has exactly one place to declare itself.

- `advance_time` no longer overrides a clock paused by `/time pause`. The rule is
  a global switch over every clock, kept separate from each clock's own paused
  state, the way 26.1 keeps them.
- Fixed the box-UV texture mapping of the animal models so cows, pigs, sheep and
  chickens match Java 26.1 face-for-face: corrected the left/right mirroring on cube
  front/back faces, the swapped inner/outer faces on mirrored limbs, and the
  front/back scrambling and misplacement of rotated torsos (cow/pig/sheep/chicken
  bodies, the cow udder, the sheep fleece) caused by a wrong bone-rotation sign.
  Sheep now render as proper white wool: the wool parts sample a dedicated fleece
  texture layer instead of an inflated copy of the bare body, and the head no
  longer smears the face across several sides.
- Sound distance attenuation was fixed: the default inverse model previously
  stopped at about 0.1 volume past the maximum distance, so mobs at any depth were
  clearly audible at the surface; it now falls off linearly to silence at the
  maximum distance and culls out-of-range sources, alongside fixes to several
  runtime audio defects.
- Fixed chunk-streaming issues: centre-chunk starvation when turning, out-of-order
  section delivery and first-mesh metric distortion.
- Fixed an animated-block atlas squash (magma) that misaligned its texture.
- Command suggestions no longer appear when the input does not begin with `/`.
- A block placed by content this build no longer has — a datapack or mod that
  was removed — is now preserved in the save as-is instead of being replaced by
  air. Its identifier and state are kept verbatim, so re-adding the content
  brings the block back exactly where it was, matching how Java keeps unknown
  blocks rather than dropping them.
- Sapling-grown logs are now written and published atomically as complete block
  states, so trunks no longer render their end grain sideways after an
  orientation-only follow-up update was lost. Cross-chunk tree blocks retain
  their complete state as well.
- Shift-click once again quick-moves full stacks between the hotbar and main
  inventory. In a creative item-category tab, shift-clicking a hotbar stack
  returns it to the catalogue by clearing that slot.
- Hostile mobs are no longer restricted to nighttime surface spawns while dark
  caves remain empty, and torch-lit caves stay spawn-proof. Pigs and cows also
  no longer appear with equal odds in deserts, oceans or other biomes outside
  their 26.1 spawn tables.
- Sky light filtered through leaves no longer makes grass below revert to dirt
  at night. Grass survival now depends only on physical shielding above it,
  while spreading to nearby dirt remains gated by the current brightness.
- Exiting the game entirely and re-entering a world no longer teleports the
  player to the world origin (0,0,0). The load path used to overwrite the
  correctly restored save position with a not-yet-initialized render snapshot
  (hot reloads looked fine only because of leftover state). After a world load,
  before the first simulation tick, the live position and the render snapshot
  now both match the saved coordinates, and switching worlds no longer inherits
  the previous world's position.
- The player, swung hand, item drops, falling blocks and creatures no longer
  occasionally jitter at tick boundaries. Interpolation is now timed from the
  client's receipt of a mirrored snapshot and paired with the endpoints from
  that same mirror update, so it cannot drift one tick out of phase.
- When render frames outnumber server ticks, a later no-press frame no longer
  overwrites an unconsumed jump or forward-double-tap edge. The server merges
  one-shot edges across `MovementInput`s, so double-tap sprint no longer fails
  intermittently.
- Slab ray hits and selection outlines no longer disagree: aiming through the
  empty half does not hit an invisible full-cell wall, while aiming at the slab
  returns the correct distance and face. Torches, plants, crops and chests now
  use their real outlines too.
- Top and double slabs no longer revert to the default bottom state when a world
  edit event reaches the client. The transport codec omitted `SlabType`, leaving
  the authoritative world correct while the client mesh was wrong.
- Sprinting onto a slab or another steppable height no longer causes a sudden
  stop or cancels sprint. A successful step-up preserves horizontal velocity and
  clears the collision it fully overcame; a true one-block wall still stops it.
- Standing on a pressure plate no longer bounces the player up repeatedly. The
  plate now has an empty collision shape (matching 26.1 `BasePressurePlateBlock#
  getCollisionShape`), keeping only the thin box for its outline/pick ray, and the
  trigger check probes the cell the feet stand in — removing the oscillation that
  came from coupling collision, the pressed-height toggle and the probe depth.
- In creative mode, Shift-clicking a hotbar item under an item-category tab now
  discards it (back to the infinite catalogue) even when the survival inventory is
  full. The active creative tab was client-only state that never reached the
  server, so the server always assumed the Inventory tab and the delete branch was
  dead code that degraded into a silent no-op swap once the inventory filled; the
  click now carries its tab context.
- A large number of block break/place/step/hit sounds now match 26.1. Blocks carry
  a full sound group (SoundType, transcribed per block from the 26.1 source and
  baked as a property, resolved once into event ids), retiring the ad-hoc whitelist
  that only covered seven groups, treated dirt as grass, knew three wool colours,
  and defaulted netherrack/nylium/basalt/soul blocks to stone.
- Doors, trapdoors and fence gates (open/close) and levers, buttons and pressure
  plates (trigger) now play their interaction sounds; the lever also gained the
  right-click toggle it never actually had. Flint and steel plays
  `item.flintandsteel.use` (instead of the fire block's place sound), and shearing
  a sheep plays `entity.sheep.shear`.
- The redstone torch is now fully aligned with the plain torch: it can be hung on
  a wall (and renders leaning like a wall torch), drops a redstone torch when
  broken (no longer ground-only); its LIT state swaps the lit/unlit sprite
  (`redstone_torch`/`redstone_torch_off`), its self-light comes from the state (7
  lit / 0 off, distinct from a torch's 14), and it is placed lit by default. This
  also fixes the redstone wall torch losing its collision/selection box when hung
  — its shape was keyed on block identity, so the redstone variant got a floor box
  in the wrong cell that the pick ray could not hit (it could not be broken); shape
  and mesh now both key on the "is a wall torch" trait so they can never disagree.

## ReBedrock beta5

### Added

- ReBedrock-specific interface text now has its own `rebedrock` language
  namespace, loaded through the same English-base/current-locale resource-pack
  stack as Java 26.1. English and Simplified Chinese option translations ship
  with the client, and packs can add any locale at
  `assets/rebedrock/lang/<code>.json` without replacing vanilla language files.
- Standard Java resource packs can now supply the game at runtime. ReBedrock
  discovers both directories and `.zip` files under `resourcepacks/`, layers
  them in deterministic filename order with the last pack winning per file,
  and lazily extracts zip entries into `.packcache` only when an asset is used.
  Pack metadata supplies the language catalog, and directory packs apply
  declaration-ordered `overlays.entries` whose format range includes 26.1's
  format 84. Namespaced resource locations now resolve textures, sounds, fonts
  and translations through one shared provider instead of hard-coded
  installation paths.
- Sound playback is driven by the active packs' `sounds.json` registries.
  Layered definitions honour append/`replace`, weighted candidates, per-entry
  volume and pitch, and recursive event references; blocks, the player,
  weather and every creature now request sound-event ids rather than guessing
  physical OGG filenames and variation counts.
- The 26.1 font provider stack is supported: `bitmap`, `space`, recursive
  `reference` and `unihex` providers (including filters and size overrides) are
  loaded from `font/*.json`. Languages are discovered from `pack.mcmeta`, built
  by layering `en_us`, the selected locale and `deprecated.json`, and prepared
  asynchronously after the Language screen's Done button is pressed so the
  visible interface stays responsive during a switch.
- Furnaces are real per-position block entities. Every placed furnace owns its
  input, fuel, output and burn/cook counters, keeps smelting while its screen is
  closed, spills its three slots when broken, and persists the contents and
  progress in world save format 15 so several furnaces no longer share one
  transient global inventory.
- World time is split into an unconditional server tick and independently
  pausable/rate-controlled named clocks. The overworld clock drives the sun,
  moon and natural-spawn lighting while simulation timers use the server tick;
  both are preserved by save format 13, with older saves migrated from their
  former `gameTimeSeconds` value.
- A central world-mutation service and 26.1-style mutation flags/cause records
  provide one ordered path for block-entity lifecycle, drops, neighbour
  notification and section invalidation. The service derives relighting and
  remeshing from the actual state change so callers cannot accidentally omit
  them.
- Player breaking and placing, water and lava buckets, tilling, furnace
  ignition, farmland trampling, tree growth and the simulation's own block
  writes all run through that service now. Placing a chest or furnace creates
  its block entity and breaking one destroys it and spills its contents
  automatically, instead of each call site having to special-case it, and lava
  poured from a bucket starts flowing immediately.
- Textures, JSON and fonts inside a zipped resource pack are now read straight
  from the archive in memory instead of being extracted to a disk cache.
- Fixed zipped *data* packs (tags and the like) resolving nothing at all.
- Blocks can be placed against chests and furnaces by sneaking with an item in
  hand; the container screen used to open unconditionally instead.
- Scooping water in creative no longer swaps the empty bucket for a full one.
  The protection now lives in the game mode and applies to every item.
- Spawn eggs no longer fail silently while a creature's model is still loading.
- Dropped items and blocks caught mid-fall are now saved with the world.
  Previously everything thrown or mined but not yet picked up vanished on
  reload, as did any sand or gravel partway through a collapse.
- The 20 TPS simulation now runs on its own thread, so a slow tick no longer
  stalls rendering and the two are decoupled through interpolation. Set
  `MC_REBEDROCK_SYNC_TICK=1` to fall back to ticking on the render thread.
- The shared world is now guarded by reader/writer sections: the simulation
  tick, player interaction and chunk-streaming batches each take a write
  section, while per-frame effect sampling and drawing take a read section.
- Creatures, dropped items and falling blocks are drawn from a render snapshot the tick publishes, so the draw
  pass no longer walks the entity list while the simulation is reordering it.
- Side effects raised by the simulation (block changes, sounds, particles, the
  player's death) are queued and replayed once a frame by the main thread, so
  the simulation no longer touches renderer state at the moment it publishes.
- Scheduled block work (falling blocks, fluids, support checks, leaf decay,
  tree growth) moved from five global queues to one per-chunk scheduler, so a
  chunk's pending work is dropped when the chunk unloads instead of later
  firing against cells that are no longer loaded. Processing order and budgets
  are unchanged.
- Random-tick dispatch moved from a central switch to a function-pointer table
  indexed by block, with the draw loop rejecting non-ticking blocks before it
  enters a call. Slightly faster than the switch in optimised builds, with
  identical behaviour.
- Which tool mines a block, and which tier keeps its drop, are now driven by
  26.1's block tag data (`mineable/*`, `needs_*_tool`) held as one 64-bit mask
  per block, replacing five hand-written switch chains. A pack that ships the
  `data/` half overrides them per tag; otherwise the compiled-in 26.1 defaults
  apply, since an ordinary resource pack carries only `assets/`.
- Block changes, sounds, particles and the player's death are now four event
  classes delivered through a synchronous single-threaded dispatcher.
  `SimulationHost` is retained and driven by a built-in subscriber. Every
  payload is a trivially copyable POD, so the simulation thread can later swap
  the dispatcher for a cross-thread queue without touching an emitter.
- 26.1 GUI sprite scaling metadata (`nine_slice`, `tile` and `stretch`, with
  per-side borders and `stretch_inner`) is parsed and applied. Menu buttons,
  slider tracks and slider handles are drawn nine-sliced, so their borders stay
  crisp at any width instead of blurring with the rest of the bitmap.

### Removed

- `resources/entity` is no longer distributed. Its only file was a zombie skin
  converted from Mojang's, which contradicted the goal of shipping no vanilla
  assets. Entity textures now come from the standard resource pack, or from the
  procedural placeholder.

### Changed

- Block states are now the cartesian product of a property list each block
  declares for itself — a crop's `age`, farmland's `moisture`, leaves'
  `persistent`, a furnace's `lit`, water's `level`, a facing — replacing three
  fixed axes. Save format 18 stores a block edit as an identifier plus named
  property values, so a block gaining a property no longer changes the file
  layout. Existing worlds open unchanged and upgrade on their next save; on the
  same test data the edit section is about 17% smaller and loads in roughly half
  the time.
- Five high-frequency systems now use C++-appropriate contiguous data paths: sound events and references compile to integer indices at resource load while static decoded assets remain cached; block-state properties use a raw-id metadata table; lighting reuses packed-node queues and an open-addressed membership set; entities update spatial membership only when crossing a section; and pathfinding reuses contiguous heap and node scratch. Save ids and gameplay results remain unchanged while allocation and hashing costs fall under dense entity and block-update workloads.
- Items now follow Java 26.1's registry-derived description-id convention.
  Ordinary items resolve `item.<namespace>.<path>`, block items resolve
  `block.<namespace>.<path>`, and their former C++ English/Chinese name fields
  have been removed. HUD labels, inventory tooltips, command suggestions and
  the window title therefore use the active resource-pack language, while a
  structured constexpr id and allocation-free heterogeneous language lookup
  keep the C++ render path from rebuilding Java-style strings every frame.
- ReBedrock no longer stages or installs Mojang textures, sounds, fonts or
  translations. A standard resource pack containing `pack.mcmeta` and
  `assets/` is now required at startup; when none is present the client creates
  `resourcepacks/`, prints its full location and exits with a bilingual
  diagnostic instead of launching without usable assets.
- Rendering and menus consume the 26.1 asset layout: named HUD/widget and
  furnace sprites, 16-pixel menu and list tiles, eight individual moon-phase
  textures, final spawn-egg icons, and the temperate cow/pig skins. Secondary
  menu screens retain the rotating panorama behind a five-pixel background
  blur while their controls remain sharp, and each asset remains independently
  replaceable by a pack.
- Blocks now use interned `BlockState` ids, separating a block's identity from
  its properties. A burning furnace is the furnace's `LIT` state and all four
  wall torches are one wall-torch block with a `FACING` state; format 14 saves
  these properties and migrates the old `lit_furnace` and directional
  wall-torch identifiers without losing light, facing or furnace progress.
- Chunk sections store their states in a per-section palette with bit-packed
  indices. All-air sections allocate no state heap, ordinary terrain uses only
  enough bits for the states it contains, and whole-state reads now carry
  orientation, fluid level and furnace light consistently through streaming,
  meshing, raycasts and lighting.
- Project documentation, entity data, resource paths and vanilla-behaviour
  references now target Java 26.1 rather than Java 1.16.1.

### Fixed

- Sandstone, bricks, quartz blocks, netherrack and furnaces now need a pickaxe
  to keep their drop, like every other stone. A bare hand used to harvest them,
  which disagreed with 26.1's `requiresCorrectToolForDrops`.
- Option labels and values no longer mix hard-coded English and Chinese text.
  Vanilla fields use their 26.1 translation keys, ReBedrock fields use project
  keys with a consistent English fallback, and dynamic labels are composed
  through `options.generic_value` / `options.percent_value` so each locale owns
  its word order, punctuation and percent formatting.
- Java 26.1 mob movement attributes are now converted once at the locomotion
  boundary instead of being interpreted directly as ReBedrock's internal
  blocks-per-tick scale. Ordinary movement is back to its intended pace while
  panic goals still apply their species-specific acceleration on top.
- Language entries use vanilla's centred 270-pixel selection width and matching
  hit area. The dark list background still spans the screen, while the
  scrollbar sits beside the inset entries instead of at the far window edge.
- Land mobs no longer treat leaf blocks as valid natural-spawn floors or path
  nodes. Ground selection now uses one shared support rule, preventing canopy
  spawning and keeping navigation from considering treetops ordinary terrain.
- Pressing Escape while the inventory is open now closes the inventory and
  returns directly to gameplay instead of allowing the underlying Game screen
  to open the pause menu in the same input event.
- Cow, pig and zombie movement attributes now use Java 26.1's actual base
  values. Panic speed modifiers therefore produce their intended visible
  acceleration instead of being applied to legacy values scaled five times too
  low.
- Terrain vegetation no longer contains the non-vanilla rare sapling showcase;
  saplings now appear only through player actions rather than chunk generation.
- The Language screen scrollbar thumb and track support mouse press-and-drag,
  including continuous scrolling while the cursor moves along the track.
- Tree crowns crossing chunk boundaries now update the gameplay block-state
  view as well as streamed meshes and lighting. Border foliage is retained for
  regenerated neighbours, while saved and runtime block edits remain
  authoritative, eliminating leaves that looked whole but collided as clipped
  or reappeared visually after moving away.
- Walking footsteps once again use the vanilla distance accumulator, keeping
  the normal walking cadence clearly slower than sprinting.
- Water and lava animation no longer depend on stale hard-coded atlas layers or
  the daylight clock. Atlas layout, frame counts and resource-pack `.mcmeta`
  frame timing now travel through one renderer contract, so animations continue
  when `doDaylightCycle` is disabled.
- The lava bucket is available in the creative Materials tab and can place or
  collect the current source-only lava block, allowing the lava renderer to be
  verified before full lava-flow simulation is implemented.
- Falling blocks rebuild their source and landing meshes immediately and use
  terrain-equivalent scene lighting while airborne, eliminating stale geometry,
  incorrect tinting and texture flicker during the static/entity handoff.
- Fast falling blocks no longer tunnel through a one-block surface and vanish.
  Collision sweeps every Y layer crossed during a tick; a failed placement
  becomes an item rather than being silently deleted, and an entity pauses while
  its owner chunk is unloaded.
- Temperate cows now use the complete Java 26.1 adult model that matches their
  64x64 skin. The missing 6x3 muzzle once again carries the nose and mouth UVs,
  horns and udder belong to their vanilla model parts, and the front-leg
  placement and mirrored left-leg texture layout match 26.1.
- Disabling `doDaylightCycle` no longer freezes mining, held-use delays or
  other gameplay timers, and `/time` changes no longer strand interaction
  cooldowns. Chat expiry, cursor blinking, held-name fades and idle animation
  use frame time, so presentation continues independently of the sun clock and
  pauses only with the relevant screen or game state.
- Furnace ignition and burnout now change a state on the same block instead of
  replacing it with another block, preserving the block entity and its smelt.
  State-aware relighting, meshing and persistence also keep the lit face and
  level-13 glow correct for furnaces that are closed or reloaded.
- Missing or damaged pack textures now produce the visible magenta-and-black
  missing-texture marker with a diagnostic instead of aborting texture-atlas
  creation; partial packs can therefore fall back cleanly through the active
  provider stack.
- Animated water and lava textures now read the explicit frame order from their
  `.png.mcmeta` sidecars and cycle or truncate non-standard frame counts to the
  atlas space available. Packs whose animation strips differ from vanilla no
  longer abort startup; frame timing and interpolation metadata are parsed for
  the future shader-timing path while the current playback rate stays fixed.
- The two bottom creative tabs now sample their own 26.1 tab sprites instead of
  both reusing the same fixed atlas slice.

## ReBedrock beta4

### Added

- Rain and thunderstorms now overcast the live skybox like 26.1: the sky and
  horizon darken, celestial sprites fade behind the clouds, and the rendered
  sky-light contribution falls smoothly with the weather gradient. Logical
  sky/block light levels stay unchanged, so gameplay checks and torch light are
  unaffected.
- The weather system works the way 26.1's does: `/weather clear` and
  `/weather rain` (each with an optional `[<duration>]` in seconds, defaulting
  to a 6000-tick spell) install a clear or rain spell that the doWeatherCycle
  auto-cycle takes over once it expires, and the rain intensity fades in and out
  at 0.01 per tick the way World's smoothed gradient does. The state persists
  with the save (format 11), `doWeatherCycle` gates the cycle like the other
  game rules, and the rain gradient is exposed to the renderer for a future
  particle-rain pass — no rain is drawn yet.
- Decorative stone variants round out the stone family: polished granite,
  polished diorite and polished andesite craft from a 2x2 block of their parent
  stone (four of the polished product), and smooth stone smelts from stone in
  the furnace — the same shapes and recipes as 26.1. All four register into
  the Building Blocks creative tab and save palettes.
- `/spawnpoint [<x> <y> <z>]` sets a personal respawn point the way 26.1's
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
- The Options menu gains an "实验性内容" (Experimental Content) sub-page holding
  the rain-render-mode selector (贴图雨 / 粒子雨 / 异步粒子雨) and the sun-shadow
  toggle, both persisted to the options file. The environment variables
  `MC_REBEDROCK_RAIN_MODE` / `MC_REBEDROCK_RAIN_COUNT` remain as headless-test
  overrides.
- Block-dust and rain particles render through a GPU instanced path: each
  particle's compact record is written into a per-frame storage buffer and the
  vertex shader expands the camera-facing billboard, so an N-particle burst is
  one `vkCmdDraw` instead of one per particle (a 64-particle block break drops
  from 64 draws to 1).
- Rain is drawn through three switchable paths. 贴图雨 follows 26.1's
  precipitation-column renderer: native-aspect `environment/rain.png`, stable
  world-column orientation and scroll phase, heightmap-like roof clipping,
  distance/rain-gradient alpha and scene light, batched into one instanced draw.
  Its 20 TPS `tickRainSplashing` pass independently samples nearby solid and
  fluid surfaces, placing compact impact particles on ground collision tops and
  the actual rendered water level instead of waiting for texture cards to land.
  粒子雨 and 异步粒子雨 share the same CPU-simulated drops so their legacy
  per-particle and storage-buffer backends remain directly comparable.
- A sun-space shadow depth pre-pass renders the in-frustum opaque terrain into
  an offscreen depth map ahead of the main pass, behind the
  `experimental.sunShadows` option (off by default). The map is infrastructure
  for a future shadow pass and is currently visible only through a debug
  overlay (`MC_REBEDROCK_SHADOW_DEBUG=1`).
- Each creature plays the 26.1 sound set its species declares, the way the
  Java entity classes override getAmbientSound / getHurtSound / getDeathSound /
  playStepSound: pigs grunt on their `mob/pig/say` clips and die with a single
  `death`, cows moo at 0.4 volume and reuse their hurt clips for death, and
  zombies groan, grunt and step through their `mob/zombie` set. Idle sounds
  fire on the vanilla baseTick scheduler (roughly every four seconds, reset by
  a landed hit), footsteps once per block walked, and every clip carries the
  species' volume with the ±0.2 randomised pitch.
- Rain has a voice: while the rain gradient is above zero the client fires a
  short `weather/rain` clip at the surface the drops are hitting, on the
  per-frame cadence of WorldRenderer#tickRainSplashing. The volume follows the
  smoothed rain gradient (a drizzle is faint, a full storm loud) and the
  thunder gradient boosts it so 雷雨天 rains harder than plain rain, and when
  the drops land on a roof over the player the muffled `rain-above` clip plays
  instead — vanilla's quiet indoor rain.
- The 实验性内容 page gains a 粒子效果 (particle-effect) level selector:
  低 / 中 / 高 / 疯狂 at 0.5x / 1x / 2x / 3x of the current density, cycling
  like the other options and persisted as `experimental.particleLevel`. The
  level scales the rain-drop budget and the particle system's live cap and
  per-event spawn counts (block dust, splashes). Plain rain and thundering rain
  are both raised to 1.5x their pre-bump baseline, and the per-frame particle
  storage buffer grows to 3 MiB so the 疯狂 level's 36,000-drop thunderstorm
  renders in a single instanced draw without clamping.
- `MC_REBEDROCK_PARTICLE_LEVEL=0..3` overrides the level headlessly, the same
  way the rain env vars override the menu.
- The rain field is wider and taller and its collision is cached instead of
  per-drop-per-frame: the box reaches ±24 blocks (was ±16) and drops spawn up
  to ~32 blocks above the camera, so tall roofs stop the rain and splashes
  appear out where the field used to be empty. The first drop to enter a column
  probes that column's topmost surface once and caches it; every drop after
  just falls to the cached surface and splashes there. A drop that wind drifts
  sideways into a wall splashes on the wall's side: the first drop to cross a
  wall face caches the face's outward direction, and every later drop reaching
  the same face bounces its droplets back off the wall in that fixed fan.
  Spawn heights are randomised across the window, so the field reads as a
  continuous fall instead of a synchronized sheet, and the splash droplets are
  bigger and live longer so they read at a distance. The wind heading now holds
  for 10-20 seconds and then eases to a new direction, instead of rotating
  constantly and spinning the whole field. A 45,000-drop crazy thunderstorm
  drops from 45,000 world lookups a frame to a handful of probes plus cheap
  cache reads, and a unit test pins the behaviour (nothing falls through a roof
  slab, splashes only on real surfaces, wall splashes appear under wind and
  spray back off the wall). The 实验性内容 page gains a 雨碰撞缓存 toggle
  (persisted as `experimental.rainCollisionCache`, overridable with
  `MC_REBEDROCK_RAIN_COLLISION_CACHE=0`): on by default, it uses the cached
  collision; turning it off reverts to the direct per-drop world probe for
  machines with headroom.

- Creatures carry a stable identity that survives the vector compactions a tick
  can cause: raycasts, hurt/kill and commands address a creature by its
  `uint64` id instead of a vector index that went stale the next tick, `byId`
  resolves in O(1), and the id space resets when a world reloads. Nearby
  queries are sped up by a chunk-section spatial hash over the herd: pushing
  tests only the sections a creature and its neighbours occupy, a raycast walks
  the sections it passes through, and the item magnet looks up the player's
  surrounding buckets — the O(n²) herd sweep and the O(n) scans become
  O(neighbours).
- Simulation distance (26.1's tick-distance gate): creatures farther than the
  configured radius from the player are frozen each tick — no AI, movement,
  gravity or timers — while staying rendered and targetable, and
  distant-despawning categories (MONSTER/AMBIENT/WATER_CREATURE) past the
  128-block despawn range are silently removed after forty ticks the way
  MobEntity#checkDespawn does; animals persist.
- A Simulation Distance slider on the Video Settings page, on the same row as
  Render Distance, counting in chunks (2–12) and persisted as
  `render.simulationDistance` — it sizes the frozen-entity radius independently
  of the render distance.
- Natural spawning (26.1's NaturalSpawner): every second the spawner probes
  positions inside the simulation radius, checks the ground and the light rule,
  samples the biome's spawn table by weight and spawns a group — respecting
  each category's spawn cap scaled to the simulated area, never inside the
  24-block ring around the player, and counting only the nearby population so a
  new area repopulates as you walk on. The day/night sky brightness decides the
  category: open ground is creature territory by day and goes dark at night, so
  MONSTERs spawn on the surface at night as well as in caves. Peaceful worlds
  never host natural hostiles.
- Creatures persist with the save (world.dat format 12): the live herd is
  serialised into a self-describing `ENTITY` block — a species palette plus each
  creature's position, yaw, velocity, health, anger/age timers and wander rng —
  and restored by species on load, so a world reopens with its animals where
  you left them. The forced four-creature debug herd at spawn is gone, replaced
  by natural spawning.
- Land mobs navigate (26.1): a per-creature brain plans a path around
  obstacles to its target, an escape goal flees a recent attacker (for the
  reachable distance, falling back to a shorter route on a plateau), and a swim
  goal keeps water creatures afloat; a stable actor reference lets goals keep
  following the same attacker across the tick boundary.
- Hostile creatures attack the player: AI emits melee-attack events each tick,
  and the session applies them to the player scaled by difficulty through the
  shared hurt pipeline, so a zombie that reaches you lands damage instead of
  milling around.
- Mobs' pending sound events carry their event type (hurt / death / ambient /
  step) so the host plays each species' own clip, and the base tick schedules
  the idle sound on the vanilla ambient counter and footsteps per block walked.

### Changed

- The Vulkan client has been split into focused rendering components without
  changing its external behaviour: device/bootstrap helpers, texture and block
  atlas ownership, HUD drawing, world drawing, shared render records and menu
  geometry now live outside `VulkanRenderer.cpp`. This removes several thousand
  lines from the central implementation and gives later runtime/gameplay
  separation explicit rendering boundaries.
- Ambient weather may now occupy at most 75% of the scaled particle pool. Block
  breaking and bucket splashes retain a reserved quarter and evict the oldest
  weather particles when necessary, so interaction feedback remains visible in
  dense rain; the medium pool grows to 8,000 live particles.
- Grass and oak-leaf colours are baked into their atlas layers at build time
  per biome (the swamp's dark tone included) instead of being tinted per-vertex
  in the fragment shader, so the biome lookup no longer rides the vertex pad
  byte; spruce and birch leaves keep their fixed tinted layers.
- The sun-shadow pre-pass is off by default and caps its caster list at the
  nearest 512: it re-renders every opaque section each frame, and during
  chunk-streaming bursts that was the heaviest new GPU load.
- The sun-space shadow depth map is now applied to the terrain: the
  `grass_block.frag` and `block_cutout.frag` surface shaders sample the pre-pass
  depth (descriptor binding 8) through `lightViewProj` carried in the camera
  uniform, so opaque terrain casts a sun shadow on itself while the
  `experimental.sunShadows` option is on — the pre-pass is no longer visible
  only through the debug overlay.

### Fixed

- Creature picking now tests the exact living-entity bounding box and probes
  neighbouring spatial sections, so aiming just beside a mob reaches the block
  behind it while tall or boundary-straddling mobs remain selectable.
- Natural spawning now validates every group member's complete species AABB,
  block placement refuses cells occupied by a creature, and restored or legacy
  creatures already embedded in terrain recover to the nearest free face.
- Hit creatures no longer hang in the air: living mobs use 0.08 gravity and
  0.98 vertical drag after movement, and an airborne follow-up hit no longer
  restarts the full upward knockback arc.
- The async rain backend no longer benchmarks five times as many drops as the
  ordinary particle backend. Both now use the same particle-level budget, and
  the instanced path reuses one host-side staging allocation and combines the
  particle/rain copy and cache flush, removing the Windows-heavy per-frame
  allocation and second flush. Thunderstorms target 6,000 / 12,000 / 18,000
  drops at medium / high / crazy on both backends.
- Switching a running world to Peaceful now updates the session difficulty as
  well as player vitals, so existing zombies disappear on the next game tick
  and the natural spawner stops creating new monsters.
- Opening "实验性内容" in the options menu no longer falls through to the
  terrain-loading screen — the page was missing from the menu draw dispatch, so
  it rendered the loading backdrop instead of its three option buttons.
- Rain splash droplets landing on a water surface rest on it like any solid
  block: the particle collision now treats a fluid cell as a landing surface, so
  the droplets stop at the water's top face instead of sinking below it under
  gravity.
- A falling drop whose respawn found every nearby column under a tall roof no
  longer spawns at the camera point: the fallback still randomises the position
  in the field, so a fully-covered player no longer sees a solid water column
  of fallback drops pouring down in one spot.
- The new render pipelines destroy their shader modules after creation, closing
  an eight-object device leak the validation layers reported at shutdown.
- The whole vanilla sound tree is staged into the build instead of a
  five-group subset (block families plus pig only): every mob species' clips,
  weather/rain and the ambient/entity/record/UI assets now ship, so the
  "Missing sound asset" spam from the newly wired cow/zombie and rain sounds
  is gone. The 155 MB music catalogue trims to the two classic C418 tracks
  (Sweden and the main-menu theme).

- Eating now plays the chewing loop through the meal: the `generic.eat` sound
  fires every fourth tick once the eat is past its first seven ticks, with the
  final burst and burp on completion — matching LivingEntity's consumption
  effects instead of a single sound when the meal starts.
- Inventory block thumbnails use vanilla 26.1's plain per-face luminance
  (up 1.0, west 0.6, east 0.8) with no colour bias, plus a per-corner ambient
  occlusion gradient so the cube edges read slightly darker than the faces.

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
- The swamp biome is flooded the way 26.1's is: its terrain sits at or below
  sea level so standing water covers most of it, the underwater floor is dirt
  (not gravel), and its oaks — which keep the vanilla radius-3 canopy — root
  through a single block of shallow water instead of marching across dry land.
  Tree placement retries a few cells when the first lands in open water, so the
  swamp's per-chunk tree count still comes through despite the drowned patches.
- The block texture atlas is now built by name from the block registry, the way
  26.1 registers sprites by ResourceLocation: each block's faces name their
  textures ("granite", "grass_block_side", ...), the renderer loads and dedupes
  them once at startup, and every block's resolved atlas layers land in a flat
  per-block table the mesher and GUI read with a plain array index. The
  transparent itemLayerPlaceholder slots that used to pin hand-ordered layer
  numbers are gone — the atlas has no empty layers, and adding a block is a
  single `.texture("name")` line with no risk of a runtime layer-count crash.
- A dead mob drops its loot in every game mode — the creative gate that
  silently swallowed the drops is gone, matching vanilla's death-time loot roll.
- A new world can no longer deadlock on "Creating terrain": the spawn search
  that found no valid column (a large sea around the preferred centre) used to
  leave the loading screen forever, on the first run and every reload of the
  same seed. It now falls back to the highest solid surface so the world always
  loads, and the spawn region is marked as never-unloading vanilla spawn
  chunks, so the player's home base stays loaded for the whole session.
- A creature's loot drops on the tick it dies, not after the twenty-tick corpse
  animation: every death path — melee, /kill and fall damage — now funnels
  through a single onDeath guard (the shared `beginDeath` latch) that rolls the
  loot table at the death moment, matching LivingEntity#onDeath instead of
  dropping when the body disappears.
- Player death and respawn run through the same one-shot onDeath guard: a tick
  that kills through two sources at once raises the death screen once, and
  respawning resets the whole body — momentum, flight/sprint/sneak state, fall
  distance, jump cooldowns and the FOV multiplier — onto the personal (or
  world) spawn point facing the spawn's stored angle, so nothing from the death
  is carried into the new life.
- Large bodies of water no longer show the chunk-seam artifacts of a neighbour
  chunk that has not streamed in yet: the surface corners, flow direction and
  optical depth sample the missing neighbour as the current water body instead
  of a gap of air (so the sheet stays flat instead of cracking into a straight
  row of same-direction flowing water), and a fluid face pointing into the
  missing chunk is culled (so the ocean does not render a waterfall-like
  vertical cut from the surface to the seabed). Both heal automatically when
  the border is remeshed with real data.
- Generation no longer leaves grass under shallow water: a column whose top
  solid block sits under water (normally the one-block-deep pond bed) gets the
  biome's filler material instead of a grass block, so a new pond does not
  spend random ticks converting its bed to dirt for no player-visible gain.
- The sun and moon no longer sample the wrong atlas layers: the sky shader
  picked hardcoded indices from the pre-name-driven atlas (sun 137, moon 224),
  so after the atlas was rebuilt the sun showed a destroy-stage frame and the
  moon a mossy-cobblestone block. The renderer now passes the derived sun and
  moon-first-phase layers through the camera uniform, and the shader reads them
  there — the two can no longer drift when the atlas layout changes.
- Biome surface boundaries no longer switch on hard four-block steps: the
  surface material is chosen by a bilinear vote over the column's surrounding
  biome cells (cells sharing a surface block pool their weight), so the
  sand/grass seam follows a smooth interpolated line instead of a staircase of
  right angles. The terrain height was already blended across biome boundaries;
  the material now matches it. In the narrow band where two materials trade
  dominance the seam is dithered with a per-position hash, so a long boundary
  reads as a soft 1-2 block blend instead of one hard line.
- Grass blocks, tall grass and foliage take the 26.1 per-biome colour: each
  biome's grass and foliage colour comes from the vanilla grass/foliage colour
  maps (indexed by the biome's temperature and downfall, with the dark-forest
  grass darkening, the swamp's fixed 0x6A7039 foliage and its noise-mottled
  0x6A7039/0x4C763C grass tones, and the fixed spruce/birch leaf colours), so
  plains are bright green, desert olive, taiga blue-green and so on. The biome
  is stored per column at generation; the grass side keeps its baked per-biome
  dirt + green strip and spruce/birch their fixed tones, while the grass tops,
  plants and oak-family leaves take their colour from the fragment lookup below.
- The grass and foliage colour blends across biome boundaries as a smooth
  per-pixel gradient, the way 26.1's BiomeColors does but robust: the renderer
  generates two biome-colour lookup textures from the world seed and the vanilla
  grass/foliage colour maps (512 texels at four blocks each, covering the spawn
  region), and the terrain fragment shader samples them with linear filtering —
  so the GPU hardware-interpolates the colour between adjacent biome cells and
  the boundary cannot wash out into the raw grey texture. The mesher stamps a
  per-vertex mask (which biome map to apply) in the vertex pad byte: grass
  tops/plants sample the grass map, oak/jungle/acacia/dark-oak leaves the
  foliage map, and everything else is untouched. The grass side keeps its baked
  per-biome layer so the dirt under a cliff stays dirt, and spruce/birch leaves
  their fixed 26.1 tones. The lookup textures regenerate when a world loads
  (per seed); grass tops and oak-family leaves now use the untinted base
  textures and take their colour from the fragment lookup.
- A torch rendered as a mostly transparent, cracked texture: the mesher
  hardcoded the torch's atlas layer (a stale fixed-section index) instead of
  the layer the name-driven atlas assigned to the "torch" sprite, so the quad
  sampled an unrelated transparent tile. The torch and its four wall variants
  now read their resolved texture layer like every other block.

## ReBedrock beta3

### Added

- A "High" smooth-lighting tier (`lighting.smooth=high`), implementing the
  vanilla 26.1 per-block ambient occlusion: full opaque cubes darken corners
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
  like 26.1) and `keepInventory` (keeps the inventory through death).
  `/gamerule <rule>` now queries the current value, and rule names accept
  `minecraft:` prefixes and any case.
- Game rules are persisted as a sparse, self-describing block in `world.dat`
  (save format 9): only rules that differ from their default are written, every
  entry carries its name/type/length/value, and the block has its own version
  and size so an older build skips what it cannot read. A format-8 save's
  `randomTickSpeed` header field is migrated into the block on load.
- Java 26.1's random-tick simulation. Every game tick, `randomTickSpeed`
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
- Basic 26.1 mouse gestures in the inventory and containers. Shift-click
  (SlotActionType.QUICK_MOVE) moves a stack between a chest/crafting/furnace
  slot and the player inventory in either direction — a furnace routes a
  smeltable item to its input and anything burnable to its fuel slot. And
  dragging (SlotActionType.QUICK_CRAFT) works like vanilla: pick a stack up and
  sweep it over slots while holding the button; a left-drag shares the stack
  out as evenly as possible, a right-drag drops a single item into each slot.
  Double-clicking a slot (SlotActionType.PICKUP_ALL) gathers every matching
  stack in the screen into the cursor, the quick way to tidy scattered items.
- A burning furnace now shows it: the front face switches to 26.1's
  `furnace_front_on` texture and the block glows with the lit furnace's level-13
  light while fuel burns, then reverts when the fire goes out. The lit state is
  transient — it is not saved, and breaking a burning furnace drops the plain
  furnace item.
- A cow species, ported from 26.1 `CowEntity` and `QuadrupedEntityModel`: the
  second CREATURE species, reusing the animal AI with no shared-code changes. It
  binds the vanilla `entity/cow/cow.png` skin, runs a 26.1-style gait (0.6662
  leg frequency, 1.4 rad swing, diagonal leg pairing), drops 1–3 raw beef and
  0–2 leather, and has its own spawn egg. The spawn-area demo herd is now 2 pigs
  + 2 cows, and a raw beef item (with a vanilla `item/beef.png` icon) joins the
  food catalogue.
- A crop-farming system, following 26.1's blocks, loot and crafting. Four new
  blocks — farmland (tilled with a hoe), and the wheat, carrots and potatoes
  crops — plus wheat seeds, wheat, carrot and potato items. A hoe right-clicks
  dirt, grass and podzol into farmland (coarse dirt back into plain dirt, the
  vanilla HOE_LOOKUP map) and wears one durability point per till. Seeds,
  carrot and potato plant on farmland; the crops grow on the random tick, need
  light 9 in the block above, and grow up to three times faster ringed by moist
  farmland (26.1's getAvailableMoisture and the 25/moisture growth odds), so
  `/gamerule randomTickSpeed` scales the rate like every other random tick.
  Mature wheat drops wheat plus a binomial roll of seeds, carrots and potatoes
  drop their produce, and an unripe crop returns a seed; tall grass drops wheat
  seeds 1/8 of the time. Three wheat in a row craft bread. Breaking farmland
  yields dirt and pops any crop standing on it, with the crop's loot rolled
  from the age it had reached.
- Farmland moisture, per 26.1's FarmlandBlock: water within four blocks
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
  the way 26.1's ItemEntityRenderer draws the same ItemRenderer model in
  GROUND transform. Block items keep their miniature cube.
- Crops select like their stage: the raycast box grows from 2/16 (a sprout) to
  a full block as the crop ages (26.1's CropBlock.SHAPES), and the selection
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
- A dedicated Language page, following 26.1's LanguageScreen: a dark
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
  horizontal move now mirrors 26.1's `adjustMovementForCollisions`: the
  dominant axis resolves first, and a blocked grounded move is retried from
  vanilla's 0.6 step. The player never lifts a full block on their own — the
  0.6 step only clears stairs and partial blocks — so a one-block rise still
  needs a jump.
- The first pass at that step let the player climb a full block by walking,
  which is not how a Java 26.1 player moves; it was removed again. In its
  place a Bedrock-style auto-jump is available: with Auto-Jump on in Controls,
  walking forward into a one-block rise jumps automatically (it never fires
  against a two-high wall or a missing headroom). It is off by default, keeping
  the vanilla feel.
- Tree crowns no longer stop flat at the chunk border. Chunk generation runs
  in isolation, so a tree near the edge used to drop every leaf past the 0..15
  box — a large share of trees came out clipped. The generation writer now
  holds those blocks back and the streamer applies them to the neighbouring
  chunk when it is published, so the crown is finished across the seam. The
  oak/birch foliage was rebuilt to 26.1's actual `BlobFoliagePlacer`: the
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
  button click sound. In 26.1 only `AbstractButtonWidget` buttons click;
  slots, the creative tabs and the delete slot are silent, so the click now
  fires only for the menu buttons.
- Sand and gravel stopped falling. A chunk-load pass that scheduled fall checks
  for freshly generated gravity blocks queued *every* sand cell in the chunk,
  and a desert chunk holds thousands of supported ones; with the shared queue
  draining 64 per tick, the genuinely floating blocks were buried behind a
  backlog that never drained. The pass is gone entirely: as in 26.1,
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
- The cow's model now matches the 26.1 `CowEntityModel`. It was built from the
  pig's torso (10×16×8 at box-UV 28,8), so the body came out too short and
  narrow and its net sampled the wrong region of `cow.png`. The torso is now the
  cow's own 12×18×10 at UV 18,4; the head is the vanilla 8×8×6 sitting at world
  y 16..24 (it used to be an 8×8×8 sunk below the body top); the horns and the
  udder the Java model adds as children of the head and torso are present; and
  the legs use the vanilla ±4 pivots with the front pair at z −6. The fix also
  includes the same horn/udder bones in the compiled-in built-in model, so the
  creature stays correct when the resource files are missing.
- The cow and pig no longer wear their belly texture on their backs. The
  body's box-UV faces were mapped for a non-flipped renderer, but 26.1 draws
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

- A title-screen panorama that follows 26.1's `CubeMap` renderer: the six
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
  saturation and exhaustion following Java 26.1: sprinting and jumping burn
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
  and `rebedrock:book`, with the `minecraft:` name kept as an alias so 26.1
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
  carrying its Java 26.1 harvest level, mining speed, attack damage/speed and
  durability in a tool table. Axes are the right tool for wood, shovels for
  dirt, sand and gravel, and hoes for leaves, all craftable through their
  vanilla recipes.
- `/give <item|index> [count]`: hands over any registered block or item by
  namespaced key, vanilla alias or bare name — or by creative-catalog index —
  and spills whatever the inventory cannot hold onto the ground at the player's
  feet.
- Eating. Apple and bread restore hunger and saturation (4 food / 2.4 and
  5 food / 6.0, following Java 26.1 FoodStats) after the vanilla 32-tick use
  duration, with a first-person eating animation that raises the held item
  toward the mouth and the vanilla eating and burp sounds.
- Creature fall damage follows 26.1: a mob that falls more than three blocks
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

- A world now opens with two-phase loading the way 26.1 enters a world with a
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
  way 26.1's `Items` registry holds `Items.STONE` for `Blocks.STONE`: every
  block has a plain `BlockItem`, and the torch is the `StandingAndWallBlockItem`
  that carries its standing and wall variants. Block identifiers resolve as
  items too, so `/give` and the save palette treat a block like vanilla does.
  The interaction routes placement through the held item's own placement state —
  the torch item decides wall-vs-floor from the clicked face, a plain block item
  places its block — instead of a hardcoded block switch. A block stack still
  keeps its block as its identity, so old saves, mined drops and crafted stacks
  all behave unchanged.
- Right-clicking now routes through 26.1's `Item#useOn`: the held item
  resolves the outcome by its own class (a block item places, a spawn egg
  spawns, the buckets collect or pour water), and the interaction loop applies
  the world-edit and audio side effects from the answer instead of comparing the
  item in a chain of `if`s. The container a clicked block opens — crafting
  table, furnace or chest — is read off the block's own registry entry
  (`container`), so no screen in the interaction loop names a block.
- Leaves are wielded as their own `LeavesBlockItem` like 26.1, the class that
  marks hand-placed leaves PERSISTENT so they never decay. The flag is stored in
  the block's orientation state, which the item resolves at placement; the
  block-property `placementOrientation` no longer special-cases leaves.
- The tiled `options_background` behind the menus (title sub-screens, the world
  list and the load screen) now keeps the vanilla 32-pixel tile at the current
  GUI scale — every tile is `32 × guiScale` pixels — instead of a fixed 64
  pixels. At GUI scales above 2 the tiles were visibly too small and dense.
- Every menu screen shares the one optimized dirt backdrop, and the singleplayer
  save rows sit on 26.1's dark list panel: `options_background` tiled under a
  solid (32,32,32) tint, half the menu dirt's (64,64,64), so the list reads as a
  deep near-black band of dirt. The flat dark overlay over the title-screen
  options is gone, so that screen shows the plain optimized dirt.
- The dirt and any other tinted GUI sprite are tinted in sRGB space, the way
  26.1 multiplies the raw texel by the vertex colour. The sRGB swapchain had
  been tinting in linear space, which left every dark tint noticeably brighter
  than vanilla — most visibly the menu dirt.
- New worlds start with an empty inventory instead of a pre-filled one.
- Wood-type blocks follow 26.1's mineable/axe tag: logs, planks, bookshelves,
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
  mines them faster, exactly like 26.1.

- Holding the attack button in creative keeps breaking blocks, and holding the
  use button keeps placing them, on the vanilla 5- and 4-tick delays instead of
  needing one click per block.
- Block-break particles now match 26.1. A full cube sheds the vanilla
  4×4×4 = 64 dust pieces (fewer for the torch and cross-plant outline shapes),
  at the vanilla velocities, 0.8-blocks-per-second gravity, per-tick drag and
  4–40-tick lifetimes, with a random texture sub-tile per particle. Each one
  also samples the block light at its own position, so the dust dims in caves
  and lights up next to a torch exactly like the block it came from instead of
  staying full-bright everywhere.
- Difficulty moved into each world save. It lives in the world's level data the
  way 26.1 keeps it in `level.dat` and is no longer a global options entry;
  a new world starts on Normal, the difficulty button only appears on the
  in-world options page, and it edits the open world's setting in place.

### Fixed

- A wall torch floated off its wall in two ways. First, placed at an angle —
  onto a replaceable plant or a non-sturdy block — it could attach to a wall
  behind the placement cell and lean back toward the player; the fallback wall
  search now walks toward the wall the player is looking at (26.1's
  `getNearestLookingDirections`) instead of a fixed north/south/west/east sweep.
  Second, the model's root was inset 0.38 of a cell from the wall face, leaving
  a two-pixel gap that read as floating; the root now sits flush against the
  wall, matching the 26.1 `WallTorchBlock` AABB that runs to the block face
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

- The first-person eating animation followed 26.1's
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
  spent a stack the way a survival one would. Java 26.1 creative players run the
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
