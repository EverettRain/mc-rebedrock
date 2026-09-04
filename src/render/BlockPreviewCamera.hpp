#pragma once

// RN-15a: where the camera stands to photograph one block.
//
// The eight viewpoints are the eight corner directions of a cube, so each frame
// shows three faces and the eight together show all six twice. That is what makes
// the export a usable check on "which face shows what": a six-face-per-frame
// orthographic sheet would hide exactly the thing this line keeps getting wrong.
//
// Everything here is pure arithmetic on a bounding box, deliberately: the export
// path cannot be run in this container (no GPU), so the part that CAN be tested
// headlessly is separated out and tested hard. `block_preview_camera_test` drives
// it through the real `PerspectiveCamera` and projects the block's corners, which
// is what makes "it fits in frame" an assertion rather than a hope.
//
// Determinism is a requirement, not a nicety (RN-15 §4): the same block in the
// same state must produce the same eight poses on every run, or every diff of two
// exports is noise. Nothing here reads a clock, a random source, or any global.

#include "world/BlockShape.hpp"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::render {

// The eight corner viewpoints, named for where the CAMERA stands relative to the
// block: compass bearing, then elevation. The order is fixed and is the order the
// exporter writes files in.
enum class PreviewCorner : std::uint8_t {
    NorthWestDown,
    NorthWestUp,
    NorthEastDown,
    NorthEastUp,
    SouthWestDown,
    SouthWestUp,
    SouthEastDown,
    SouthEastUp,
    Count,
};

inline constexpr std::size_t kPreviewCornerCount =
    static_cast<std::size_t>(PreviewCorner::Count);

// File-name stems. Stable: renaming one invalidates every stored baseline, which
// is the whole point of the tool.
inline constexpr std::array<std::string_view, kPreviewCornerCount> kPreviewCornerNames{
    "north-west-down", "north-west-up",   "north-east-down", "north-east-up",
    "south-west-down", "south-west-up",   "south-east-down", "south-east-up",
};

// The unit direction from the block toward the camera. World axes: +X east,
// +Y up, +Z south (north is -Z, matching BlockOrientation::North).
[[nodiscard]] constexpr glm::vec3 previewCornerDirection(PreviewCorner corner) {
    const auto index = static_cast<std::uint8_t>(corner);
    // Bit 0 = up, bit 1 = east, bit 2 = south — the enum order above.
    const float x = (index & 0b010U) != 0U ? 1.0F : -1.0F;
    const float y = (index & 0b001U) != 0U ? 1.0F : -1.0F;
    const float z = (index & 0b100U) != 0U ? 1.0F : -1.0F;
    return {x, y, z};
}

// An axis-aligned box in cell coordinates (0..1 within the block's own cell).
struct PreviewBounds final {
    glm::vec3 minimum{0.0F, 0.0F, 0.0F};
    glm::vec3 maximum{1.0F, 1.0F, 1.0F};

