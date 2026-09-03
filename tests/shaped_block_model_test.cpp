// RN-10a: the door / trapdoor / fence-gate model descriptions, locked against a
// SECOND, INDEPENDENT transcription of the same vanilla json.
//
// Why the second copy exists, spelled out because it is the whole point of this
// file: element_model_baker_test's oracle re-derives *geometry* independently
// (boxFaceCorners, not kFaceInfo) but reads its UV rects from `elementsFor` —
// the very table under test. That is a self-certifying check: a wrong rect in
// the description is a wrong rect in the oracle, and no sabotage of the data can
// fail it. RN-10b/10c/10d all build on these rects, so a silent error here would
// propagate into every one of them.
//
// The golden table below is therefore emitted straight from the vanilla model
// files
//
//   /workspace/mc-26.1-java/Resourcepack Convert/vanilla-26.1/
//       assets/minecraft/models/block/{door_*,template_trapdoor_*,template_fence_gate*}.json
//
// as flat literals — one entry per json face, in json order, carrying the raw
// `uv`, `rotation` and `cullface`. It shares no code and no rule with
// ElementModelBaker's transcription, which compresses the same data through
// shared rects, a hinge-mirror predicate and an `in_wall` Y offset. Any of those
// compressions being wrong shows up here.
//
// What this file does NOT check is what a texture looks like on screen — that is
// mac-eyes-only (RN-10-MAC-VERIFICATION-CHECKLIST.md). What it can check without
// a screenshot is that the *numbers* are vanilla's, plus one real orientation
// anchor: for the door and the trapdoor the model box IS the BlockShape box, so
// the baked model's bounds must equal `blockShape`'s for all 48 variants. That
// pins the yaw convention (engine yaw = 360 - vanilla `"y"`) against a table that
// was verified independently in the AR-B2/B3 audit.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/ElementModelBaker.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

using namespace mc::world::bake;
using mc::world::Block;
using mc::world::BlockModel;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::DoorHinge;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "shaped block model: %s\n", what);
        std::abort();
    }
}

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-4F; }

// --- the golden table: literals emitted from the vanilla json ----------------

struct GoldenFace final {
    const char* face;
    float u0, v0, u1, v1;
    int rotation;
    const char* cullface;
};
struct GoldenBox final {
    std::array<float, 3> from;
    std::array<float, 3> to;
    std::vector<GoldenFace> faces;
};
struct GoldenModel final {
    std::vector<GoldenBox> boxes;
};

// door_bottom_left.json
const GoldenModel kGoldenDoorBottomLeft{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"down", 16.0F, 13.0F, 0.0F, 16.0F, 90, "down"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 0.0F, 0.0F, 16.0F, 16.0F, 0, "west"},
        {"east", 16.0F, 0.0F, 0.0F, 16.0F, 0, ""},
    }},
}};

// door_bottom_right.json
const GoldenModel kGoldenDoorBottomRight{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"down", 0.0F, 13.0F, 16.0F, 16.0F, 90, "down"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 16.0F, 0.0F, 0.0F, 16.0F, 0, "west"},
        {"east", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
    }},
}};

// door_bottom_left_open.json
const GoldenModel kGoldenDoorBottomLeftOpen{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"down", 0.0F, 16.0F, 16.0F, 13.0F, 90, "down"},
        {"north", 0.0F, 0.0F, 3.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 16.0F, 0.0F, 0.0F, 16.0F, 0, "west"},
        {"east", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
    }},
}};

// door_bottom_right_open.json
const GoldenModel kGoldenDoorBottomRightOpen{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"down", 16.0F, 16.0F, 0.0F, 13.0F, 90, "down"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 3.0F, 0.0F, 0.0F, 16.0F, 0, "south"},
        {"west", 0.0F, 0.0F, 16.0F, 16.0F, 0, "west"},
        {"east", 16.0F, 0.0F, 0.0F, 16.0F, 0, ""},
    }},
}};

// door_top_left.json
const GoldenModel kGoldenDoorTopLeft{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"up", 0.0F, 3.0F, 16.0F, 0.0F, 90, "up"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 0.0F, 0.0F, 16.0F, 16.0F, 0, "west"},
        {"east", 16.0F, 0.0F, 0.0F, 16.0F, 0, ""},
    }},
}};

