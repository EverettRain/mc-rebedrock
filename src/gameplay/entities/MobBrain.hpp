#pragma once

#include "gameplay/Inventory.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {
struct SimpleEntity;

// A stable reference used by AI memory. Vector indices are deliberately not
// stored: EntitySystem compacts its entity vector after deaths and despawns.
struct ActorReference final {
    enum class Kind : std::uint8_t {
        None,
        Player,
        Entity,
    };

    Kind kind = Kind::None;
    std::uint64_t entityId = 0U;

    [[nodiscard]] static constexpr ActorReference player() { return {Kind::Player, 0U}; }
    [[nodiscard]] static constexpr ActorReference entity(std::uint64_t id) {
        return {Kind::Entity, id};
    }
    [[nodiscard]] constexpr bool valid() const { return kind != Kind::None; }
    [[nodiscard]] constexpr bool operator==(const ActorReference&) const = default;
};

// The small player-facing view AI consumes. Keeping it separate from
// PlayerController prevents the goal layer from reaching through GameSession.
struct PlayerAiView final {
    glm::vec3 position{0.0F};
    bool present = false;
    bool alive = true;
    bool creative = false;
    float width = 0.6F;
    float height = 1.8F;
    // The player's held stack, so TemptGoal can see whether the player is
    // offering a species' tempt item. Empty when the hand is empty; a TemptGoal
    // matches it against its own attractant, so the goal layer never learns which
    // item any particular species wants — the species states that.
    ItemStack heldItem{};
};

// Read-only sensing context for one AI pass. Actions are written to the mob's
// brain/navigation and applied later by EntitySystem's normal physics path.
class MobAiContext final {
  public:
    MobAiContext(const world::World& world, std::span<const SimpleEntity> entities,
                 PlayerAiView player, std::uint64_t gameTick)
        : world_(world), entities_(entities), player_(player), gameTick_(gameTick) {}
    MobAiContext(const world::World& world, std::span<const SimpleEntity> entities,
                 const std::unordered_map<std::uint64_t, std::size_t>& entityIndex,
                 PlayerAiView player, std::uint64_t gameTick)
        : world_(world), entities_(entities), entityIndex_(&entityIndex), player_(player),
          gameTick_(gameTick) {}

    [[nodiscard]] const world::World& world() const { return world_; }
    [[nodiscard]] const PlayerAiView& player() const { return player_; }
    [[nodiscard]] std::uint64_t gameTick() const { return gameTick_; }
    [[nodiscard]] std::optional<glm::vec3> actorPosition(ActorReference actor) const;
    [[nodiscard]] bool canSee(const SimpleEntity& observer, ActorReference actor) const;
    // The other creatures this pass can sense, read-only. Breeding/follow goals
    // scan it for a nearby partner or parent of the same species. It is the same
    // vector EntitySystem is ticking, so a goal must not store a pointer into it
    // past the pass — it holds a stable id (findPartner/findParent) instead.
    [[nodiscard]] std::span<const SimpleEntity> entities() const { return entities_; }
    [[nodiscard]] const SimpleEntity* entityById(std::uint64_t id) const;

  private:

    const world::World& world_;
    std::span<const SimpleEntity> entities_;
    const std::unordered_map<std::uint64_t, std::size_t>* entityIndex_ = nullptr;
    PlayerAiView player_;
    std::uint64_t gameTick_ = 0U;
};

namespace entities {

enum class GoalControl : std::uint8_t {
    Move = 1U << 0U,
    Look = 1U << 1U,
    Jump = 1U << 2U,
    Target = 1U << 3U,
};

using GoalControls = std::uint8_t;

[[nodiscard]] constexpr GoalControls control(GoalControl value) {
    return static_cast<GoalControls>(value);
}

[[nodiscard]] constexpr GoalControls controls(GoalControl first, GoalControl second) {
    return static_cast<GoalControls>(control(first) | control(second));
}

// Per-entity Goal instance. Concrete goals may safely keep destinations,
// cooldowns and other mutable state because MobBrain owns a distinct instance
// for every creature.
class MobGoal {
  public:
    virtual ~MobGoal() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual GoalControls controls() const = 0;
    [[nodiscard]] virtual bool canStart(SimpleEntity& self, MobAiContext& context,
                                        class MobBrain& brain) = 0;
    [[nodiscard]] virtual bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                              class MobBrain& brain) = 0;
    virtual bool canStop() const { return true; }
    virtual void start(SimpleEntity&, MobAiContext&, class MobBrain&) {}
    virtual void stop(SimpleEntity&, MobAiContext&, class MobBrain&) {}
    virtual void tick(SimpleEntity&, MobAiContext&, class MobBrain&) {}
};

