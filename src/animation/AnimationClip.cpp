#include "animation/AnimationClip.hpp"

#include "core/Json.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mc::animation {
namespace {

// Parses a scalar channel component: either a JSON number or a Molang string.
[[nodiscard]] MolangExpression compileComponent(const core::Json& value) {
    if (value.isString()) {
        return MolangExpression::compile(value.asString());
    }
    return MolangExpression::constant(value.asFloat(0.0F));
}

// Parses a [x, y, z] triple where any element may be a number or Molang string.
// A bare number or single-element form applies uniformly to all three axes,
// matching Bedrock's leniency for scale channels.
[[nodiscard]] Vec3Expression compileVec3(const core::Json& value) {
    Vec3Expression result;
    if (value.isArray()) {
        if (value.size() >= 3U) {
            result.x = compileComponent(value[0]);
            result.y = compileComponent(value[1]);
            result.z = compileComponent(value[2]);
        } else if (value.size() == 1U) {
            result.x = compileComponent(value[0]);
            result.y = compileComponent(value[0]);
            result.z = compileComponent(value[0]);
        }
    } else if (value.isNumber() || value.isString()) {
        result.x = compileComponent(value);
        result.y = compileComponent(value);
        result.z = compileComponent(value);
    }
    return result;
}

[[nodiscard]] LerpMode parseLerpMode(const core::Json& value) {
    if (value.isString()) {
        const std::string& mode = value.asString();
        if (mode == "step") return LerpMode::Step;
        if (mode == "catmullrom" || mode == "catmull_rom") return LerpMode::CatmullRom;
        if (mode == "bezier") return LerpMode::Bezier;
    }
    return LerpMode::Linear;
}

// Parses a channel that is either a constant value or a { "time": value } map.
[[nodiscard]] AnimationChannel parseChannel(const core::Json& node) {
    AnimationChannel channel;
    if (node.isNull()) {
        return channel;
    }
    // Constant form: rotation / position / scale given directly as a value.
    if (node.isArray() || node.isNumber() || node.isString()) {
        Keyframe keyframe;
        keyframe.time = 0.0F;
        keyframe.post = keyframe.pre = compileVec3(node);
        channel.addKeyframe(std::move(keyframe));
        return channel;
    }
    // Keyframed form: an object mapping a time string to a value/descriptor.
    if (node.isObject()) {
        for (const auto& [timeText, value] : node.asObject()) {
            Keyframe keyframe;
            keyframe.time = std::stof(timeText);
            if (value.isObject() && (value.contains("pre") || value.contains("post"))) {
                const core::Json& pre = value.contains("pre") ? value["pre"] : value["post"];
                const core::Json& post = value.contains("post") ? value["post"] : value["pre"];
                keyframe.pre = compileVec3(pre);
                keyframe.post = compileVec3(post);
                keyframe.lerp = parseLerpMode(value["lerp_mode"]);
            } else if (value.isObject() && value.contains("vector")) {
                keyframe.post = keyframe.pre = compileVec3(value["vector"]);
                keyframe.lerp = parseLerpMode(value["lerp_mode"]);
            } else {
                keyframe.post = keyframe.pre = compileVec3(value);
            }
            // Optional Bezier handles. `left_tangent`/`right_tangent` (graph-editor
            // slopes) map to the arriving/leaving tangents.
            if (value.isObject()) {
                if (value.contains("left_tangent")) {
                    keyframe.inTangent = compileVec3(value["left_tangent"]);
                }
                if (value.contains("right_tangent")) {
                    keyframe.outTangent = compileVec3(value["right_tangent"]);
                }
            }
            channel.addKeyframe(std::move(keyframe));
        }
    }
    return channel;
}

[[nodiscard]] glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                                   const glm::vec3& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * p1) + (-p0 + p2) * t +
                   (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * t2 +
                   (-p0 + 3.0F * p1 - 3.0F * p2 + p3) * t3);
}

// Cubic-Hermite interpolation with endpoint tangents expressed as value/second
// slopes. `duration` is the segment length in seconds so the tangents scale
// into the normalised parameter t. Flat tangents (0) reduce to a smoothstep,
// giving a natural ease-in/ease-out.
[[nodiscard]] glm::vec3 hermite(const glm::vec3& p0, const glm::vec3& p1,
                                const glm::vec3& slope0, const glm::vec3& slope1, float duration,
                                float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0F * t3 - 3.0F * t2 + 1.0F;
    const float h10 = t3 - 2.0F * t2 + t;
    const float h01 = -2.0F * t3 + 3.0F * t2;
    const float h11 = t3 - t2;
    const glm::vec3 m0 = slope0 * duration;
    const glm::vec3 m1 = slope1 * duration;
    return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

} // namespace

void AnimationChannel::addKeyframe(Keyframe keyframe) {
    const auto position = std::upper_bound(
        keyframes_.begin(), keyframes_.end(), keyframe.time,
        [](float time, const Keyframe& existing) { return time < existing.time; });
    keyframes_.insert(position, std::move(keyframe));
}

void AnimationChannel::addLinear(float time, const glm::vec3& value) {
    Keyframe keyframe;
    keyframe.time = time;
    keyframe.lerp = LerpMode::Linear;
    keyframe.pre = keyframe.post = {MolangExpression::constant(value.x),
                                    MolangExpression::constant(value.y),
                                    MolangExpression::constant(value.z)};
    addKeyframe(std::move(keyframe));
}

void AnimationChannel::addStep(float time, const glm::vec3& value) {
    Keyframe keyframe;
    keyframe.time = time;
    keyframe.lerp = LerpMode::Step;
    keyframe.pre = keyframe.post = {MolangExpression::constant(value.x),
                                    MolangExpression::constant(value.y),
                                    MolangExpression::constant(value.z)};
    addKeyframe(std::move(keyframe));
}