// door_top_right.json
const GoldenModel kGoldenDoorTopRight{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"up", 0.0F, 0.0F, 16.0F, 3.0F, 270, "up"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 16.0F, 0.0F, 0.0F, 16.0F, 0, "west"},
        {"east", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
    }},
}};

// door_top_left_open.json
const GoldenModel kGoldenDoorTopLeftOpen{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"up", 0.0F, 3.0F, 16.0F, 0.0F, 270, "up"},
        {"north", 0.0F, 0.0F, 3.0F, 16.0F, 0, "north"},
        {"south", 0.0F, 0.0F, 3.0F, 16.0F, 0, "south"},
        {"west", 16.0F, 0.0F, 0.0F, 16.0F, 0, "west"},
        {"east", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
    }},
}};

// door_top_right_open.json
const GoldenModel kGoldenDoorTopRightOpen{{
    {{0.0F, 0.0F, 0.0F}, {3.0F, 16.0F, 16.0F}, {
        {"up", 0.0F, 0.0F, 16.0F, 3.0F, 90, "up"},
        {"north", 3.0F, 0.0F, 0.0F, 16.0F, 0, "north"},
        {"south", 3.0F, 0.0F, 0.0F, 16.0F, 0, "south"},
        {"west", 0.0F, 0.0F, 16.0F, 16.0F, 0, "west"},
        {"east", 16.0F, 0.0F, 0.0F, 16.0F, 0, ""},
    }},
}};

