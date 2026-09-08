#version 450

layout(push_constant) uniform PanoramaPush {
    vec4 rotationFov; // x = yaw, y = pitch (radians), z = tan(fov/2), w = aspect
} pano;

layout(location = 0) out vec3 viewRay;

const vec2 triangle[3] = vec2[](
    vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0)
);

void main() {
    vec2 ndc = triangle[gl_VertexIndex];
    vec2 tanHalfFov = vec2(pano.rotationFov.z * pano.rotationFov.w, pano.rotationFov.z);
    // Deliberately NOT normalized: the raw perspective ray is linear in NDC, so
    // interpolating it across the fullscreen triangle yields the correct ray at
    // every pixel, with the projection centred on the screen (distortion grows
    // toward the edges). Normalizing here would bend the interpolation and move
    // the undistorted sweet spot off to a corner.
    // The NDC y is negated so the panorama faces upright: the vanilla cube's
    // 180-degree X flip makes the screen-up ray land on the -Y face, so the
    // images arrive upside down without this vertical reflection.
    viewRay = vec3(ndc.x * tanHalfFov.x, -ndc.y * tanHalfFov.y, -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0);
}