// vanilla GoalSelector semantics: lower numeric priorities may replace higher
// numeric priorities when their MOVE/LOOK/JUMP/TARGET controls conflict.
class GoalSelector final {
  public:
    void add(int priority, std::unique_ptr<MobGoal> goal);
    void tick(SimpleEntity& self, MobAiContext& context, class MobBrain& brain);
    void stopAll(SimpleEntity& self, MobAiContext& context, class MobBrain& brain);

    [[nodiscard]] std::size_t size() const { return goals_.size(); }
    [[nodiscard]] bool isRunning(std::string_view name) const;

  private:
    struct Entry final {
        int priority = 0;
        std::unique_ptr<MobGoal> goal;
        bool running = false;
    };

    void stop(Entry& entry, SimpleEntity& self, MobAiContext& context, class MobBrain& brain);
    std::vector<Entry> goals_;
};

enum class GroundNodeType : std::uint8_t {
    Walkable,
    Blocked,
};

// The result of classifying one potential feet cell. `penalty` is deliberately
// part of the contract even though the basic evaluator currently returns zero:
// doors, water, rails, fire and species-specific hazards can later influence A*
// without changing GroundPathFinder or any Goal.
struct GroundNodeEvaluation final {
    glm::ivec3 position{0};
    GroundNodeType type = GroundNodeType::Blocked;
    float penalty = 0.0F;
};

// PathNodeMaker-style terrain policy. The default implementation handles
// loaded chunks, the entity's complete footprint/headroom, one-block climbs
// and one-block drops. Alternative land policies can override classification
// and successor selection while sharing the path finder and follower.
class GroundNodeEvaluator {
  public:
    virtual ~GroundNodeEvaluator() = default;

    [[nodiscard]] virtual std::optional<GroundNodeEvaluation>
    evaluate(const world::World& world, const SimpleEntity& self, glm::ivec3 feet) const;
    [[nodiscard]] bool isStandable(const world::World& world, const SimpleEntity& self,
                                   glm::ivec3 feet) const {
        return evaluate(world, self, feet).has_value();
    }
    [[nodiscard]] virtual std::optional<GroundNodeEvaluation>
    standableNear(const world::World& world, const SimpleEntity& self, glm::ivec3 requested,
                  int verticalRange = 4) const;
    [[nodiscard]] virtual std::optional<GroundNodeEvaluation> successor(const world::World& world,
                                                                        const SimpleEntity& self,
                                                                        glm::ivec3 current,
                                                                        glm::ivec2 direction) const;
};

struct GroundPathSearchStats final {
    std::size_t expandedNodes = 0U;
    std::size_t evaluatedSuccessors = 0U;
    bool reachedTarget = false;
};

struct GroundPathCumulativeStats final {
    std::uint64_t searches = 0U;
    std::uint64_t expandedNodes = 0U;
    std::uint64_t evaluatedSuccessors = 0U;
};

struct GroundPathSearchResult final {
    std::vector<glm::ivec3> nodes;
    GroundPathSearchStats stats;
};

// Bounded A* independent from terrain classification. It expands eight
// horizontal directions, charges Euclidean step cost, and permits a diagonal
// only when both adjacent cardinal cells are traversable without climbing.
class GroundPathFinder final {
  public:
    explicit GroundPathFinder(std::size_t maximumExpandedNodes = 1024U);

    [[nodiscard]] GroundPathSearchResult find(const world::World& world, const SimpleEntity& self,
                                              const GroundNodeEvaluator& evaluator,
                                              glm::ivec3 requestedTarget) const;

  private:
    struct SearchRecord final {
        glm::ivec3 position{0};
        glm::ivec3 parent{0};
        float cost = 0.0F;
        bool hasParent = false;
        bool closed = false;
    };
    struct HeapEntry final {
        float score = 0.0F;
        std::uint32_t record = 0U;
    };

