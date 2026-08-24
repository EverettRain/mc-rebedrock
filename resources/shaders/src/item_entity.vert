#version 450

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;

layout(push_constant) uniform ItemPush {
    vec4 positionSize;
    vec4 textureLayersRotation;
    vec4 data;
    vec4 dimensions;
    mat4 viewModelTransform;
} item;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
layout(location = 2) out vec3 fragmentNormal;
layout(location = 3) flat out float fragmentIsCube;
layout(location = 4) flat out float fragmentShadowOpacity;
layout(location = 5) flat out float fragmentOpacity;
layout(location = 6) out float fragmentCameraDistance;
// 1.0 selects the dedicated entity/creature texture array (binding 4) instead of
// the 16x16 block texture array, and marks fragmentUv as real box-UV atlas
// coordinates rather than a per-face full-quad sample.
layout(location = 7) flat out float fragmentEntityTexture;
// Normalised (sky, block) light levels sampled once per entity on the CPU, the
// way vanilla feeds one lightmap coordinate per entity. Negative means "no
// scene light supplied": the fragment shader then keeps the legacy fixed light.
layout(location = 8) flat out vec2 fragmentSceneLight;
// World position of world-space draws, so the moving point lights (held torch)
// reach entities exactly as they reach terrain. Only meaningful when
// fragmentSceneLight is non-negative, which never happens on view-space paths.
layout(location = 9) out vec3 fragmentWorldPosition;
// OverlayTexture's hurt row: 1.0 while a creature is inside its hurtTime, 0.0
// otherwise. Only the box-UV entity path ever raises it.
layout(location = 10) flat out float fragmentHurtFlash;
// Falling blocks use terrain-equivalent face lighting and shadows rather than
// the generic dropped-item light. Kept as a mode bit so ordinary item cubes and
// articulated entity cuboids retain their existing presentation.
layout(location = 11) flat out float fragmentFallingBlock;

const vec2 corners[6] = vec2[](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5)
);

const int quadIndices[6] = int[](0, 1, 2, 2, 3, 0);
const vec2 faceUvs[4] = vec2[](
    vec2(0.0, 1.0), vec2(1.0, 1.0),
    vec2(1.0, 0.0), vec2(0.0, 0.0)
);

// dimensions.w carries the per-entity scene lightmap sample: 0.0 means "no
// scene light", otherwise 1.0 + skyLevel + blockLevel * 16.0 with both levels
// integers in [0, 15]. The two levels share one float because ItemPush already
// sits exactly on Vulkan's guaranteed 128-byte push-constant limit, and w is
// the only slot every draw mode leaves free (the cuboid modes use dimensions.xyz
// as the box extent, the billboard modes leave dimensions zeroed).
vec2 decodeSceneLight(float packedLight) {
    if (packedLight < 0.5) {
        return vec2(-1.0);
    }
    float value = packedLight - 1.0;
    return vec2(mod(value, 16.0), floor(value / 16.0)) / 15.0;
}

