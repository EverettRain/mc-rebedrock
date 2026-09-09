#ifndef INCLUDE_VERTEX_NORMALS
#define INCLUDE_VERTEX_NORMALS

// 打包顶点里那个法线下标指向的表。与 render/MeshData.hpp 的 kVertexNormals 逐位一致，
// 由 sun_shadow_map_test 对拍。
//
// RN-51：从 grass_block.vert 搬进来，因为阴影通道也要读它了（见 shadow.vert 里那条
// 「近乎侧对光的薄投射者不投影」）。两份 GLSL 副本正是这条链子上反复出问题的形状，
// 所以搬的时候顺手合成了一份。
const vec3 kVertexNormals[15] = vec3[15](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
    vec3(0.0, 0.900552, -0.434749), vec3(0.0, -0.434749, -0.900552),
    vec3(0.434749, 0.900552, 0.0), vec3(0.900552, -0.434749, 0.0),
    vec3(0.0, 0.900552, 0.434749), vec3(0.0, -0.434749, 0.900552),
    vec3(-0.434749, 0.900552, 0.0), vec3(-0.900552, -0.434749, 0.0),
    vec3(0.0, 1.0, 0.0)
);
const int kThinPlaneNormalIndex = 14;

#endif // INCLUDE_VERTEX_NORMALS
