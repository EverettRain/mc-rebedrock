// RN-15a: the eight preview viewpoints, and the only part of the block-preview
// export that can be verified without a GPU.
//
// The export itself renders through the real game path and writes PNGs; this
// container has no GPU, so that half is verified by the user on a machine that
// does. What is verifiable here is the arithmetic that decides where the camera
// stands — and it is where the interesting mistakes live: a viewpoint that shows
// two faces instead of three, a thin block cropped at the frame edge, a camera
// inside the block, or a pose that comes out different on the second run and
// makes every image diff red.
//
// The projection assertions go through the REAL PerspectiveCamera rather than a
// re-derivation, because half of what is being checked is that the pose's
// yaw/pitch match that camera's forward convention.

#include "render/BlockPreviewCamera.hpp"
#include "render/PerspectiveCamera.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <set>
#include <string_view>

namespace {

using namespace mc::render;
using mc::world::Block;
using mc::world::BlockState;
namespace world = mc::world;

constexpr float kFov = 70.0F;
constexpr float kAspect = 1.0F;
constexpr float kFar = 100.0F;

// The camera the exporter actually configures from a pose.
[[nodiscard]] PerspectiveCamera cameraFor(const PreviewCameraPose& pose) {
    PerspectiveCamera camera{pose.eye, pose.eye + glm::vec3{0.0F, 0.0F, 1.0F}, kFov};
    camera.setPosition(pose.eye);
    camera.setRotation(pose.yawDegrees, pose.pitchDegrees);
    return camera;
}

// Where a world point lands in normalised device coordinates. Returns false when
// the point is behind the camera, which is itself a failure for a preview.
[[nodiscard]] bool projectPoint(const PerspectiveCamera& camera, const glm::vec3& world,
                                glm::vec3& ndc) {
    const glm::mat4 viewProjection = camera.projectionMatrix(kAspect, kFar) * camera.viewMatrix();
    const glm::vec4 clip = viewProjection * glm::vec4{world, 1.0F};
    if (clip.w <= 0.0F) {
        return false;
    }
    ndc = glm::vec3{clip} / clip.w;
    return true;
}

// The eight corners of a box, in world space.
[[nodiscard]] std::array<glm::vec3, 8> boxCorners(const PreviewBounds& bounds,
                                                  glm::vec3 cellOrigin) {
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        corners[i] = cellOrigin + glm::vec3{(i & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
                                            (i & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
                                            (i & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
    }
    return corners;
}

// Every corner of `bounds` is inside the frame, with `margin` of NDC to spare.
void assertFitsInFrame(const PreviewBounds& bounds, glm::vec3 cellOrigin,
                       const PreviewCameraPose& pose, float margin, std::string_view what) {
    const auto camera = cameraFor(pose);
    for (const glm::vec3& corner : boxCorners(bounds, cellOrigin)) {
        glm::vec3 ndc{};
        assert(projectPoint(camera, corner, ndc) && "a block corner is behind the preview camera");
        assert(std::fabs(ndc.x) <= 1.0F - margin && "a block corner falls outside the frame");
        assert(std::fabs(ndc.y) <= 1.0F - margin && "a block corner falls outside the frame");
        assert(ndc.z > 0.0F && ndc.z < 1.0F && "a block corner falls outside the depth range");
        static_cast<void>(what);
    }
}

[[nodiscard]] PreviewBounds paddedBounds(const PreviewBounds& bounds) {
    return {bounds.minimum - glm::vec3{kPreviewModelOverhang},
            bounds.maximum + glm::vec3{kPreviewModelOverhang}};
}

} // namespace

int main() {
    constexpr glm::vec3 kCell{8.0F, 64.0F, 8.0F};

    // --- The eight directions are eight DISTINCT corners of a cube, and every one
    //     shows three faces. A direction with a zero component would look straight
    //     at a face and show one or two, which is the whole reason there are eight
    //     of them rather than six. ---
    {
        std::set<std::array<int, 3>> seen;
        for (std::size_t i = 0; i < kPreviewCornerCount; ++i) {
            const glm::vec3 d = previewCornerDirection(static_cast<PreviewCorner>(i));
            assert(std::fabs(d.x) == 1.0F && std::fabs(d.y) == 1.0F && std::fabs(d.z) == 1.0F);
            const std::array<int, 3> key{static_cast<int>(d.x), static_cast<int>(d.y),
                                         static_cast<int>(d.z)};
            assert(seen.insert(key).second && "two viewpoints share a direction");
        }
        assert(seen.size() == 8U);
        // And the names are eight distinct, stable file stems.
        std::set<std::string_view> names;
        for (const std::string_view name : kPreviewCornerNames) {
            assert(!name.empty());
            assert(names.insert(name).second);
        }
        assert(names.size() == kPreviewCornerCount);
        // The name says where the camera is: "north" means -Z, "up" means +Y.
        for (std::size_t i = 0; i < kPreviewCornerCount; ++i) {
            const glm::vec3 d = previewCornerDirection(static_cast<PreviewCorner>(i));
            const std::string_view name = kPreviewCornerNames[i];
            assert((name.find("north") != std::string_view::npos) == (d.z < 0.0F));
            assert((name.find("south") != std::string_view::npos) == (d.z > 0.0F));
            assert((name.find("west") != std::string_view::npos) == (d.x < 0.0F));
            assert((name.find("east") != std::string_view::npos) == (d.x > 0.0F));
            assert((name.find("-up") != std::string_view::npos) == (d.y > 0.0F));
            assert((name.find("-down") != std::string_view::npos) == (d.y < 0.0F));
        }
    }

    // --- The bounding box comes from the block's own shape. A pressure plate is
    //     1px tall there and a full cube fills its cell; that difference is what
    //     "adaptive" means. ---
    {
        const auto cube = previewBoundsOf(world::blockShape(BlockState{Block::Stone}));
        assert(cube.minimum.y == 0.0F && cube.maximum.y == 1.0F);
        assert(cube.extent().x == 1.0F && cube.extent().z == 1.0F);

        const auto plate =
            previewBoundsOf(world::blockShape(BlockState{Block::StonePressurePlate}));
        assert(plate.extent().y < 0.2F && "a pressure plate is a thin slab");
        assert(plate.extent().y > 0.0F);

        const auto slab = previewBoundsOf(world::blockShape(BlockState{Block::OakSlab}));
        assert(std::fabs(slab.extent().y - 0.5F) < 1.0e-4F);

        // A shape that is Empty must not collapse to a point — that is a division
        // by zero one refactor away, and "the whole cell" is the only sensible
        // answer when the block declines to say how big it is.
        const auto empty = previewBoundsOf(world::BlockShape{});
        assert(empty.extent().x == 1.0F && empty.extent().y == 1.0F && empty.extent().z == 1.0F);
    }

    // --- Every viewpoint of every shape in the roster keeps the block in frame,
    //     model overhang included, and keeps the camera outside it. This is the
    //     assertion the whole header exists to satisfy. ---
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
            const auto block = static_cast<Block>(i);
            if (!world::isRenderable(block)) {
                continue;
            }
            const BlockState state{block};
            const auto bounds = previewBoundsOf(world::blockShape(state));
            for (std::size_t c = 0; c < kPreviewCornerCount; ++c) {
                const auto corner = static_cast<PreviewCorner>(c);
                const auto pose = previewCameraPose(state, kCell, corner, kFov, kAspect);
                // The padded box — shape plus the overhang a model may add — fits
                // with 5% of the frame to spare on every side.
                assertFitsInFrame(paddedBounds(bounds), kCell, pose, 0.05F,
                                  world::blockDefinition(block).identifier.path);
                // And the camera stands outside the block, not inside it.
                const glm::vec3 centre = kCell + bounds.centre();
                assert(glm::length(pose.eye - centre) > 0.9F);
            }
        }
    }

    // --- The adaptive part, stated as a comparison: a thin block is photographed
    //     from closer than a full cube, and a half slab from between the two. A
    //     fixed distance would pass every "fits in frame" assertion above while
    //     making a pressure plate four pixels tall. ---
    {
        const auto poseFor = [&](Block block) {
            return previewCameraPose(BlockState{block}, kCell, PreviewCorner::SouthEastUp, kFov,
                                     kAspect);
        };
        const float cube = poseFor(Block::Stone).distance;
        const float slab = poseFor(Block::OakSlab).distance;
        const float plate = poseFor(Block::StonePressurePlate).distance;
        assert(plate < slab && slab < cube);
        // Not merely different: enough to matter. A plate at the cube's distance
        // would be a sliver.
        assert(plate < cube * 0.9F);
    }

    // --- Determinism. The same block in the same state must produce the same
    //     eight poses on every call, bit for bit — an export whose camera drifts
    //     makes every diff of two runs red, and the tool is worthless the day
    //     that happens. ---
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
            const auto block = static_cast<Block>(i);
            if (!world::isRenderable(block)) {
                continue;
            }
            for (std::size_t c = 0; c < kPreviewCornerCount; ++c) {
                const auto corner = static_cast<PreviewCorner>(c);
                const auto first = previewCameraPose(BlockState{block}, kCell, corner, kFov,
                                                     kAspect);
                const auto second = previewCameraPose(BlockState{block}, kCell, corner, kFov,
                                                      kAspect);
                assert(first.eye.x == second.eye.x && first.eye.y == second.eye.y &&
                       first.eye.z == second.eye.z);
                assert(first.yawDegrees == second.yawDegrees);
                assert(first.pitchDegrees == second.pitchDegrees);
                assert(first.distance == second.distance);
            }
        }
    }

    // --- The state matters, not just the block: an open trapdoor is a vertical
    //     plate where a closed one is a floor slab, so the two must not photograph
    //     the same. If they did, the export could never show a state difference. ---
    {
        const BlockState closed{Block::OakTrapdoor};
        const BlockState open = BlockState{Block::OakTrapdoor}.withOpen(true);
        const auto closedBounds = previewBoundsOf(world::blockShape(closed));
        const auto openBounds = previewBoundsOf(world::blockShape(open));
        assert(closedBounds.extent().y != openBounds.extent().y);
        const auto closedPose =
            previewCameraPose(closed, kCell, PreviewCorner::SouthEastUp, kFov, kAspect);
        const auto openPose =
            previewCameraPose(open, kCell, PreviewCorner::SouthEastUp, kFov, kAspect);
        assert(closedPose.eye != openPose.eye);
    }

    // --- The pose points AT the block. Derived from yaw/pitch through the real
    //     camera, so a sign error in the derivation shows up here rather than as a
    //     picture of the sky. ---
    {
        for (std::size_t c = 0; c < kPreviewCornerCount; ++c) {
            const auto corner = static_cast<PreviewCorner>(c);
            const auto pose = previewCameraPose(BlockState{Block::Stone}, kCell, corner, kFov,
                                                kAspect);
            const auto camera = cameraFor(pose);
            const glm::vec3 centre = kCell + glm::vec3{0.5F};
            const glm::vec3 toBlock = glm::normalize(centre - pose.eye);
            assert(glm::dot(camera.direction(), toBlock) > 0.999F);
            // Three faces visible: the view direction has a non-zero component on
            // every axis, so no face is edge-on.
            assert(std::fabs(camera.direction().x) > 0.1F);
            assert(std::fabs(camera.direction().y) > 0.1F);
            assert(std::fabs(camera.direction().z) > 0.1F);
        }
    }

    // --- A non-square frame still fits the block. The exporter uses a square
    //     window, but nothing stops a caller passing the game's aspect, and the
    //     limiting half-axis is the narrower one. ---
    {
        for (const float aspect : {0.5F, 1.0F, 16.0F / 9.0F}) {
            const auto bounds = previewBoundsOf(world::blockShape(BlockState{Block::Stone}));
            const auto pose = previewCameraPose(BlockState{Block::Stone}, kCell,
                                                PreviewCorner::NorthWestUp, kFov, aspect);
            const auto camera = cameraFor(pose);
            for (const glm::vec3& point : boxCorners(paddedBounds(bounds), kCell)) {
                const glm::mat4 viewProjection =
                    camera.projectionMatrix(aspect, kFar) * camera.viewMatrix();
                const glm::vec4 clip = viewProjection * glm::vec4{point, 1.0F};
                assert(clip.w > 0.0F);
                const glm::vec3 ndc = glm::vec3{clip} / clip.w;
                assert(std::fabs(ndc.x) <= 0.95F && std::fabs(ndc.y) <= 0.95F);
            }
        }
    }

    return 0;
}