void main() {
    fragmentEntityTexture = 0.0;
    fragmentHurtFlash = 0.0;
    fragmentFallingBlock = 0.0;
    fragmentSceneLight = decodeSceneLight(item.dimensions.w);
    fragmentWorldPosition = vec3(0.0);
    if (item.data.x > 6.5 && item.data.x < 7.5) {
        int layer = int(item.textureLayersRotation.x + 0.5);
        vec3 local = vec3(0.0);
        vec3 normal = vec3(0.0, 0.0, 1.0);
        vec2 uv = vec2(0.5);
        float edgeVisible = 1.0;
        if (gl_VertexIndex < 12) {
            int face = gl_VertexIndex / 6;
            int corner = quadIndices[gl_VertexIndex % 6];
            vec2 xy = vec2(
                corner == 0 || corner == 3 ? -0.5 : 0.5,
                corner < 2 ? -0.5 : 0.5);
            local = vec3(xy, face == 0 ? 0.03125 : -0.03125);
            normal = vec3(0.0, 0.0, face == 0 ? 1.0 : -1.0);
            uv = vec2(xy.x + 0.5, 0.5 - xy.y);
        } else {
            int edgeVertex = gl_VertexIndex - 12;
            int edgeQuad = edgeVertex / 6;
            int vertex = edgeVertex % 6;
            int corner = quadIndices[vertex];
            int pixel = edgeQuad / 4;
            int edge = edgeQuad % 4;
            ivec2 texelPosition = ivec2(pixel % 16, pixel / 16);
            ivec2 neighborOffset = edge == 0 ? ivec2(-1, 0)
                : (edge == 1 ? ivec2(1, 0)
                : (edge == 2 ? ivec2(0, -1) : ivec2(0, 1)));
            ivec2 neighborPosition = texelPosition + neighborOffset;
            float centerAlpha = texelFetch(blockTextures, ivec3(texelPosition, layer), 0).a;
            float neighborAlpha = neighborPosition.x < 0 || neighborPosition.x >= 16 ||
                                  neighborPosition.y < 0 || neighborPosition.y >= 16
                ? 0.0
                : texelFetch(blockTextures, ivec3(neighborPosition, layer), 0).a;
            edgeVisible = centerAlpha >= 0.1 && neighborAlpha < 0.1 ? 1.0 : 0.0;

            float left = float(texelPosition.x) / 16.0 - 0.5;
            float right = float(texelPosition.x + 1) / 16.0 - 0.5;
            float top = 0.5 - float(texelPosition.y) / 16.0;
            float bottom = 0.5 - float(texelPosition.y + 1) / 16.0;
            float along = corner == 0 || corner == 3 ? 0.0 : 1.0;
            float depth = corner < 2 ? 0.03125 : -0.03125;
            if (edge == 0) {
                local = vec3(left, mix(bottom, top, along), depth);
                normal = vec3(-1.0, 0.0, 0.0);
            } else if (edge == 1) {
                local = vec3(right, mix(bottom, top, along), -depth);
                normal = vec3(1.0, 0.0, 0.0);
            } else if (edge == 2) {
                local = vec3(mix(left, right, along), top, -depth);
                normal = vec3(0.0, 1.0, 0.0);
            } else {
                local = vec3(mix(left, right, along), bottom, depth);
                normal = vec3(0.0, -1.0, 0.0);
            }
            uv = (vec2(texelPosition) + vec2(0.5)) / 16.0;
        }
        vec3 worldPosition = (item.viewModelTransform * vec4(local, 1.0)).xyz;
        gl_Position = camera.projection * vec4(worldPosition, 1.0);
        fragmentUv = uv;
        fragmentTextureLayer = item.textureLayersRotation.x;
        fragmentNormal = normalize(mat3(item.viewModelTransform) * normal);
        fragmentIsCube = 1.0;
        fragmentShadowOpacity = 0.0;
        fragmentOpacity = edgeVisible;
        fragmentCameraDistance = length(worldPosition);
        return;
    }
    if (item.data.x > 1.5 && item.data.x < 2.5) {
        int triangle = gl_VertexIndex / 3;
        int vertex = gl_VertexIndex % 3;
        float startAngle = float(triangle) * 6.28318530718 / 12.0;
        float endAngle = float(triangle + 1) * 6.28318530718 / 12.0;
        vec2 radial = vertex == 0
            ? vec2(0.0)
            : (vertex == 1
                ? vec2(cos(startAngle), sin(startAngle))
                : vec2(cos(endAngle), sin(endAngle)));
        vec3 worldPosition = item.positionSize.xyz +
            vec3(radial.x, 0.0, radial.y) * item.positionSize.w;
        gl_Position = camera.projection * camera.view * vec4(worldPosition, 1.0);
        fragmentUv = radial * 0.5 + vec2(0.5);
        fragmentTextureLayer = 0.0;
        fragmentNormal = vec3(0.0, 1.0, 0.0);
        fragmentIsCube = 2.0;
        fragmentShadowOpacity = item.data.y;
        fragmentOpacity = 1.0;
        fragmentCameraDistance = distance(worldPosition, camera.cameraPosition.xyz);
        return;
    }
    if ((item.data.x > 0.5 && item.data.x < 1.5) || item.data.x > 2.5) {
        bool matrixViewModel = item.data.x > 5.5 && item.data.x < 6.5;
        // World-space skinned cuboid: viewModelTransform is a world matrix, drawn
        // through the camera view with world-space normals (so the fixed light is
        // orientation-correct). Used for the chest lid and articulated mob bones.
        bool worldMatrixCuboid = item.data.x > 7.5 && item.data.x < 8.5;
        // Box-UV skinned entity cuboid (mobs/NPCs): a world matrix carries the
        // bone transform, dimensions.xyz is the drawn cube extent (size +
        // 2*inflate), positionSize.xyz the uninflated size the UV net is built
        // from, data.yz is the cube's UV net origin, data.w mirrors the net, and
        // textureLayersRotation.xyz holds the entity texture layer plus the
        // model's declared texture_width/height.
        bool boxUvEntity = item.data.x > 8.5 && item.data.x < 9.5;
        bool useMatrix = matrixViewModel || worldMatrixCuboid || boxUvEntity;
        bool heldInViewSpace =
            (item.data.x > 2.5 && item.data.x < 4.5) || matrixViewModel;
        bool articulatedWorldCuboid = item.data.x > 4.5 && item.data.x < 5.5;
        bool playerSkinCuboid =
            (item.data.x > 3.5 && item.data.x < 4.5) ||
            articulatedWorldCuboid ||
            worldMatrixCuboid ||
            (matrixViewModel && item.data.w > 0.5);
        int face = gl_VertexIndex / 6;
        int corner = quadIndices[gl_VertexIndex % 6];
        vec2 uv = faceUvs[corner];
        vec3 local;
        vec3 normal;
        if (face == 0) {
            const vec3 positions[4] = vec3[](
                vec3(0.5, -0.5, 0.5), vec3(0.5, -0.5, -0.5),
                vec3(0.5, 0.5, -0.5), vec3(0.5, 0.5, 0.5));
            local = positions[corner];
            normal = vec3(1.0, 0.0, 0.0);
        } else if (face == 1) {
            const vec3 positions[4] = vec3[](
                vec3(-0.5, -0.5, -0.5), vec3(-0.5, -0.5, 0.5),
                vec3(-0.5, 0.5, 0.5), vec3(-0.5, 0.5, -0.5));
            local = positions[corner];
            normal = vec3(-1.0, 0.0, 0.0);
        } else if (face == 2) {
            const vec3 positions[4] = vec3[](
                vec3(-0.5, 0.5, -0.5), vec3(-0.5, 0.5, 0.5),
                vec3(0.5, 0.5, 0.5), vec3(0.5, 0.5, -0.5));
            local = positions[corner];
            normal = vec3(0.0, 1.0, 0.0);
        } else if (face == 3) {
            const vec3 positions[4] = vec3[](
                vec3(-0.5, -0.5, 0.5), vec3(-0.5, -0.5, -0.5),
                vec3(0.5, -0.5, -0.5), vec3(0.5, -0.5, 0.5));
            local = positions[corner];
            normal = vec3(0.0, -1.0, 0.0);
        } else if (face == 4) {
            const vec3 positions[4] = vec3[](
                vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
                vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
            local = positions[corner];
            normal = vec3(0.0, 0.0, 1.0);
        } else {
            const vec3 positions[4] = vec3[](
                vec3(0.5, -0.5, -0.5), vec3(-0.5, -0.5, -0.5),
                vec3(-0.5, 0.5, -0.5), vec3(0.5, 0.5, -0.5));
            local = positions[corner];
            normal = vec3(0.0, 0.0, -1.0);
        }
        float yawCosine = cos(item.textureLayersRotation.w);
        float yawSine = sin(item.textureLayersRotation.w);
        mat3 yawRotation = mat3(
            yawCosine, 0.0, -yawSine,
            0.0, 1.0, 0.0,
            yawSine, 0.0, yawCosine);
        float pitchCosine = cos(item.data.y);
        float pitchSine = sin(item.data.y);
        mat3 pitchRotation = mat3(
            1.0, 0.0, 0.0,
            0.0, pitchCosine, pitchSine,
            0.0, -pitchSine, pitchCosine);
        float rollCosine = cos(item.data.z);
        float rollSine = sin(item.data.z);
        mat3 rollRotation = mat3(
            rollCosine, rollSine, 0.0,
            -rollSine, rollCosine, 0.0,
            0.0, 0.0, 1.0);
        mat3 rotation = useMatrix
            ? mat3(1.0)
            : ((heldInViewSpace || articulatedWorldCuboid)
                ? rollRotation * pitchRotation * yawRotation
                : yawRotation);
        vec3 dimensions = length(item.dimensions.xyz) > 0.0001
            ? item.dimensions.xyz
            : vec3(item.positionSize.w);
        // Scale the box in its own axes first, then rotate rigidly. Scaling an
        // already-rotated cuboid by non-uniform dimensions shears it (the chest
        // lid appeared to shrink as it pitched open); scaling first keeps the
        // box rigid. For the matrix paths rotation is identity and the world
        // matrix carries the orientation instead.
        local = rotation * (local * dimensions);
        normal = rotation * normal;
        vec3 worldPosition = useMatrix
            ? (item.viewModelTransform * vec4(local, 1.0)).xyz
            : item.positionSize.xyz + local;
        normal = useMatrix
            ? normalize(mat3(item.viewModelTransform) * normal)
            : normal;
        gl_Position = heldInViewSpace
            ? camera.projection * vec4(worldPosition, 1.0)
            : camera.projection * camera.view * vec4(worldPosition, 1.0);
        // Only the world-space paths produce a real world position; the held
        // item is in view space, so it never opts into the scene light.
        fragmentWorldPosition = heldInViewSpace ? vec3(0.0) : worldPosition;
        if (boxUvEntity) {
            // positionSize.w is unused by this path (the box extent lives in
            // dimensions.xyz), so it carries the hurt overlay strength.
            fragmentHurtFlash = item.positionSize.w;
            // Standard Minecraft box-UV net. This must stay identical to the
            // reference in tools/entity_uv_lib.py (mirrored by the texture
            // editor) and animation::boxUvFaceRect. Per face: pick the texel
            // rect from the *uninflated* size, then orient it from the
            // cube-fractional position frac = local/dimensions + 0.5 (rotation
            // is identity here, so local == unit * dimensions). Inflating a cube
            // stretches the same texels over a bigger box, it never shifts the net.
            float sx = item.positionSize.x;
            float sy = item.positionSize.y;
            float sz = item.positionSize.z;
            float u0 = item.data.y;
            float v0 = item.data.z;
            vec3 frac = local / dimensions + vec3(0.5);
            // The orientation (faceUv) always comes from the geometric face so the
            // texture reads correctly on it; the rect it samples comes from the
            // "faces" override (ModelCube::faceOverride, packed into
            // textureLayersRotation.w): 6 faces x (3-bit source rect, 1-bit rotate).
            // The identity layout — each face samples its own rect — is the classic
            // box-UV mapping.
            vec2 faceUv;
            if (face == 0)         faceUv = vec2(1.0 - frac.z, 1.0 - frac.y);  // +X east
            else if (face == 1)    faceUv = vec2(frac.z, 1.0 - frac.y);         // -X west
            else if (face == 2)    faceUv = vec2(frac.x, 1.0 - frac.z);         // +Y up
            else if (face == 3)    faceUv = vec2(frac.x, frac.z);               // -Y down
            else if (face == 4)    faceUv = vec2(frac.x, 1.0 - frac.y);         // +Z back
            else                   faceUv = vec2(1.0 - frac.x, 1.0 - frac.y);   // -Z front
            if (item.data.w > 0.5) {
                faceUv.x = 1.0 - faceUv.x;
            }
            uint packed = floatBitsToUint(item.textureLayersRotation.w);
            uint src = (packed >> uint(face * 4)) & 7u;
            if (((packed >> uint(face * 4 + 3)) & 1u) != 0u) {
                faceUv = vec2(1.0 - faceUv.x, 1.0 - faceUv.y);   // rotate 180
            }
            vec2 rectOrigin;
            vec2 rectSize;
            if (src == 0u)         { rectOrigin = vec2(u0 + sz + sx, v0 + sz); rectSize = vec2(sz, sy); }
            else if (src == 1u)    { rectOrigin = vec2(u0, v0 + sz); rectSize = vec2(sz, sy); }
            else if (src == 2u)    { rectOrigin = vec2(u0 + sz + sx, v0); rectSize = vec2(sx, sz); } // +Y up -> right cap
            else if (src == 3u)    { rectOrigin = vec2(u0 + sz, v0); rectSize = vec2(sx, sz); }      // -Y down -> left cap
            else if (src == 4u)    { rectOrigin = vec2(u0 + 2.0 * sz + sx, v0 + sz); rectSize = vec2(sx, sy); }
            else                   { rectOrigin = vec2(u0 + sz, v0 + sz); rectSize = vec2(sx, sy); }
            // Declared texture_width/height, not the atlas pixel size: texels are
            // normalised against the geometry's own coordinate space so a skin
            // authored at a higher resolution still lands on the same faces.
            vec2 textureSize = vec2(item.textureLayersRotation.y, item.textureLayersRotation.z);
            fragmentUv = (rectOrigin + faceUv * rectSize) / textureSize;
            fragmentTextureLayer = item.textureLayersRotation.x;
            fragmentEntityTexture = 1.0;
        } else {
            // The held empty hand renders the Java-model right arm, whose local
            // +Y is the HAND end (up-screen, fingers toward the camera), while
            // the skin-atlas faces are authored for the Bedrock/world +Y-up
            // convention (shoulder at the top of every face). Sampling the same
            // layer the world player uses would therefore paint the sleeve onto
            // the fingers. data.z flags the held arm and mirrors V so the sleeve
            // lands on the wrist and the skin on the hand, exactly as vanilla's
            // HeldItemRenderer lays the 40,16 box-UV net out.
            // A slab item is a half-height block: on this cube path data.z flags
            // it (it is the roll angle, unused here). Its four side faces show
            // only the lower half strip of the side texture — v in [0.5, 1] — the
            // way vanilla's slab model maps a 16x8 side, instead of the whole
            // texture squeezed into half height. Top and bottom faces keep full
            // UVs. The held arm keeps its own V mirror (it is playerSkinCuboid).
            vec2 cubeUv = uv;
            if (!playerSkinCuboid && item.data.z > 0.5 && face != 2 && face != 3) {
                cubeUv.y = 0.5 + cubeUv.y * 0.5;
            }
            fragmentUv = playerSkinCuboid && item.data.z > 0.5
                ? vec2(uv.x, 1.0 - uv.y)
                : cubeUv;
            fragmentTextureLayer = playerSkinCuboid
                ? item.textureLayersRotation.x + float(face)
                : (matrixViewModel && item.textureLayersRotation.w > 10.0 && face == 4
                    ? item.textureLayersRotation.w
                : (face == 2
                    ? item.textureLayersRotation.x
                    : (face == 3
                        ? item.textureLayersRotation.z
                        : item.textureLayersRotation.y)));
        }
        fragmentNormal = normal;
        fragmentFallingBlock =
            item.data.x > 0.5 && item.data.x < 1.5 && item.data.w > 1.5 ? 1.0 : 0.0;
        fragmentIsCube = 1.0;
        fragmentShadowOpacity = 0.0;
        fragmentOpacity = 1.0;
        fragmentCameraDistance = heldInViewSpace
            ? length(worldPosition)
            : distance(worldPosition, camera.cameraPosition.xyz);
        return;
    }
    vec2 corner = corners[gl_VertexIndex];
    bool heldBillboard = item.data.x < -1.5;
    bool matrixHeldBillboard = item.data.x < -2.5;
    vec3 cameraRight = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 cameraUp = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    float heldRoll = radians(-15.0);
    mat2 heldRotation = mat2(
        cos(heldRoll), sin(heldRoll),
        -sin(heldRoll), cos(heldRoll));
    vec2 heldCorner = heldRotation * corner;
    vec3 worldPosition = matrixHeldBillboard
        ? (item.viewModelTransform * vec4(corner, 0.0, 1.0)).xyz
        : heldBillboard
        ? item.positionSize.xyz + vec3(heldCorner, 0.0) * item.positionSize.w
        : item.positionSize.xyz +
            (cameraRight * corner.x + cameraUp * corner.y) * item.positionSize.w;
    gl_Position = heldBillboard
        ? camera.projection * vec4(worldPosition, 1.0)
        : camera.projection * camera.view * vec4(worldPosition, 1.0);
    // World-space billboards (the flat sprite of a dropped item) reach the moving
    // point lights too; the held billboard is in view space and never lit.
    fragmentWorldPosition = heldBillboard ? vec3(0.0) : worldPosition;
    vec2 baseUv = corner + vec2(0.5);
    fragmentUv = item.data.x < -0.5 && !heldBillboard
        ? item.data.yz + baseUv * item.data.w
        : vec2(baseUv.x, 1.0 - baseUv.y);
    fragmentTextureLayer = item.textureLayersRotation.x;
    fragmentNormal = heldBillboard
        ? (matrixHeldBillboard
            ? normalize(mat3(item.viewModelTransform) * vec3(0.0, 0.0, 1.0))
            : vec3(0.0, 0.0, 1.0))
        : normalize(camera.cameraPosition.xyz - item.positionSize.xyz);
    fragmentIsCube = 0.0;
    fragmentShadowOpacity = 0.0;
    fragmentOpacity = item.data.x < -0.5 && !heldBillboard
        ? item.textureLayersRotation.w
        : 1.0;
    fragmentCameraDistance = heldBillboard
        ? length(worldPosition)
        : distance(worldPosition, camera.cameraPosition.xyz);
}