    void beginSearch() const;
    [[nodiscard]] std::uint32_t findRecord(glm::ivec3 position) const;
    [[nodiscard]] std::uint32_t recordFor(glm::ivec3 position) const;
    void rebuildRecordIndex(std::size_t capacity) const;

    std::size_t maximumExpandedNodes_ = 1024U;
    // Per-navigation A* scratch. Capacity survives searches, nodes live in
    // contiguous arrays, and the packed-coordinate index uses generations to
    // clear logically without touching or freeing the whole table.
    mutable std::vector<SearchRecord> records_;
    mutable std::vector<HeapEntry> openHeap_;
    mutable std::vector<std::uint64_t> recordKeys_;
    mutable std::vector<std::uint32_t> recordValues_;
    mutable std::vector<std::uint32_t> recordGenerations_;
    mutable std::uint32_t searchGeneration_ = 0U;
};

// Ordinary land navigation: selects a path with GroundPathFinder, then emits
// movement intent while EntitySystem remains the sole owner of collision and
// velocity integration. Its public movement API stays stable for every Goal.
class GroundNavigation final {
  public:
    GroundNavigation();
    explicit GroundNavigation(std::unique_ptr<GroundNodeEvaluator> evaluator);

    [[nodiscard]] bool startMovingTo(const world::World& world, const SimpleEntity& self,
                                     glm::vec3 target, float speedMultiplier);
    [[nodiscard]] bool canReach(const world::World& world, const SimpleEntity& self,
                                glm::vec3 target) const;
    [[nodiscard]] bool canStandAt(const world::World& world, const SimpleEntity& self,
                                  glm::vec3 target) const;
    void tick(const world::World& world, SimpleEntity& self);
    void stop(SimpleEntity& self);

    [[nodiscard]] bool isIdle() const { return pathIndex_ >= path_.size(); }
    [[nodiscard]] std::size_t pathSize() const { return path_.size(); }
    [[nodiscard]] std::span<const glm::ivec3> pathNodes() const { return path_; }
    [[nodiscard]] std::optional<glm::vec3> destination() const { return destination_; }
    [[nodiscard]] const GroundPathSearchStats& lastSearchStats() const { return lastSearchStats_; }
    [[nodiscard]] const GroundPathCumulativeStats& cumulativeSearchStats() const {
        return cumulativeSearchStats_;
    }

  private:
    void recordSearch(const GroundPathSearchStats& stats) const;

    std::unique_ptr<GroundNodeEvaluator> evaluator_;
    GroundPathFinder pathFinder_;
    std::vector<glm::ivec3> path_;
    std::size_t pathIndex_ = 0U;
    float speedMultiplier_ = 1.0F;
    std::optional<glm::vec3> destination_;
    glm::vec3 lastProgressPosition_{0.0F};
    int stuckTicks_ = 0;
    GroundPathSearchStats lastSearchStats_;
    mutable GroundPathCumulativeStats cumulativeSearchStats_;
};

class MobBrain final {
  public:
    struct AttackRequest final {
        ActorReference target{};
        float amount = 0.0F;
    };

    [[nodiscard]] GoalSelector& goals() { return goals_; }
    [[nodiscard]] const GoalSelector& goals() const { return goals_; }
    [[nodiscard]] GoalSelector& targets() { return targets_; }
    [[nodiscard]] const GoalSelector& targets() const { return targets_; }
    [[nodiscard]] GroundNavigation& navigation() { return navigation_; }
    [[nodiscard]] const GroundNavigation& navigation() const { return navigation_; }
    [[nodiscard]] ActorReference combatTarget() const { return combatTarget_; }
    void setCombatTarget(ActorReference target) { combatTarget_ = target; }
    void clearCombatTarget(ActorReference expected);
    void requestAttack(ActorReference target, float amount);
    [[nodiscard]] std::optional<AttackRequest> takeAttackRequest();

    // AnimalMateGoal emits a breed the same way MeleeAttackGoal emits an attack:
    // the read-only AI pass cannot spawn a baby into the (const) entity span, so
    // it records the partner and EntitySystem resolves it after the pass, when it
    // owns the mutable vector. The lower id of the pair carries the request so a
    // couple breeds once, not twice.
    void requestBreed(std::uint64_t partnerId);
    [[nodiscard]] std::optional<std::uint64_t> takeBreedRequest();

