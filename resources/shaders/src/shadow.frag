#version 450

// The shadow pre-pass is depth-only (no color attachment); the rasterizer
// writes depth from the vertex positions and this trivial fragment keeps the
// pipeline on the well-trodden fragment-shader path across drivers.
void main() {}
