#include "animation/AnimationController.hpp"

#include "animation/AnimationClip.hpp"
#include "core/Json.hpp"

#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mc::animation {

namespace {

// Compiles a weight field: a Molang string, or a plain number (default 1).
[[nodiscard]] MolangExpression compileWeight(const core::Json& value) {
    if (value.isString()) {
        return MolangExpression::compile(value.asString());
    }
    if (value.isNumber()) {
        return MolangExpression::constant(value.asFloat(1.0F));
    }
    return MolangExpression::constant(1.0F);
}

[[nodiscard]] BlendMode parseMode(std::string_view text) {
    return text == "override" ? BlendMode::Override : BlendMode::Additive;
}

} // namespace

// ---- AnimationController ----------------------------------------------------

const ControllerState* AnimationController::findState(std::string_view name) const {
    const auto it = stateIndex_.find(std::string{name});
    return it != stateIndex_.end() ? &states_[it->second] : nullptr;
}

ControllerState& AnimationController::state(const std::string& name) {
    const auto it = stateIndex_.find(name);
    if (it != stateIndex_.end()) {
        return states_[it->second];
    }
    const std::size_t index = states_.size();
    ControllerState created;
    created.name = name;
    states_.push_back(std::move(created));
    stateIndex_.emplace(name, index);
    if (initialState_.empty()) {
        initialState_ = name; // first declared state is the default initial
    }
    return states_[index];
}

AnimationController AnimationController::load(const core::Json& node) {
    AnimationController controller;

    const core::Json& states = node["states"];
    if (!states.isObject()) {
        throw std::runtime_error("animation controller is missing a 'states' object");
    }

    for (const auto& [stateName, stateJson] : states.asObject()) {
        ControllerState& target = controller.state(stateName);

        // animations: array of either "clip" or {"clip": weightExprOrNumber} or
        // {"animation": "clip", "weight": ..., "mask": ..., "blend": ...}.
        const core::Json& animations = stateJson["animations"];
        if (animations.isArray()) {
            for (std::size_t i = 0U; i < animations.size(); ++i) {
                const core::Json& entry = animations[i];
                ControllerAnimation anim;
                if (entry.isString()) {
                    anim.clip = entry.asString();
                } else if (entry.isObject()) {
                    if (entry.contains("animation")) {
                        anim.clip = entry["animation"].asString();
                        anim.weight = compileWeight(entry["weight"]);
                    } else {
                        // Bedrock's short form: a single-key {"clip": weight} map.
                        for (const auto& [clipName, weightJson] : entry.asObject()) {
                            anim.clip = clipName;
                            anim.weight = compileWeight(weightJson);
                            break;
                        }
                    }
                    if (entry.contains("mask")) {
                        anim.mask = entry["mask"].asString();
                    }
                    if (entry.contains("blend")) {
                        anim.mode = parseMode(entry["blend"].asString());
                    }
                }
                if (!anim.clip.empty()) {
                    target.animations.push_back(std::move(anim));
                }
            }
        }

        // transitions: array of single-key maps {"targetState": "molang cond"}.
        const core::Json& transitions = stateJson["transitions"];
        if (transitions.isArray()) {
            for (std::size_t i = 0U; i < transitions.size(); ++i) {
                const core::Json& entry = transitions[i];
                if (!entry.isObject()) {
                    continue;
                }
                for (const auto& [targetName, condJson] : entry.asObject()) {
                    ControllerTransition transition;
                    transition.target = targetName;
                    if (condJson.isString()) {
                        transition.condition = MolangExpression::compile(condJson.asString());
                    } else if (condJson.isNumber()) {
                        transition.condition = MolangExpression::constant(condJson.asFloat(0.0F));
                    }
                    target.transitions.push_back(std::move(transition));
                    break; // one target per transition entry
                }
            }
        }

        target.blendTransition = stateJson["blend_transition"].asFloat(0.0F);
    }

    // An explicit initial_state overrides the first-declared default.
    if (node.contains("initial_state")) {
        const std::string initial = node["initial_state"].asString();
        if (controller.findState(initial) != nullptr) {
            controller.setInitialState(initial);
        }
    }
    return controller;
}

// ---- AnimationControllerSet -------------------------------------------------

const AnimationController* AnimationControllerSet::find(std::string_view name) const {
    const auto it = controllers_.find(std::string{name});
    return it != controllers_.end() ? &it->second : nullptr;
}

void AnimationControllerSet::add(std::string name, AnimationController controller) {
    controllers_.insert_or_assign(std::move(name), std::move(controller));
}

void AnimationControllerSet::loadDocument(const core::Json& document) {
    const core::Json& controllers = document["animation_controllers"];
    if (!controllers.isObject()) {
        throw std::runtime_error("document is missing an 'animation_controllers' object");
    }
    for (const auto& [name, node] : controllers.asObject()) {
        add(name, AnimationController::load(node));
    }
}

AnimationControllerSet AnimationControllerSet::parse(std::string_view jsonText) {
    AnimationControllerSet set;
    set.loadDocument(core::Json::parse(jsonText));
    return set;
}

// ---- AnimationControllerInstance --------------------------------------------

AnimationControllerInstance::AnimationControllerInstance(const AnimationController& controller) {
    bind(controller);
}

