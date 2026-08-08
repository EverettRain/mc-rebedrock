# Source layout

- `core`: application lifetime, timing, logging, jobs, filesystem and JSON helpers.
- `animation`: unified data-driven animation — Bedrock geometry/animation JSON, a
  Molang evaluator and a skeletal Animator for blocks, players and mobs
  (see `animation/README.md`).
- `platform`: windows, input and platform-specific paths.
- `render/vulkan`: Vulkan instance, device, swapchain, resources and renderer.
- `assets`: resource locations and texture/model/sound loading.
- `world`: blocks, chunks, terrain generation, lighting and meshing.
- `gameplay`: player movement, collision, game modes, extensible command dispatch, inventory, item entities and block interaction.
- `ui`: page stack, HUD layout, chat history, the bitmap/unicode text font and
  the translation table.
- `audio`: audio decoding, mixing and spatial playback.
- `persistence`: the single-slot manual save system.
