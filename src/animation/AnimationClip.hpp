#pragma once

#include "animation/Molang.hpp"

#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::core {
class Json;
}

namespace mc::animation {

// Interpolation between two keyframes. Linear/Step/CatmullRom match Bedrock's
// `lerp_mode`; Bezier adds cubic-Hermite handles (per-keyframe in/out tangents)
// for authored ease curves such as the chest lid.
enum class LerpMode { Linear, Step, CatmullRom, Bezier };

// A vector value whose components are Molang expressions, so a channel can be a
// literal like [0, 30, 0] or an expression like ["math.sin(q.anim_time*38)", 0, 0].
struct Vec3Expression final {
    MolangExpression x = MolangExpression::constant(0.0F);
    MolangExpression y = MolangExpression::constant(0.0F);
    MolangExpression z = MolangExpression::constant(0.0F);

    [[nodiscard]] glm::vec3 evaluate(const MolangContext& context) const {
        return {x.evaluate(context), y.evaluate(context), z.evaluate(context)};
    }
};

// A single keyframe. `pre`/`post` differ only for stepped/discontinuous keys.
// For Bezier segments, `outTangent` is the slope leaving this keyframe and
// `inTangent` is the slope arriving at it (value units per second). Unset
// tangents default to zero, which yields a smooth ease-in/ease-out.
struct Keyframe final {
    float time = 0.0F;
    Vec3Expression pre;
    Vec3Expression post;
    LerpMode lerp = LerpMode::Linear;
    Vec3Expression inTangent;
    Vec3Expression outTangent;
};

// A time-ordered list of keyframes for one property (rotation/position/scale).
class AnimationChannel final {
  public:
    [[nodiscard]] bool empty() const { return keyframes_.empty(); }
    [[nodiscard]] const std::vector<Keyframe>& keyframes() const { return keyframes_; }
    void addKeyframe(Keyframe keyframe);

    // Programmatic keyframe helpers (the "basic keyframe system") so gameplay
    // code can author clips in C++ without going through JSON.
    void addLinear(float time, const glm::vec3& value);
    void addStep(float time, const glm::vec3& value);
    void addBezier(float time, const glm::vec3& value, const glm::vec3& inTangent,
                   const glm::vec3& outTangent);
    // Bezier keyframe with flat (zero) tangents: a symmetric ease-in/ease-out.
    void addEased(float time, const glm::vec3& value);

    // Samples the channel at `time`, returning `fallback` when empty.
    [[nodiscard]] glm::vec3 sample(float time, const MolangContext& context,
                                   const glm::vec3& fallback) const;

  private:
    std::vector<Keyframe> keyframes_;
};

// The three animated properties for a single bone.
struct BoneAnimation final {
    AnimationChannel rotation; // degrees, added to the bone's rest rotation
    AnimationChannel position; // model-unit offset
    AnimationChannel scale;    // multiplier around the pivot (default 1)
};

// One animation, e.g. "animation.player.walk". Bones are keyed by name so a
// clip authored for a geometry maps onto that geometry's bones by name.
class AnimationClip final {
  public:
    [[nodiscard]] float length() const { return length_; }
    [[nodiscard]] bool loops() const { return loop_; }
    [[nodiscard]] const std::unordered_map<std::string, BoneAnimation>& bones() const {
        return bones_;
    }
    [[nodiscard]] const BoneAnimation* findBone(std::string_view name) const;

    // Wraps/clamps a raw elapsed time into the clip's local time based on loop.
    [[nodiscard]] float localTime(float elapsedSeconds) const;

    // Programmatic construction (pairs with the AnimationChannel keyframe API).
    void setLength(float seconds) { length_ = seconds; }
    void setLoop(bool loop) { loop_ = loop; }
    [[nodiscard]] BoneAnimation& bone(const std::string& name) { return bones_[name]; }

    // Parses a single Bedrock animation node (the value under an animation name).
    [[nodiscard]] static AnimationClip load(const core::Json& node);

  private:
    float length_ = 0.0F;
    bool loop_ = false;
    std::unordered_map<std::string, BoneAnimation> bones_;
};

// A library of clips loaded from a Bedrock `{"animations": {...}}` document.
class AnimationLibrary final {
  public:
    [[nodiscard]] const AnimationClip* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return clips_.size(); }
    void add(std::string name, AnimationClip clip);

    // Merges all animations from a document into this library.
    void loadDocument(const core::Json& document);

    [[nodiscard]] static AnimationLibrary parse(std::string_view jsonText);

  private:
    std::unordered_map<std::string, AnimationClip> clips_;
};

} // namespace mc::animation
