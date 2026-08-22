#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace mc::animation {

class SkeletalModel;

// A per-bone selection: a dense bit per skeleton bone. An animation layer that
// carries a mask only writes the bones the mask selects, so a walk cycle can own
// the legs while an item-hold pose owns the arms without either touching the
// other (Bedrock's avatar-mask / layered-animation semantics).
//
// Deliberately a value type over a dense bone-index bit vector: masking is a
// single `test(bone)` in the blend loop with zero allocation. An empty mask (no
// bones, e.g. default-constructed) selects nothing; callers that want "all
// bones" pass a null mask pointer to Animator::addLayer instead, which keeps the
// pre-mask whole-skeleton path byte-for-byte unchanged.
class BoneMask final {
  public:
    BoneMask() = default;
    explicit BoneMask(std::size_t boneCount) : bits_(boneCount, false) {}

    [[nodiscard]] std::size_t size() const { return bits_.size(); }

    // Selects/deselects a bone. Out-of-range indices are ignored so a mask built
    // for one skeleton never reads past a clip that references a foreign bone.
    void set(int bone, bool value = true);

    // True when `bone` is selected. Out-of-range (including a negative "not
    // found" index) is always false.
    [[nodiscard]] bool test(int bone) const;

    // Number of selected bones (mainly for tests/asserts).
    [[nodiscard]] std::size_t count() const;

    // Adds `bone` and every bone in its subtree (children, grandchildren, …)
    // according to `model`'s parent hierarchy. This is how a named group grows:
    // masking "head" also masks a hat bone parented to it.
    void addSubtree(const SkeletalModel& model, int bone);

    // Convenience: resolve `boneName` and add its subtree; no-op if absent.
    void addSubtree(const SkeletalModel& model, std::string_view boneName);

  private:
    std::vector<bool> bits_;
};

// Named bone groups derived from a skeleton's bone names + hierarchy. These
// mirror Bedrock's upper/lower-body split so the same mask factory serves the
// player and every mob that reuses the humanoid layout.
//
// Classification is by (case-insensitive substring) bone name, then each matched
// bone's whole subtree is included:
//   * head  : names containing "head".
//   * arms  : names containing "arm" or "hand".
//   * legs  : names containing "leg" or "foot", plus a "root"/"waist"/"pelvis".
//   * torso : names containing "body"/"torso"/"chest"/"spine"/"waist".
// upperBody = head ∪ arms ∪ torso; lowerBody = legs.
struct BoneGroups final {
    BoneMask head;
    BoneMask arms;
    BoneMask torso;
    BoneMask legs;
    BoneMask upperBody;
    BoneMask lowerBody;
};

[[nodiscard]] BoneGroups buildBoneGroups(const SkeletalModel& model);

// Individual named-group builders (each returns a mask sized to the model).
[[nodiscard]] BoneMask headMask(const SkeletalModel& model);
[[nodiscard]] BoneMask armsMask(const SkeletalModel& model);
[[nodiscard]] BoneMask upperBodyMask(const SkeletalModel& model);
[[nodiscard]] BoneMask lowerBodyMask(const SkeletalModel& model);

} // namespace mc::animation