    // EatGrassGoal (AR-A2) emits the block it wants eaten the same way
    // AnimalMateGoal emits a breed: MobAiContext only holds a `const World&`,
    // so the goal cannot write the cell itself, and even a mutable World
    // reference would be the wrong layer — a block edit must flow through
    // WorldMutationService so neighbour/light updates fire (see the mechanism-
    // fix-progress "lost lit" class of bug this is written to avoid). The goal
    // only records which cell it ate; EntitySystem::tick collects the request
    // into EntityTickResult, and GameSession performs the actual setBlock
    // (mirroring how block-drop events already cross from EntitySystem to
    // GameSession's mutation sink).
    void requestEatGrass(glm::ivec3 grassBlock);
    [[nodiscard]] std::optional<glm::ivec3> takeEatGrassRequest();

    void tick(SimpleEntity& self, MobAiContext& context);
    void stop(SimpleEntity& self, MobAiContext& context);

  private:
    GoalSelector goals_;
    GoalSelector targets_;
    GroundNavigation navigation_;
    ActorReference combatTarget_{};
    std::optional<AttackRequest> attackRequest_;
    std::optional<std::uint64_t> breedRequest_;
    std::optional<glm::ivec3> eatGrassRequest_;
};

// ActiveTargetGoal<PlayerEntity>: acquires a living non-creative player inside
// the species follow range and keeps the stable ActorReference in MobBrain.
class ActiveTargetPlayerGoal final : public MobGoal {
  public:
    [[nodiscard]] std::string_view name() const override { return "active_target_player"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Target); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    int unseenTicks_ = 0;
};

// MeleeAttackGoal: follows the Brain's current target, periodically rebuilds a
// ground path, and emits one attack request per vanilla 20-tick cooldown while
// the target is in reach and visible.
class MeleeAttackGoal final : public MobGoal {
  public:
    explicit MeleeAttackGoal(float speedMultiplier) : speedMultiplier_(speedMultiplier) {}

    [[nodiscard]] std::string_view name() const override { return "melee_attack"; }
    [[nodiscard]] GoalControls controls() const override {
        return entities::controls(GoalControl::Move, GoalControl::Look);
    }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    [[nodiscard]] bool targetValid(SimpleEntity& self, MobAiContext& context,
                                   MobBrain& brain) const;
    [[nodiscard]] float squaredAttackRange(const SimpleEntity& self,
                                           const MobAiContext& context) const;

    float speedMultiplier_ = 1.0F;
    int startCheckCooldownTicks_ = 0;
    int attackCooldownTicks_ = 0;
    int repathCooldownTicks_ = 0;
    glm::vec3 lastTargetPosition_{0.0F};
};

class SwimGoal final : public MobGoal {
  public:
    [[nodiscard]] std::string_view name() const override { return "swim"; }
    [[nodiscard]] GoalControls controls() const override {
        return entities::controls(GoalControl::Move, GoalControl::Jump);
    }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
};

class EscapeDangerGoal final : public MobGoal {
  public:
    explicit EscapeDangerGoal(float speedMultiplier) : speedMultiplier_(speedMultiplier) {}

    [[nodiscard]] std::string_view name() const override { return "escape_danger"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Move); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    float speedMultiplier_ = 1.0F;
    std::uint64_t handledHurtSequence_ = 0U;
    glm::vec3 target_{0.0F};
};

class WanderAroundFarGoal final : public MobGoal {
  public:
    explicit WanderAroundFarGoal(float speedMultiplier) : speedMultiplier_(speedMultiplier) {}

    [[nodiscard]] std::string_view name() const override { return "wander_around_far"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Move); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    float speedMultiplier_ = 1.0F;
    glm::vec3 target_{0.0F};
};

class LookAtPlayerGoal final : public MobGoal {
  public:
    explicit LookAtPlayerGoal(float range) : range_(range) {}

    [[nodiscard]] std::string_view name() const override { return "look_at_player"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Look); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    float range_ = 6.0F;
    int remainingTicks_ = 0;
};

class LookAroundGoal final : public MobGoal {
  public:
    [[nodiscard]] std::string_view name() const override { return "look_around"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Look); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    int remainingTicks_ = 0;
    float lookYaw_ = 0.0F;
};

