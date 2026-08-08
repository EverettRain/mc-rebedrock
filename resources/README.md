# Runtime resources

- `shaders/src`: project-owned GLSL 450 source files.
- `config`: project-owned runtime configuration.
- `ui`: project-owned UI resources.
- `vanilla/1.16.1`: locally extracted Minecraft resources; ignored by Git.

The project references the source resource tree during development. Generated SPIR-V files are
written below the active CMake build directory.
