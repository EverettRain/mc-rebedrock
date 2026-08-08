#version 450

// Colour output is masked off for this pipeline; the shader exists so the
// pass can be rasterised at all.
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
}