void AnimationControllerInstance::bind(const AnimationController& controller) {
    controller_ = &controller;
    currentState_ = controller.initialState();
    previousState_.clear();
    blend_ = Transition{1.0F, 1.0F, 0.0F}; // settled: no crossfade in flight
}

void AnimationControllerInstance::update(float deltaSeconds, const MolangContext& context) {
    if (controller_ == nullptr) {
        return;
    }
    // Advance an in-flight crossfade first so a settled transition frees the old
    // state before we consider leaving the new one.
    blend_.advance(deltaSeconds);
    if (blend_.finished()) {
        previousState_.clear();
    }

    const ControllerState* state = controller_->findState(currentState_);
    if (state == nullptr) {
        return;
    }

    // First transition whose condition is truthy wins (Bedrock order semantics).
    for (const ControllerTransition& transition : state->transitions) {
        if (transition.condition.evaluate(context) != 0.0F) {
            if (transition.target == currentState_) {
                break; // already here
            }
            const ControllerState* next = controller_->findState(transition.target);
            if (next == nullptr) {
                break; // dangling target: stay put
            }
            previousState_ = currentState_;
            currentState_ = transition.target;
            // Crossfade the new state in from 0 to 1 over its blend duration,
            // advancing by this frame's dt so the fade begins immediately rather
            // than sitting at 0 for one frame.
            blend_ = Transition{0.0F, 1.0F, next->blendTransition};
            blend_.advance(deltaSeconds);
            if (blend_.finished()) {
                previousState_.clear(); // zero-duration blend settles at once
            }
            break;
        }
    }
}

void AnimationControllerInstance::queueState(Animator& animator, const AnimationLibrary& library,
                                             const MaskResolver& resolveMask,
                                             const MolangContext& context,
                                             const ControllerState& state, float stateWeight) const {
    if (stateWeight <= 0.0F) {
        return;
    }
    for (const ControllerAnimation& anim : state.animations) {
        const AnimationClip* clip = library.find(anim.clip);
        if (clip == nullptr) {
            continue; // controller names a clip this library does not have
        }
        const float weight = anim.weight.evaluate(context) * stateWeight;
        if (weight <= 0.0F) {
            continue;
        }
        const BoneMask* mask = (!anim.mask.empty() && resolveMask) ? resolveMask(anim.mask) : nullptr;
        animator.addLayer(*clip, clip->localTime(context.query("anim_time")), weight, mask,
                          anim.mode);
    }
}

void AnimationControllerInstance::apply(Animator& animator, const AnimationLibrary& library,
                                        const MaskResolver& resolveMask,
                                        const MolangContext& context) const {
    if (controller_ == nullptr) {
        return;
    }
    const float progress = blend_.value();
    // Fade the state being left out while the entered state fades in. Once the
    // crossfade settles, only the current state contributes at full weight.
    if (!blend_.finished() && !previousState_.empty()) {
        if (const ControllerState* prev = controller_->findState(previousState_)) {
            queueState(animator, library, resolveMask, context, *prev, 1.0F - progress);
        }
    }
    if (const ControllerState* current = controller_->findState(currentState_)) {
        queueState(animator, library, resolveMask, context, *current, progress);
    }
}

// ---- built-in player controller ---------------------------------------------

AnimationControllerSet builtinPlayerControllers() {
    // idle <-> walk on variable.walk_amount, and a sneak state on
    // variable.sneaking, blended by short crossfades. The look clip is applied
    // in every state so head tracking is independent of locomotion. This is the
    // data equivalent of PlayerModelAnimator's hand-eased walk/sneak weights;
    // wiring the player to consume it is ANIM-4.
    static constexpr const char* kDocument = R"({
      "animation_controllers": {
        "controller.player.locomotion": {
          "initial_state": "idle",
          "states": {
            "idle": {
              "animations": [
                {"animation.player.idle": 1.0},
                {"animation": "animation.player.look"}
              ],
              "transitions": [
                {"sneak": "variable.sneaking"},
                {"walk": "variable.walk_amount > 0.1"}
              ],
              "blend_transition": 0.15
            },
            "walk": {
              "animations": [
                {"animation.player.walk": 1.0},
                {"animation": "animation.player.look"}
              ],
              "transitions": [
                {"sneak": "variable.sneaking"},
                {"idle": "variable.walk_amount <= 0.1"}
              ],
              "blend_transition": 0.1
            },
            "sneak": {
              "animations": [
                {"animation.player.sneak": 1.0},
                {"animation": "animation.player.walk", "weight": "variable.walk_amount"},
                {"animation": "animation.player.look"}
              ],
              "transitions": [
                {"idle": "!variable.sneaking"}
              ],
              "blend_transition": 0.15
            }
          }
        }
      }
    })";
    return AnimationControllerSet::parse(kDocument);
}

AnimationControllerSet loadPlayerControllers(const std::filesystem::path& animationDirectory) {
    try {
        const std::filesystem::path path =
            animationDirectory / "player.animation_controllers.json";
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return builtinPlayerControllers();
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        AnimationControllerSet set = AnimationControllerSet::parse(buffer.str());
        if (set.size() == 0U) {
            return builtinPlayerControllers(); // empty override: keep built-ins
        }
        return set;
    } catch (const std::exception&) {
        return builtinPlayerControllers(); // unreadable/malformed: keep built-ins
    }
}

} // namespace mc::animation