// TemptGoal (26.1): while the player holds this goal's attractant item, the
// animal walks toward the player and stops the moment the hand no longer offers
// it. The attractant is a parameter — the species passes wheat/seeds through its
// breeding profile — so the goal itself is content-free. Adults and babies are
// both temptable, matching vanilla.
class TemptGoal final : public MobGoal {
  public:
    TemptGoal(gameplay::ItemStack attractant, float speedMultiplier, float range)
        : attractant_(attractant), speedMultiplier_(speedMultiplier), range_(range) {}

    [[nodiscard]] std::string_view name() const override { return "tempt"; }
    [[nodiscard]] GoalControls controls() const override {
        return entities::controls(GoalControl::Move, GoalControl::Look);
    }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    [[nodiscard]] bool playerOffering(const MobAiContext& context) const;

    gameplay::ItemStack attractant_{};
    float speedMultiplier_ = 1.0F;
    float range_ = 10.0F;
};

// AnimalMateGoal (26.1): an in-love adult seeks the nearest in-love adult of its
// own species, walks to it, and once the pair is within breeding range asks the
// brain to breed. The lower-id partner carries the request so a couple produces
// one baby, not two. Spawning the baby is EntitySystem's job (this pass is
// read-only); the goal only signals the intent.
class AnimalMateGoal final : public MobGoal {
  public:
    explicit AnimalMateGoal(float speedMultiplier) : speedMultiplier_(speedMultiplier) {}

    [[nodiscard]] std::string_view name() const override { return "animal_mate"; }
    [[nodiscard]] GoalControls controls() const override {
        return entities::controls(GoalControl::Move, GoalControl::Look);
    }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    // The nearest in-love adult of the same species within breeding search range,
    // or an invalid reference. Reads the shared entity span from the context.
    [[nodiscard]] std::uint64_t findPartner(const SimpleEntity& self,
                                            const MobAiContext& context) const;

    float speedMultiplier_ = 1.0F;
    std::uint64_t partnerId_ = 0U;
};

// FollowParentGoal (26.1): a baby walks toward the nearest adult of its own
// species, so newborns trail the herd. Only babies run it; adults never start.
class FollowParentGoal final : public MobGoal {
  public:
    explicit FollowParentGoal(float speedMultiplier) : speedMultiplier_(speedMultiplier) {}

    [[nodiscard]] std::string_view name() const override { return "follow_parent"; }
    [[nodiscard]] GoalControls controls() const override { return control(GoalControl::Move); }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    [[nodiscard]] std::uint64_t findParent(const SimpleEntity& self,
                                           const MobAiContext& context) const;

    float speedMultiplier_ = 1.0F;
    std::uint64_t parentId_ = 0U;
};

// EatGrassGoal (AR-A2, sheep-specific): a sheared sheep that stands over a
// grass-family block lowers its head for kEatDurationTicks, then eats the
// block (grass_block -> dirt, short_grass -> air) and regrows its wool.
// Vanilla's EatBlockGoal only ever looks at the block occupying the mob's feet
// cell (short_grass) and the cell directly below it (grass_block) — no wider
// search — which this mirrors. Deterministic: both the 1-in-1000 roll that
// starts the goal and the eat itself are entirely driven by `self.rngState`,
// never a global RNG or wall clock. The goal only *requests* the eat (see
// MobBrain::requestEatGrass) — the write itself happens where
// WorldMutationService is reachable.
class EatGrassGoal final : public MobGoal {
  public:
    [[nodiscard]] std::string_view name() const override { return "eat_grass"; }
    [[nodiscard]] GoalControls controls() const override {
        return entities::controls(GoalControl::Move, GoalControl::Look);
    }
    [[nodiscard]] bool canStart(SimpleEntity& self, MobAiContext& context,
                                MobBrain& brain) override;
    [[nodiscard]] bool shouldContinue(SimpleEntity& self, MobAiContext& context,
                                      MobBrain& brain) override;
    void start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void stop(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;
    void tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) override;

  private:
    // EatBlockGoal#EAT_ANIMATION_TICKS: forty ticks (two seconds) of the head
    // lowered before the block actually changes.
    static constexpr int kEatDurationTicks = 40;
    int remainingTicks_ = 0;
};

} // namespace entities
} // namespace mc::gameplay
