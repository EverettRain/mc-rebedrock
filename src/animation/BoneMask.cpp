#include "animation/BoneMask.hpp"

#include "animation/SkeletalModel.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace mc::animation {

namespace {

[[nodiscard]] std::string toLower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Adds every bone whose lowercased name matches `predicate`, together with its
// subtree, into `mask`.
template <typename Predicate>
void addNamed(BoneMask& mask, const SkeletalModel& model, Predicate predicate) {
    const auto& bones = model.bones();
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        if (predicate(toLower(bones[i].name))) {
            mask.addSubtree(model, static_cast<int>(i));
        }
    }
}

} // namespace

void BoneMask::set(int bone, bool value) {
    if (bone < 0 || static_cast<std::size_t>(bone) >= bits_.size()) {
        return;
    }
    bits_[static_cast<std::size_t>(bone)] = value;
}

bool BoneMask::test(int bone) const {
    if (bone < 0 || static_cast<std::size_t>(bone) >= bits_.size()) {
        return false;
    }
    return bits_[static_cast<std::size_t>(bone)];
}

std::size_t BoneMask::count() const {
    return static_cast<std::size_t>(std::count(bits_.begin(), bits_.end(), true));
}

void BoneMask::addSubtree(const SkeletalModel& model, int bone) {
    if (bone < 0 || static_cast<std::size_t>(bone) >= model.boneCount()) {
        return;
    }
    if (bits_.size() != model.boneCount()) {
        bits_.assign(model.boneCount(), false);
    }
    set(bone);
    // Walk descendants by parent link: a bone belongs to the subtree iff walking
    // its parent chain reaches `bone`. Bones are laid out roots-first only by
    // convention, so we chase the chain rather than assume ordering.
    const auto& bones = model.bones();
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        for (int p = bones[i].parent; p >= 0; p = bones[static_cast<std::size_t>(p)].parent) {
            if (p == bone) {
                set(static_cast<int>(i));
                break;
            }
        }
    }
}

void BoneMask::addSubtree(const SkeletalModel& model, std::string_view boneName) {
    addSubtree(model, model.findBone(boneName));
}

BoneGroups buildBoneGroups(const SkeletalModel& model) {
    BoneGroups groups;
    groups.head = BoneMask{model.boneCount()};
    groups.arms = BoneMask{model.boneCount()};
    groups.torso = BoneMask{model.boneCount()};
    groups.legs = BoneMask{model.boneCount()};
    groups.upperBody = BoneMask{model.boneCount()};
    groups.lowerBody = BoneMask{model.boneCount()};

    addNamed(groups.head, model, [](const std::string& n) { return contains(n, "head"); });
    addNamed(groups.arms, model,
             [](const std::string& n) { return contains(n, "arm") || contains(n, "hand"); });
    addNamed(groups.torso, model, [](const std::string& n) {
        return contains(n, "body") || contains(n, "torso") || contains(n, "chest") ||
               contains(n, "spine") || contains(n, "waist");
    });
    addNamed(groups.legs, model, [](const std::string& n) {
        return contains(n, "leg") || contains(n, "foot") || n == "root" || contains(n, "pelvis");
    });

    // upper = head ∪ arms ∪ torso ; lower = legs. Built by union so a bone
    // classified into more than one group (e.g. an arm parented to the torso)
    // lands in upper via both routes without double-counting.
    for (std::size_t i = 0U; i < model.boneCount(); ++i) {
        const int b = static_cast<int>(i);
        if (groups.head.test(b) || groups.arms.test(b) || groups.torso.test(b)) {
            groups.upperBody.set(b);
        }
        if (groups.legs.test(b)) {
            groups.lowerBody.set(b);
        }
    }
    return groups;
}

BoneMask headMask(const SkeletalModel& model) { return buildBoneGroups(model).head; }
BoneMask armsMask(const SkeletalModel& model) { return buildBoneGroups(model).arms; }
BoneMask upperBodyMask(const SkeletalModel& model) { return buildBoneGroups(model).upperBody; }
BoneMask lowerBodyMask(const SkeletalModel& model) { return buildBoneGroups(model).lowerBody; }

} // namespace mc::animation