    [[nodiscard]] constexpr glm::vec3 centre() const {
        return {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                (minimum.z + maximum.z) * 0.5F};
    }
    [[nodiscard]] constexpr glm::vec3 extent() const {
        return {maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
    }
};

// The block's outline shape as one box. `blockShape` is 26.1's `getShape`, which
// is what the selection box traces, so it is the honest answer to "how big is
// this block" — a pressure plate is 1px tall there and gets a closer camera than
// a full cube, which is the adaptive part of RN-15a.
//
// An Empty shape (air, and a few blocks whose outline is genuinely nothing) falls
// back to the whole cell rather than collapsing to a point: a camera derived from
// a zero-size box is a division by zero waiting to happen, and "show me the whole
// cell" is the only sensible answer when the block declines to say how big it is.
[[nodiscard]] inline PreviewBounds previewBoundsOf(const world::BlockShape& shape) {
    switch (shape.kind) {
    case world::ShapeKind::Column:
        return {{0.0F, shape.bottom, 0.0F}, {1.0F, shape.top, 1.0F}};
    case world::ShapeKind::Boxes: {
        if (shape.boxes.empty()) {
            break;
        }
        PreviewBounds bounds{{shape.boxes[0].minX, shape.boxes[0].minY, shape.boxes[0].minZ},
                             {shape.boxes[0].maxX, shape.boxes[0].maxY, shape.boxes[0].maxZ}};
        for (const world::ShapeBox& box : shape.boxes) {
            bounds.minimum.x = std::min(bounds.minimum.x, box.minX);
            bounds.minimum.y = std::min(bounds.minimum.y, box.minY);
            bounds.minimum.z = std::min(bounds.minimum.z, box.minZ);
            bounds.maximum.x = std::max(bounds.maximum.x, box.maxX);
            bounds.maximum.y = std::max(bounds.maximum.y, box.maxY);
            bounds.maximum.z = std::max(bounds.maximum.z, box.maxZ);
        }
        return bounds;
    }
    case world::ShapeKind::Empty:
        break;
    }
    return {};
}

// How far a block's MODEL may stick out past its shape, in cell units.
//
// The two are not the same box and the gap is not hypothetical: a lit repeater's
// glow billboards run from -1.5 to 17.5 model units (ElementModelBaker's
// appendTorchHalo) while its shape is a 2px column, and a cross plant's quad
// spans the cell diagonal while its shape is a 0.1..0.9 box. Sizing the camera
// off the shape alone would crop them. 2/16 covers every overhang the transcribed
// models actually have; the camera is sized off the padded box, so "it fits" is a
// property of the construction rather than of the caller remembering a margin.
inline constexpr float kPreviewModelOverhang = 2.0F / 16.0F;

// How much of the frame's smaller half-axis the block's bounding sphere fills.
// The remaining margin is what keeps a block off the edge of the picture; it is
// deliberately generous, because a preview cropped at the silhouette is exactly
// the picture in which you cannot see whether the silhouette is right.
inline constexpr float kPreviewFillFraction = 0.75F;

struct PreviewCameraPose final {
    glm::vec3 eye{0.0F};
    float yawDegrees = 0.0F;
    float pitchDegrees = 0.0F;
    // Distance from the block's bounding-box centre. Reported so a test (and a
    // reader) can see the adaptive part without re-deriving it.
    float distance = 0.0F;
};

// Where to stand to photograph `bounds` (a box in the cell at `cellOrigin`) from
// `corner`.
//
// The block is enclosed in a bounding SPHERE and the distance solved so that
// sphere subtends `fillFraction` of the narrower half-FOV. A sphere rather than
// the box's silhouette because a sphere is view-independent: the eight frames of
// one block then share a scale, and two blocks of the same size photograph the
// same, which is what makes a stored baseline comparable.
[[nodiscard]] inline PreviewCameraPose previewCameraPose(
    const PreviewBounds& bounds, glm::vec3 cellOrigin, PreviewCorner corner,
    float verticalFieldOfViewDegrees, float aspectRatio,
    float fillFraction = kPreviewFillFraction, float modelOverhang = kPreviewModelOverhang) {
    const glm::vec3 padded = bounds.extent() + glm::vec3{2.0F * modelOverhang};
    // Half the diagonal of the padded box: the radius of the sphere that contains
    // it whatever way the camera looks at it.
    float radius = 0.5F * std::sqrt(padded.x * padded.x + padded.y * padded.y +
                                    padded.z * padded.z);
    // A degenerate shape must still yield a usable camera rather than a division
    // by zero; half a cell is the smallest thing worth photographing.
    radius = std::max(radius, 0.5F);

    const float halfVertical = std::tan(glm::radians(verticalFieldOfViewDegrees) * 0.5F);
    const float halfHorizontal = halfVertical * std::max(aspectRatio, 0.01F);
    const float limiting = std::min(halfVertical, halfHorizontal);
    const float safeFill = std::clamp(fillFraction, 0.05F, 1.0F);

    PreviewCameraPose pose;
    pose.distance = radius / (safeFill * std::max(limiting, 0.01F));
    // Never inside the sphere: a camera closer than the radius is looking at the
    // block from within it, and the near plane would slice the model open.
    pose.distance = std::max(pose.distance, radius + 0.5F);

    const glm::vec3 centre = cellOrigin + bounds.centre();
    const glm::vec3 direction = glm::normalize(previewCornerDirection(corner));
    pose.eye = centre + direction * pose.distance;
    // Look back at the centre. PerspectiveCamera's forward is
    // (cos yaw cos pitch, sin pitch, sin yaw cos pitch), so this is its inverse.
    const glm::vec3 look = -direction;
    pose.yawDegrees = glm::degrees(std::atan2(look.z, look.x));
    pose.pitchDegrees = glm::degrees(std::asin(std::clamp(look.y, -1.0F, 1.0F)));
    return pose;
}

// The convenience form: a block state's shape, a cell, a corner.
[[nodiscard]] inline PreviewCameraPose previewCameraPose(
    world::BlockState state, glm::vec3 cellOrigin, PreviewCorner corner,
    float verticalFieldOfViewDegrees, float aspectRatio) {
    return previewCameraPose(previewBoundsOf(world::blockShape(state)), cellOrigin, corner,
                             verticalFieldOfViewDegrees, aspectRatio);
}

} // namespace mc::render