void AnimationChannel::addBezier(float time, const glm::vec3& value, const glm::vec3& inTangent,
                                 const glm::vec3& outTangent) {
    Keyframe keyframe;
    keyframe.time = time;
    keyframe.lerp = LerpMode::Bezier;
    keyframe.pre = keyframe.post = {MolangExpression::constant(value.x),
                                    MolangExpression::constant(value.y),
                                    MolangExpression::constant(value.z)};
    keyframe.inTangent = {MolangExpression::constant(inTangent.x),
                          MolangExpression::constant(inTangent.y),
                          MolangExpression::constant(inTangent.z)};
    keyframe.outTangent = {MolangExpression::constant(outTangent.x),
                           MolangExpression::constant(outTangent.y),
                           MolangExpression::constant(outTangent.z)};
    addKeyframe(std::move(keyframe));
}

void AnimationChannel::addEased(float time, const glm::vec3& value) {
    addBezier(time, value, glm::vec3{0.0F}, glm::vec3{0.0F});
}

glm::vec3 AnimationChannel::sample(float time, const MolangContext& context,
                                   const glm::vec3& fallback) const {
    if (keyframes_.empty()) {
        return fallback;
    }
    if (keyframes_.size() == 1U) {
        return keyframes_.front().post.evaluate(context);
    }
    // Before the first / after the last keyframe: clamp to the endpoints.
    if (time <= keyframes_.front().time) {
        return keyframes_.front().post.evaluate(context);
    }
    if (time >= keyframes_.back().time) {
        return keyframes_.back().pre.evaluate(context);
    }

    std::size_t upper = 0U;
    while (upper < keyframes_.size() && keyframes_[upper].time <= time) {
        ++upper;
    }
    const std::size_t lower = upper - 1U;
    const Keyframe& start = keyframes_[lower];
    const Keyframe& end = keyframes_[upper];

    const float span = end.time - start.time;
    const float t = span > 0.0F ? (time - start.time) / span : 0.0F;

    const glm::vec3 startValue = start.post.evaluate(context);
    const glm::vec3 endValue = end.pre.evaluate(context);

    if (end.lerp == LerpMode::Step) {
        return startValue;
    }
    if (end.lerp == LerpMode::CatmullRom) {
        const glm::vec3 before =
            lower > 0U ? keyframes_[lower - 1U].post.evaluate(context) : startValue;
        const glm::vec3 after = upper + 1U < keyframes_.size()
                                    ? keyframes_[upper + 1U].pre.evaluate(context)
                                    : endValue;
        return catmullRom(before, startValue, endValue, after, t);
    }
    if (end.lerp == LerpMode::Bezier) {
        const glm::vec3 slopeOut = start.outTangent.evaluate(context);
        const glm::vec3 slopeIn = end.inTangent.evaluate(context);
        return hermite(startValue, endValue, slopeOut, slopeIn, span, t);
    }
    return startValue + (endValue - startValue) * t;
}

const BoneAnimation* AnimationClip::findBone(std::string_view name) const {
    const auto it = bones_.find(std::string{name});
    return it != bones_.end() ? &it->second : nullptr;
}

float AnimationClip::localTime(float elapsedSeconds) const {
    if (length_ <= 0.0F) {
        return 0.0F;
    }
    if (loop_) {
        float wrapped = std::fmod(elapsedSeconds, length_);
        if (wrapped < 0.0F) {
            wrapped += length_;
        }
        return wrapped;
    }
    return std::clamp(elapsedSeconds, 0.0F, length_);
}

AnimationClip AnimationClip::load(const core::Json& node) {
    AnimationClip clip;
    clip.loop_ = node["loop"].asBool(false);

    const core::Json& bonesNode = node["bones"];
    float maxKeyframeTime = 0.0F;
    if (bonesNode.isObject()) {
        for (const auto& [boneName, boneNode] : bonesNode.asObject()) {
            BoneAnimation animation;
            animation.rotation = parseChannel(boneNode["rotation"]);
            animation.position = parseChannel(boneNode["position"]);
            animation.scale = parseChannel(boneNode["scale"]);
            for (const AnimationChannel* channel :
                 {&animation.rotation, &animation.position, &animation.scale}) {
                if (!channel->empty()) {
                    maxKeyframeTime = std::max(maxKeyframeTime, channel->keyframes().back().time);
                }
            }
            clip.bones_.emplace(boneName, std::move(animation));
        }
    }

    // `animation_length` is authoritative; otherwise derive it from keyframes.
    if (node.contains("animation_length")) {
        clip.length_ = node["animation_length"].asFloat(0.0F);
    } else {
        clip.length_ = maxKeyframeTime;
    }
    return clip;
}

const AnimationClip* AnimationLibrary::find(std::string_view name) const {
    const auto it = clips_.find(std::string{name});
    return it != clips_.end() ? &it->second : nullptr;
}

void AnimationLibrary::add(std::string name, AnimationClip clip) {
    clips_.insert_or_assign(std::move(name), std::move(clip));
}

void AnimationLibrary::loadDocument(const core::Json& document) {
    const core::Json& animations = document["animations"];
    if (!animations.isObject()) {
        throw std::runtime_error("animation document is missing an 'animations' object");
    }
    for (const auto& [name, node] : animations.asObject()) {
        clips_.insert_or_assign(name, AnimationClip::load(node));
    }
}

AnimationLibrary AnimationLibrary::parse(std::string_view jsonText) {
    AnimationLibrary library;
    library.loadDocument(core::Json::parse(jsonText));
    return library;
}

} // namespace mc::animation
