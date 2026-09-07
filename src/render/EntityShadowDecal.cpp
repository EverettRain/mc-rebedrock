#include "render/EntityShadowDecal.hpp"

#include <algorithm>

namespace mc::render {

float lightmapBrightness(int level, float ambientLight) {
    const float v = static_cast<float>(level) / 15.0F;
    const float curved = v / (4.0F - 3.0F * v);
    // Mth.lerp(delta, from, to) 是 from + delta * (to - from)，delta 在**第一位**。
    // Lightmap 传的是 lerp(ambientLight, curved, 1.0)，所以插值系数是环境光。
    return curved + ambientLight * (1.0F - curved);
}

int maxLocalRawBrightness(int skyLight, int blockLight, int skyDarken) {
    return std::max(blockLight, skyLight - skyDarken);
}

float entityShadowPower(const EntityShadowInput& entity) {
    // 256 = 16²。距离是相机到实体的，不是影子到实体的：一个走远的生物，它的影子在
    // 16 格处整片消失，而不是缩小。
    return (1.0F - entity.distanceToCameraSq / 256.0F) * entity.strength;
}

float entityShadowDepth(float power, float radius) {
    return std::min(power / 0.5F - 1.0F, radius);
}

EntityShadowPieceUv entityShadowPieceUv(const EntityShadowPiece& piece, float radius) {
    const float scale = 1.0F / (2.0F * radius);
    return EntityShadowPieceUv{
        -piece.relativeX * scale + 0.5F,
        -piece.relativeZ * scale + 0.5F,
        -(piece.relativeX + piece.sizeX) * scale + 0.5F,
        -(piece.relativeZ + piece.sizeZ) * scale + 0.5F,
    };
}

float entityShadowPieceAlpha(float power, float entityY, int pieceY, int brightness) {
    const float powerAtDepth = power - (entityY - static_cast<float>(pieceY)) * 0.5F;
    return std::clamp(powerAtDepth * 0.5F * lightmapBrightness(brightness), 0.0F, 1.0F);
}

} // namespace mc::render
