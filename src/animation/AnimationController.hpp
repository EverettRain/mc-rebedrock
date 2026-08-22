#pragma once

#include "animation/Animator.hpp"
#include "animation/BoneMask.hpp"
#include "animation/Molang.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::core {
class Json;
}

namespace mc::animation {

class AnimationLibrary;
class AnimationClip;

// One animation a state plays: which clip, at what weight (a Molang expression
// so it can read live inputs like variable.walk_amount), through which named
// bone mask, and with which blend mode. This is the data-driven equivalent of a
// hand-written `animator.addLayer(clip, t, weight, mask, mode)` call.
struct ControllerAnimation final {
    std::string clip;            // clip name looked up in the AnimationLibrary
    std::string mask;            // named bone-mask group, or empty for whole body
    MolangExpression weight = MolangExpression::constant(1.0F);
    BlendMode mode = BlendMode::Additive;
};

// A transition out of a state: the target state and the Molang condition that
// triggers it. Conditions are evaluated in declaration order and the first that
// evaluates truthy (non-zero) wins, matching Bedrock's animation_controllers.
struct ControllerTransition final {
    std::string target;
    MolangExpression condition = MolangExpression::constant(0.0F);
};

// One controller state: the animations it plays, the transitions that can leave
// it, and how long a crossfade into/out of this state lasts.
struct ControllerState final {
    std::string name;
    std::vector<ControllerAnimation> animations;
    std::vector<ControllerTransition> transitions;
    float blendTransition = 0.0F; // seconds; 0 = snap
};

// A data-driven animation state machine (Bedrock `animation_controllers`). Pure
// data: it names states and their transitions but does not itself hold runtime
// position — that lives in AnimationControllerInstance, so one shared controller
// definition drives many entities.
class AnimationController final {
  public:
    [[nodiscard]] const std::string& initialState() const { return initialState_; }
    [[nodiscard]] const ControllerState* findState(std::string_view name) const;
    [[nodiscard]] std::size_t stateCount() const { return states_.size(); }

    // Programmatic construction (built-in controllers author themselves this way
    // so unit tests stay hermetic and the game animates with no resource pack).
    void setInitialState(std::string name) { initialState_ = std::move(name); }
    ControllerState& state(const std::string& name);

    // Parses one controller node (the value under a controller name in a
    // `{"animation_controllers": {...}}` document).
    [[nodiscard]] static AnimationController load(const core::Json& node);

  private:
    std::string initialState_;
    // Ordered so declaration order (and thus a stable initial fallback) survives.
    std::vector<ControllerState> states_;
    std::unordered_map<std::string, std::size_t> stateIndex_;
};

// A named collection of controllers, loaded from a Bedrock
// `{"animation_controllers": {...}}` document. Mirrors AnimationLibrary.
class AnimationControllerSet final {
  public:
    [[nodiscard]] const AnimationController* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return controllers_.size(); }
    void add(std::string name, AnimationController controller);

    // Merges every controller from a document into this set. Throws on malformed
    // input (callers that want a fallback catch and keep their built-ins).
    void loadDocument(const core::Json& document);

    [[nodiscard]] static AnimationControllerSet parse(std::string_view jsonText);

  private:
    std::unordered_map<std::string, AnimationController> controllers_;
};

// Resolves a named bone-mask group to a mask pointer (or nullptr for the whole
// body / an unknown name). The pointer must outlive any Animator::evaluate the
// controller feeds. buildBoneGroups plus this adapter is the usual source.
using MaskResolver = std::function<const BoneMask*(std::string_view)>;

// Per-entity runtime for one controller: it owns the current state and the
// crossfade in flight, advances transitions each frame from a Molang context,
// and queues the active state's animations onto an Animator.
class AnimationControllerInstance final {
  public:
    AnimationControllerInstance() = default;
    explicit AnimationControllerInstance(const AnimationController& controller);

    // Rebinds to a controller, resetting to its initial state.
    void bind(const AnimationController& controller);

    [[nodiscard]] const std::string& currentState() const { return currentState_; }
    [[nodiscard]] const std::string& previousState() const { return previousState_; }
    // The crossfade weight of the state being entered, 0..1 (1 when settled).
    [[nodiscard]] float blendProgress() const { return blend_.value(); }
    [[nodiscard]] bool transitioning() const { return !blend_.finished(); }

    // Advances by `deltaSeconds`: evaluates the current state's transitions (the
    // first truthy condition wins) and starts a crossfade to that target, then
    // advances any in-flight crossfade. `context` supplies variable.*/query.*.
    void update(float deltaSeconds, const MolangContext& context);

    // Queues the active state(s) onto `animator`, resolving clip names via
    // `library` and mask names via `resolveMask`. During a crossfade the state
    // being left fades out (weight * (1 - progress)) while the state being
    // entered fades in (weight * progress). Weights come from each animation's
    // Molang expression evaluated against `context`.
    void apply(Animator& animator, const AnimationLibrary& library,
               const MaskResolver& resolveMask, const MolangContext& context) const;

  private:
    void queueState(Animator& animator, const AnimationLibrary& library,
                    const MaskResolver& resolveMask, const MolangContext& context,
                    const ControllerState& state, float stateWeight) const;

    const AnimationController* controller_ = nullptr;
    std::string currentState_;
    std::string previousState_;
    Transition blend_{1.0F, 1.0F, 0.0F}; // settled by default
};

// The built-in player controller set (idle/walk/sneak). Authored in C++ so the
// game animates and tests run without any resource pack; `load()` overrides it.
[[nodiscard]] AnimationControllerSet builtinPlayerControllers();

// Loads the player controllers from
// `<animationDirectory>/player.animation_controllers.json`, falling back to the
// built-in set if the file is missing, unreadable or malformed (REGULAR §7:
// built-in one copy, resources override, broken files revert to built-in).
[[nodiscard]] AnimationControllerSet
loadPlayerControllers(const std::filesystem::path& animationDirectory);

} // namespace mc::animation
