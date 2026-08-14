# Runtime resources

- `shaders/src`: project-owned GLSL 450 source files.
- `animation`: project-owned entity geometry and animation data.
- `lang/rebedrock`: project-owned option translations layered after the active
  vanilla language. Resource packs may extend the same namespace.
- `default-options.properties`: the default runtime configuration.
- `config` and `ui`: reserved project-owned resource roots.

Mojang assets are not stored or staged here. At runtime the player supplies a
standard Java 26.1 resource pack under the game directory's `resourcepacks/`.
CMake stages only ReBedrock-owned resources and writes generated SPIR-V files
below the active build directory.