// template_trapdoor_bottom.json
const GoldenModel kGoldenTrapdoorBottom{{
    {{0.0F, 0.0F, 0.0F}, {16.0F, 3.0F, 16.0F}, {
        {"down", 0.0F, 0.0F, 16.0F, 16.0F, 0, "down"},
        {"up", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
        {"north", 0.0F, 16.0F, 16.0F, 13.0F, 0, "north"},
        {"south", 0.0F, 16.0F, 16.0F, 13.0F, 0, "south"},
        {"west", 0.0F, 16.0F, 16.0F, 13.0F, 0, "west"},
        {"east", 0.0F, 16.0F, 16.0F, 13.0F, 0, "east"},
    }},
}};

// template_trapdoor_top.json
const GoldenModel kGoldenTrapdoorTop{{
    {{0.0F, 13.0F, 0.0F}, {16.0F, 16.0F, 16.0F}, {
        {"down", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
        {"up", 0.0F, 0.0F, 16.0F, 16.0F, 0, "up"},
        {"north", 0.0F, 16.0F, 16.0F, 13.0F, 0, "north"},
        {"south", 0.0F, 16.0F, 16.0F, 13.0F, 0, "south"},
        {"west", 0.0F, 16.0F, 16.0F, 13.0F, 0, "west"},
        {"east", 0.0F, 16.0F, 16.0F, 13.0F, 0, "east"},
    }},
}};

// template_trapdoor_open.json
const GoldenModel kGoldenTrapdoorOpen{{
    {{0.0F, 0.0F, 13.0F}, {16.0F, 16.0F, 16.0F}, {
        {"down", 0.0F, 13.0F, 16.0F, 16.0F, 0, "down"},
        {"up", 0.0F, 16.0F, 16.0F, 13.0F, 0, "up"},
        {"north", 0.0F, 0.0F, 16.0F, 16.0F, 0, ""},
        {"south", 0.0F, 0.0F, 16.0F, 16.0F, 0, "south"},
        {"west", 16.0F, 0.0F, 13.0F, 16.0F, 0, "west"},
        {"east", 13.0F, 0.0F, 16.0F, 16.0F, 0, "east"},
    }},
}};

// template_fence_gate.json
const GoldenModel kGoldenGateClosed{{
    {{0.0F, 5.0F, 7.0F}, {2.0F, 16.0F, 9.0F}, {
        {"down", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"up", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"north", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"south", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, "west"},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
    }},
    {{14.0F, 5.0F, 7.0F}, {16.0F, 16.0F, 9.0F}, {
        {"down", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"up", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"north", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"south", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, "east"},
    }},
    {{6.0F, 6.0F, 7.0F}, {8.0F, 15.0F, 9.0F}, {
        {"down", 6.0F, 7.0F, 8.0F, 9.0F, 0, ""},
        {"up", 6.0F, 7.0F, 8.0F, 9.0F, 0, ""},
        {"north", 6.0F, 1.0F, 8.0F, 10.0F, 0, ""},
        {"south", 6.0F, 1.0F, 8.0F, 10.0F, 0, ""},
        {"west", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
        {"east", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
    }},
    {{8.0F, 6.0F, 7.0F}, {10.0F, 15.0F, 9.0F}, {
        {"down", 8.0F, 7.0F, 10.0F, 9.0F, 0, ""},
        {"up", 8.0F, 7.0F, 10.0F, 9.0F, 0, ""},
        {"north", 8.0F, 1.0F, 10.0F, 10.0F, 0, ""},
        {"south", 8.0F, 1.0F, 10.0F, 10.0F, 0, ""},
        {"west", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
        {"east", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
    }},
    {{2.0F, 6.0F, 7.0F}, {6.0F, 9.0F, 9.0F}, {
        {"down", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"up", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"north", 2.0F, 7.0F, 6.0F, 10.0F, 0, ""},
        {"south", 2.0F, 7.0F, 6.0F, 10.0F, 0, ""},
    }},
    {{2.0F, 12.0F, 7.0F}, {6.0F, 15.0F, 9.0F}, {
        {"down", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"up", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"north", 2.0F, 1.0F, 6.0F, 4.0F, 0, ""},
        {"south", 2.0F, 1.0F, 6.0F, 4.0F, 0, ""},
    }},
    {{10.0F, 6.0F, 7.0F}, {14.0F, 9.0F, 9.0F}, {
        {"down", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"up", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"north", 10.0F, 7.0F, 14.0F, 10.0F, 0, ""},
        {"south", 10.0F, 7.0F, 14.0F, 10.0F, 0, ""},
    }},
    {{10.0F, 12.0F, 7.0F}, {14.0F, 15.0F, 9.0F}, {
        {"down", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"up", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"north", 10.0F, 1.0F, 14.0F, 4.0F, 0, ""},
        {"south", 10.0F, 1.0F, 14.0F, 4.0F, 0, ""},
    }},
}};

// template_fence_gate_open.json
const GoldenModel kGoldenGateOpen{{
    {{0.0F, 5.0F, 7.0F}, {2.0F, 16.0F, 9.0F}, {
        {"down", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"up", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"north", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"south", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, "west"},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
    }},
    {{14.0F, 5.0F, 7.0F}, {16.0F, 16.0F, 9.0F}, {
        {"down", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"up", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"north", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"south", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, "east"},
    }},
    {{0.0F, 6.0F, 13.0F}, {2.0F, 15.0F, 15.0F}, {
        {"down", 0.0F, 13.0F, 2.0F, 15.0F, 0, ""},
        {"up", 0.0F, 13.0F, 2.0F, 15.0F, 0, ""},
        {"north", 0.0F, 1.0F, 2.0F, 10.0F, 0, ""},
        {"south", 0.0F, 1.0F, 2.0F, 10.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{14.0F, 6.0F, 13.0F}, {16.0F, 15.0F, 15.0F}, {
        {"down", 14.0F, 13.0F, 16.0F, 15.0F, 0, ""},
        {"up", 14.0F, 13.0F, 16.0F, 15.0F, 0, ""},
        {"north", 14.0F, 1.0F, 16.0F, 10.0F, 0, ""},
        {"south", 14.0F, 1.0F, 16.0F, 10.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{0.0F, 6.0F, 9.0F}, {2.0F, 9.0F, 13.0F}, {
        {"down", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"up", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"west", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{0.0F, 12.0F, 9.0F}, {2.0F, 15.0F, 13.0F}, {
        {"down", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"up", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
    }},
    {{14.0F, 6.0F, 9.0F}, {16.0F, 9.0F, 13.0F}, {
        {"down", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"up", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"west", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{14.0F, 12.0F, 9.0F}, {16.0F, 15.0F, 13.0F}, {
        {"down", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"up", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
    }},
}};

// template_fence_gate_wall.json
const GoldenModel kGoldenGateWallClosed{{
    {{0.0F, 2.0F, 7.0F}, {2.0F, 13.0F, 9.0F}, {
        {"down", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"up", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"north", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"south", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, "west"},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
    }},
    {{14.0F, 2.0F, 7.0F}, {16.0F, 13.0F, 9.0F}, {
        {"down", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"up", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"north", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"south", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, "east"},
    }},
    {{6.0F, 3.0F, 7.0F}, {8.0F, 12.0F, 9.0F}, {
        {"down", 6.0F, 7.0F, 8.0F, 9.0F, 0, ""},
        {"up", 6.0F, 7.0F, 8.0F, 9.0F, 0, ""},
        {"north", 6.0F, 1.0F, 8.0F, 10.0F, 0, ""},
        {"south", 6.0F, 1.0F, 8.0F, 10.0F, 0, ""},
        {"west", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
        {"east", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
    }},
    {{8.0F, 3.0F, 7.0F}, {10.0F, 12.0F, 9.0F}, {
        {"down", 8.0F, 7.0F, 10.0F, 9.0F, 0, ""},
        {"up", 8.0F, 7.0F, 10.0F, 9.0F, 0, ""},
        {"north", 8.0F, 1.0F, 10.0F, 10.0F, 0, ""},
        {"south", 8.0F, 1.0F, 10.0F, 10.0F, 0, ""},
        {"west", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
        {"east", 7.0F, 1.0F, 9.0F, 10.0F, 0, ""},
    }},
    {{2.0F, 3.0F, 7.0F}, {6.0F, 6.0F, 9.0F}, {
        {"down", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"up", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"north", 2.0F, 7.0F, 6.0F, 10.0F, 0, ""},
        {"south", 2.0F, 7.0F, 6.0F, 10.0F, 0, ""},
    }},
    {{2.0F, 9.0F, 7.0F}, {6.0F, 12.0F, 9.0F}, {
        {"down", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"up", 2.0F, 7.0F, 6.0F, 9.0F, 0, ""},
        {"north", 2.0F, 1.0F, 6.0F, 4.0F, 0, ""},
        {"south", 2.0F, 1.0F, 6.0F, 4.0F, 0, ""},
    }},
    {{10.0F, 3.0F, 7.0F}, {14.0F, 6.0F, 9.0F}, {
        {"down", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"up", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"north", 10.0F, 7.0F, 14.0F, 10.0F, 0, ""},
        {"south", 10.0F, 7.0F, 14.0F, 10.0F, 0, ""},
    }},
    {{10.0F, 9.0F, 7.0F}, {14.0F, 12.0F, 9.0F}, {
        {"down", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"up", 10.0F, 7.0F, 14.0F, 9.0F, 0, ""},
        {"north", 10.0F, 1.0F, 14.0F, 4.0F, 0, ""},
        {"south", 10.0F, 1.0F, 14.0F, 4.0F, 0, ""},
    }},
}};

// template_fence_gate_wall_open.json
const GoldenModel kGoldenGateWallOpen{{
    {{0.0F, 2.0F, 7.0F}, {2.0F, 13.0F, 9.0F}, {
        {"down", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"up", 0.0F, 7.0F, 2.0F, 9.0F, 0, ""},
        {"north", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"south", 0.0F, 0.0F, 2.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, "west"},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
    }},
    {{14.0F, 2.0F, 7.0F}, {16.0F, 13.0F, 9.0F}, {
        {"down", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"up", 14.0F, 7.0F, 16.0F, 9.0F, 0, ""},
        {"north", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"south", 14.0F, 0.0F, 16.0F, 11.0F, 0, ""},
        {"west", 7.0F, 0.0F, 9.0F, 11.0F, 0, ""},
        {"east", 7.0F, 0.0F, 9.0F, 11.0F, 0, "east"},
    }},
    {{0.0F, 3.0F, 13.0F}, {2.0F, 12.0F, 15.0F}, {
        {"down", 0.0F, 13.0F, 2.0F, 15.0F, 0, ""},
        {"up", 0.0F, 13.0F, 2.0F, 15.0F, 0, ""},
        {"north", 0.0F, 1.0F, 2.0F, 10.0F, 0, ""},
        {"south", 0.0F, 1.0F, 2.0F, 10.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{14.0F, 3.0F, 13.0F}, {16.0F, 12.0F, 15.0F}, {
        {"down", 14.0F, 13.0F, 16.0F, 15.0F, 0, ""},
        {"up", 14.0F, 13.0F, 16.0F, 15.0F, 0, ""},
        {"north", 14.0F, 1.0F, 16.0F, 10.0F, 0, ""},
        {"south", 14.0F, 1.0F, 16.0F, 10.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{0.0F, 3.0F, 9.0F}, {2.0F, 6.0F, 13.0F}, {
        {"down", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"up", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"west", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{0.0F, 9.0F, 9.0F}, {2.0F, 12.0F, 13.0F}, {
        {"down", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"up", 0.0F, 9.0F, 2.0F, 13.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
    }},
    {{14.0F, 3.0F, 9.0F}, {16.0F, 6.0F, 13.0F}, {
        {"down", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"up", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"west", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
        {"east", 13.0F, 7.0F, 15.0F, 10.0F, 0, ""},
    }},
    {{14.0F, 9.0F, 9.0F}, {16.0F, 12.0F, 13.0F}, {
        {"down", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"up", 14.0F, 9.0F, 16.0F, 13.0F, 0, ""},
        {"west", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
        {"east", 13.0F, 1.0F, 15.0F, 4.0F, 0, ""},
    }},
}};

// --- comparison ---------------------------------------------------------------

[[nodiscard]] Facing facingOfName(std::string_view name) {
    if (name == "down") return Facing::Down;
    if (name == "up") return Facing::Up;
    if (name == "north") return Facing::North;
    if (name == "south") return Facing::South;
    if (name == "west") return Facing::West;
    require(name == "east", "unknown face name in the golden table");
    return Facing::East;
}

[[nodiscard]] std::uint8_t cullOfName(std::string_view name) {
    if (name.empty()) {
        return kNoCull;
    }
    return static_cast<std::uint8_t>(facingOfName(name));
}

// One model description against one golden model: same box count, same boxes,
// same set of present faces, and for each face the same rect, quarter-turn and
// cullface. `label` names the json so a failure points at the file to re-read.
void checkAgainstGolden(const std::vector<ModelElement>& elements, const GoldenModel& golden,
                        const char* label) {
    require(elements.size() == golden.boxes.size(), label);
    for (std::size_t b = 0; b < elements.size(); ++b) {
        const ModelElement& element = elements[b];
        const GoldenBox& box = golden.boxes[b];
        require(near(element.from16.x, box.from[0]) && near(element.from16.y, box.from[1]) &&
                    near(element.from16.z, box.from[2]),
                label);
        require(near(element.to16.x, box.to[0]) && near(element.to16.y, box.to[1]) &&
                    near(element.to16.z, box.to[2]),
                label);
        // Every face the json declares, and no face it does not: an extra face is
        // audit R7 (the fence gate's 8 phantom quads), a missing one is a hole.
        std::array<bool, kFacingCount> expected{};
        for (const GoldenFace& gf : box.faces) {
            const Facing facing = facingOfName(gf.face);
            expected[static_cast<std::size_t>(facing)] = true;
            const ElementFace& face = element.faces[static_cast<std::size_t>(facing)];
            require(face.present, label);
            require(!face.uv.absent, label); // an explicit rect, never the projection
            require(near(face.uv.minU, gf.u0) && near(face.uv.minV, gf.v0) &&
                        near(face.uv.maxU, gf.u1) && near(face.uv.maxV, gf.v1),
                    label);
            require(face.quadrant == static_cast<std::uint8_t>(gf.rotation / 90), label);
            require(face.cull == cullOfName(gf.cullface), label);
        }
        for (std::size_t f = 0; f < kFacingCount; ++f) {
            require(element.faces[f].present == expected[f], label);
        }
    }
}

// --- the orientation anchor ---------------------------------------------------

// A door leaf and a trapdoor leaf are single boxes whose model box IS the block's
// BlockShape box (`Block.boxZ(16,13,16)` and its rotations, verified against
// 26.1 in the AR-B2/B3 audit). So the baked model's bounds must equal the shape's
// single box for every state — which is what pins the yaw table without eyes on
// a screen. It cannot be done for the fence gate: its model is a lattice strictly
// inside the shape box, and closed it is 180-degree symmetric, so a half-turn
// error is invisible to any geometric check. See the mac checklist.
void checkBoundsMatchShape(Block block, BlockState state, const char* label) {
    const auto quads = bakeElementModel(block, state);
    require(!quads.empty(), label);
    glm::vec3 lo{9.0F, 9.0F, 9.0F};
    glm::vec3 hi{-9.0F, -9.0F, -9.0F};
    for (const BakedElementQuad& q : quads) {
        for (const glm::vec3& p : q.quad.position) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    const mc::world::BlockShape shape = mc::world::blockShape(state);
    require(shape.boxes.size() == 1U, label);
    const mc::world::ShapeBox& box = shape.boxes[0];
    require(near(lo.x, box.minX) && near(lo.y, box.minY) && near(lo.z, box.minZ), label);
    require(near(hi.x, box.maxX) && near(hi.y, box.maxY) && near(hi.z, box.maxZ), label);
}

} // namespace

int main() {
    constexpr std::array<BlockOrientation, 4> kFacings{
        {BlockOrientation::North, BlockOrientation::East, BlockOrientation::South,
         BlockOrientation::West}};

    // --- 1. Doors: eight model variants, rect for rect. ---
    {
        struct Case final {
            bool upper;
            bool rightHinge;
            bool open;
            const GoldenModel* golden;
            const char* label;
        };
        const std::array<Case, 8> cases{{
            {false, false, false, &kGoldenDoorBottomLeft, "door_bottom_left.json"},
            {false, true, false, &kGoldenDoorBottomRight, "door_bottom_right.json"},
            {false, false, true, &kGoldenDoorBottomLeftOpen, "door_bottom_left_open.json"},
            {false, true, true, &kGoldenDoorBottomRightOpen, "door_bottom_right_open.json"},
            {true, false, false, &kGoldenDoorTopLeft, "door_top_left.json"},
            {true, true, false, &kGoldenDoorTopRight, "door_top_right.json"},
            {true, false, true, &kGoldenDoorTopLeftOpen, "door_top_left_open.json"},
            {true, true, true, &kGoldenDoorTopRightOpen, "door_top_right_open.json"},
        }};
        for (const Case& c : cases) {
            checkAgainstGolden({doorElement(c.upper, c.rightHinge, c.open)}, *c.golden, c.label);
            // ...and the same through the state-driven entry the store fills from,
            // so the variant decode cannot disagree with the description.
            const BlockState state = BlockState{Block::OakDoor, BlockOrientation::East}
                                         .withDoorUpperHalf(c.upper)
                                         .withHinge(c.rightHinge ? DoorHinge::Right
                                                                 : DoorHinge::Left)
                                         .withOpen(c.open);
            checkAgainstGolden(doorElements(state), *c.golden, c.label);
        }
    }

    // --- 2. Trapdoors: three model variants. ---
    {
        checkAgainstGolden({trapdoorElement(false, false)}, kGoldenTrapdoorBottom,
                           "template_trapdoor_bottom.json");
        checkAgainstGolden({trapdoorElement(true, false)}, kGoldenTrapdoorTop,
                           "template_trapdoor_top.json");
        checkAgainstGolden({trapdoorElement(false, true)}, kGoldenTrapdoorOpen,
                           "template_trapdoor_open.json");
        // An open trapdoor's model does not depend on which half it hangs from
        // (oak_trapdoor.json points both halves at template_trapdoor_open).
        checkAgainstGolden({trapdoorElement(true, true)}, kGoldenTrapdoorOpen,
                           "template_trapdoor_open.json (top half)");
    }

    // --- 3. Fence gates: four model variants, including the in_wall pair whose
    // boxes the description derives by a -3 Y offset. The golden copy has them
    // literally, so the offset is a claim this file checks rather than trusts. ---
    {
        checkAgainstGolden(fenceGateElements(false, false), kGoldenGateClosed,
                           "template_fence_gate.json");
        checkAgainstGolden(fenceGateElements(true, false), kGoldenGateOpen,
                           "template_fence_gate_open.json");
        checkAgainstGolden(fenceGateElements(false, true), kGoldenGateWallClosed,
                           "template_fence_gate_wall.json");
        checkAgainstGolden(fenceGateElements(true, true), kGoldenGateWallOpen,
                           "template_fence_gate_wall_open.json");
    }

    // --- 4. Face count: audit R7 stated as a number. The hand-written
    // appendFenceGate draws six faces per box = 48; vanilla declares 40, because
    // each of the four bars is buried in a post on two sides. ---
    {
        const auto faceCount = [](const std::vector<ModelElement>& elements) {
            std::size_t n = 0;
            for (const ModelElement& e : elements) {
                for (const ElementFace& f : e.faces) {
                    n += f.present ? 1U : 0U;
                }
            }
            return n;
        };
        require(faceCount(fenceGateElements(false, false)) == 40U, "closed gate declares 40 faces");
        require(faceCount(fenceGateElements(true, false)) == 40U, "open gate declares 40 faces");
        require(faceCount(fenceGateElements(false, true)) == 40U, "wall gate declares 40 faces");
        require(faceCount(fenceGateElements(true, true)) == 40U,
                "open wall gate declares 40 faces");
        // A door leaf declares five faces (no cap on the half it does not own),
        // a trapdoor leaf all six.
        require(faceCount({doorElement(false, false, false)}) == 5U, "a door leaf has five faces");
        require(faceCount({doorElement(true, true, true)}) == 5U, "both halves have five");
        require(faceCount({trapdoorElement(false, false)}) == 6U, "a trapdoor leaf has six faces");
    }

    // --- 5. The orientation anchor: model bounds == BlockShape box, every
    // door and trapdoor variant. ---
    for (const BlockOrientation facing : kFacings) {
        for (const bool upper : {false, true}) {
            for (const bool open : {false, true}) {
                for (const auto hinge : {DoorHinge::Left, DoorHinge::Right}) {
                    checkBoundsMatchShape(Block::OakDoor,
                                          BlockState{Block::OakDoor, facing}
                                              .withDoorUpperHalf(upper)
                                              .withHinge(hinge)
                                              .withOpen(open),
                                          "a door's baked bounds must equal its BlockShape box");
                }
                checkBoundsMatchShape(Block::OakTrapdoor,
                                      BlockState{Block::OakTrapdoor, facing}
                                          .withDoorUpperHalf(upper)
                                          .withOpen(open),
                                      "a trapdoor's baked bounds must equal its BlockShape box");
            }
        }
    }

    // --- 5b. The fence gate's half-turn, the one orientation the bounds anchor
    // cannot reach. Its model is a lattice inside the shape box and is
    // 180-degree symmetric when closed, so a wrong half-turn is invisible there;
    // only an OPEN gate shows it, because its two leaves have swung to one side.
    //
    // The rule comes from the model itself, not from taste:
    // template_fence_gate_open.json puts the swung leaves at z 9..15, and that
    // model is oak_fence_gate.json's `facing=south` variant (the one with no
    // `"y"`). +Z is south. So an open gate's leaves lie on its FACING side,
    // always — which is also what FenceGateBlock#useWithoutItem arranges by
    // turning FACING to the player's own direction before opening, so a gate
    // always swings away from whoever opened it.
    //
    // This is what the hand-written appendFenceGate got wrong: it yawed west to
    // 90 and east to 270, i.e. a half turn off on both, and only an open gate
    // facing east or west could show it.
    {
        for (const BlockOrientation facing : kFacings) {
            const auto quads = bakeElementModel(
                Block::OakFenceGate,
                BlockState{Block::OakFenceGate, facing}.withOpen(true));
            require(!quads.empty(), "an open gate bakes quads");
            glm::vec3 centroid{0.0F};
            float n = 0.0F;
            for (const BakedElementQuad& q : quads) {
                for (const glm::vec3& p : q.quad.position) {
                    centroid += p;
                    n += 1.0F;
                }
            }
            centroid /= n;
            const glm::ivec3 offset = mc::world::orientationOffset(facing);
            const float along = (centroid.x - 0.5F) * static_cast<float>(offset.x) +
                                (centroid.z - 0.5F) * static_cast<float>(offset.z);
            require(along > 0.05F, "an open gate's leaves must lie on its FACING side");
        }
        // ...and a closed gate is centred, so the same probe cannot pass by
        // accident on a model that never moved.
        for (const BlockOrientation facing : kFacings) {
            const auto quads = bakeElementModel(Block::OakFenceGate,
                                                BlockState{Block::OakFenceGate, facing});
            glm::vec3 centroid{0.0F};
            float n = 0.0F;
            for (const BakedElementQuad& q : quads) {
                for (const glm::vec3& p : q.quad.position) {
                    centroid += p;
                    n += 1.0F;
                }
            }
            centroid /= n;
            require(near(centroid.x, 0.5F) && near(centroid.z, 0.5F),
                    "a closed gate is centred in its cell");
        }
    }

    // --- 6. The AO bit (RN-10a capability 2). It is a per-MODEL fact, and the
    // door/trapdoor pair is the proof it cannot be derived from the shape family:
    // the two are near-identical leaves and disagree. ---
    {
        require(!mc::world::blockDefinition(Block::OakDoor).ambientOcclusion,
                "door_bottom_left.json declares ambientocclusion false");
        require(!mc::world::blockDefinition(Block::DarkOakDoor).ambientOcclusion,
                "every door species, not just oak");
        require(!mc::world::blockDefinition(Block::Repeater).ambientOcclusion,
                "repeater_*tick*.json declares ambientocclusion false");
        require(!mc::world::blockDefinition(Block::Comparator).ambientOcclusion,
                "comparator*.json declares ambientocclusion false");
        require(!mc::world::blockDefinition(Block::Lever).ambientOcclusion,
                "lever.json declares ambientocclusion false");
        require(mc::world::blockDefinition(Block::OakTrapdoor).ambientOcclusion,
                "template_trapdoor_bottom.json inherits true");
        require(mc::world::blockDefinition(Block::OakFenceGate).ambientOcclusion,
                "template_fence_gate.json inherits true");
        require(mc::world::blockDefinition(Block::Anvil).ambientOcclusion,
                "template_anvil.json inherits true");
        require(mc::world::blockDefinition(Block::Stone).ambientOcclusion,
                "an ordinary cube keeps the default");
    }

    // --- 7. The shade bit (RN-10a capability 3). Nothing sets it yet — RN-10d's
    // halo elements are its first user — so what is locked here is that it
    // defaults to true and survives the bake onto every quad. A bit that silently
    // dropped between the element and the baked quad would be found only by
    // eye, on a halo, three nodes later. ---
    {
        ModelElement e;
        e.from16 = {0.0F, 0.0F, 0.0F};
        e.to16 = {16.0F, 16.0F, 16.0F};
        require(e.shade, "shade defaults to true, as a json element with no `shade` does");
        detail::putFace(e, Facing::Up, 0, detail::rect(0, 0, 16, 16));
        detail::putFace(e, Facing::North, 0, detail::rect(0, 0, 16, 16));

        const std::array<ModelElement, 1> shaded{{e}};
        for (const BakedElementQuad& q : bakeElements(shaded, ModelTransform{})) {
            require(q.shade, "an element that never sets shade bakes shaded quads");
        }
        e.shade = false;
        const std::array<ModelElement, 1> unshaded{{e}};
        const auto quads = bakeElements(unshaded, ModelTransform{});
        require(quads.size() == 2U, "both declared faces baked");
        for (const BakedElementQuad& q : quads) {
            require(!q.shade, "and `shade: false` has to survive the bake onto every quad");
        }
    }

    return 0;
}
