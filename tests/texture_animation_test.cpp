#include "assets/TextureAnimation.hpp"

#include <cassert>
#include <string_view>

// The .mcmeta animation block drives animated block textures. These pin the
// frametime default, both frame-entry forms, and that a still texture (no
// animation object) or malformed JSON reads as "not animated".
int main() {
    using namespace mc::assets;

    // --- A frametime-only animation: every source frame, in order. ---
    {
        const auto anim = TextureAnimation::parse(R"({"animation": {"frametime": 2}})");
        assert(anim.has_value());
        assert(anim->frametime == 2);
        assert(!anim->interpolate);
        assert(anim->frames.empty()); // no explicit order
    }

    // --- Explicit frame order, bare-index and object forms. ---
    {
        const auto anim = TextureAnimation::parse(R"({
            "animation": {
                "interpolate": true,
                "frametime": 3,
                "frames": [0, 2, {"index": 1, "time": 5}]
            }
        })");
        assert(anim.has_value());
        assert(anim->interpolate && anim->frametime == 3);
        assert(anim->frames.size() == 3U);
        assert(anim->frames[0].index == 0 && anim->frames[0].time == -1);   // bare index
        assert(anim->frames[1].index == 2);
        assert(anim->frames[2].index == 1 && anim->frames[2].time == 5);    // object form
    }

    // --- frametime below 1 is clamped so a bad pack cannot divide by zero. ---
    {
        const auto anim = TextureAnimation::parse(R"({"animation": {"frametime": 0}})");
        assert(anim.has_value() && anim->frametime == 1);
    }

    // --- A still texture (no animation object) is not animated. ---
    assert(!TextureAnimation::parse(R"({"other": {}})").has_value());
    // --- Malformed JSON is treated as "not animated", not a throw. ---
    assert(!TextureAnimation::parse("{ not json").has_value());

    return 0;
}
