#include "gameplay/entities/MobBrain.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace mc::gameplay {
namespace {

[[nodiscard]] float horizontalDistanceSquared(glm::vec3 first, glm::vec3 second) {
    const float x = first.x - second.x;
    const float z = first.z - second.z;
    return x * x + z * z;
}

} // namespace

std::optional<glm::vec3> MobAiContext::actorPosition(ActorReference actor) const {
    if (actor.kind == ActorReference::Kind::Player) {
        return player_.present ? std::optional<glm::vec3>{player_.position} : std::nullopt;
    }
    if (actor.kind == ActorReference::Kind::Entity) {
        for (const auto& entity : entities_) {
            if (entity.id == actor.entityId && !entity.dead()) {
                return entity.position;
            }
        }
    }
    return std::nullopt;
}

bool MobAiContext::canSee(const SimpleEntity& observer, ActorReference actor) const {
    const auto targetFeet = actorPosition(actor);
    if (!targetFeet.has_value()) {
        return false;
    }
    float targetHeight = player_.height;
    if (actor.kind == ActorReference::Kind::Entity) {
        for (const auto& entity : entities_) {
            if (entity.id == actor.entityId) {
                targetHeight = entity.dimensions().height;
                break;
            }
        }
    }
    const glm::vec3 origin =
        observer.position + glm::vec3{0.0F, observer.dimensions().height * 0.85F, 0.0F};
    const glm::vec3 target = *targetFeet + glm::vec3{0.0F, targetHeight * 0.65F, 0.0F};
    const glm::vec3 delta = target - origin;
    const float distance = glm::length(delta);
    if (distance < 0.001F) {
        return true;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(distance * 4.0F)));
    for (int step = 1; step < steps; ++step) {
        const glm::vec3 sample =
            origin + delta * (static_cast<float>(step) / static_cast<float>(steps));
        if (world::hasCollision(world_.block(static_cast<int>(std::floor(sample.x)),
                                             static_cast<int>(std::floor(sample.y)),
                                             static_cast<int>(std::floor(sample.z))))) {
            return false;
        }
    }
    return true;
}

namespace entities {
namespace {

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] bool chunkLoaded(const world::World& world, int x, int z) {
    return world.hasChunk({floorDiv(x, world::kChunkWidth), floorDiv(z, world::kChunkDepth)});
}

[[nodiscard]] bool defaultStandable(const world::World& world, const SimpleEntity& self,
                                    glm::ivec3 feet) {
    if (feet.y <= 0 || feet.y >= world::kWorldHeight) {
        return false;
    }

    // A node represents the entity centred at x/z + 0.5. Test every block cell
    // touched by that square footprint rather than only the centre column, so a
    // future wide mob cannot plan through a one-block corridor or clip a corner.
    constexpr float kBoundsEpsilon = 0.0001F;
    const float halfWidth = self.dimensions().width * 0.5F;
    const float centreX = static_cast<float>(feet.x) + 0.5F;
    const float centreZ = static_cast<float>(feet.z) + 0.5F;
    const int minimumX = static_cast<int>(std::floor(centreX - halfWidth + kBoundsEpsilon));
    const int maximumX = static_cast<int>(std::floor(centreX + halfWidth - kBoundsEpsilon));
    const int minimumZ = static_cast<int>(std::floor(centreZ - halfWidth + kBoundsEpsilon));
    const int maximumZ = static_cast<int>(std::floor(centreZ + halfWidth - kBoundsEpsilon));
    const int bodyCells = std::max(1, static_cast<int>(std::ceil(self.dimensions().height)));

    for (int z = minimumZ; z <= maximumZ; ++z) {
        for (int x = minimumX; x <= maximumX; ++x) {
            if (!chunkLoaded(world, x, z) || !world::hasCollision(world.block(x, feet.y - 1, z))) {
                return false;
            }
            for (int offset = 0; offset < bodyCells; ++offset) {
                if (feet.y + offset >= world::kWorldHeight ||
                    world::hasCollision(world.block(x, feet.y + offset, z))) {
                    return false;
                }
            }
        }
    }
    return true;
}

struct PathNode final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const PathNode&) const = default;
};

struct PathNodeHash final {
    [[nodiscard]] std::size_t operator()(const PathNode& node) const noexcept {
        std::size_t value = std::hash<int>{}(node.x);
        value ^= std::hash<int>{}(node.y) + 0x9E3779B9U + (value << 6U) + (value >> 2U);
        value ^= std::hash<int>{}(node.z) + 0x9E3779B9U + (value << 6U) + (value >> 2U);
        return value;
    }
};

[[nodiscard]] float heuristic(PathNode from, PathNode to) {
    const float x = static_cast<float>(from.x - to.x);
    const float y = static_cast<float>(from.y - to.y);
    const float z = static_cast<float>(from.z - to.z);
    return std::sqrt(x * x + y * y + z * z);
}

[[nodiscard]] bool touchingWater(const world::World& world, const SimpleEntity& self) {
    const int x = static_cast<int>(std::floor(self.position.x));
    const int y = static_cast<int>(std::floor(self.position.y));
    const int z = static_cast<int>(std::floor(self.position.z));
    return world::isFluid(world.block(x, y, z)) || world::isFluid(world.block(x, y + 1, z));
}

[[nodiscard]] glm::vec3 randomLandTarget(SimpleEntity& self, int radius) {
    const float angle = randomUnit(self.rngState) * 6.28318530718F;
    const float distance = 3.0F + randomUnit(self.rngState) * static_cast<float>(radius - 3);
    return self.position + glm::vec3{std::sin(angle) * distance, 0.0F, std::cos(angle) * distance};
}

[[nodiscard]] glm::ivec3 feetCellAt(glm::vec3 position) {
    return {static_cast<int>(std::floor(position.x)),
            static_cast<int>(std::floor(position.y + 0.05F)),
            static_cast<int>(std::floor(position.z))};
}

// Navigation nodes are safe, but the velocity integrator can carry a mob past
// the centre of its final node or cut toward an unsupported corner. Recheck a
// short projected step so ordinary locomotion accepts level ground, a one-block
// climb or a one-block drop, but not a cliff.
[[nodiscard]] bool safeProjectedStep(const world::World& world, const SimpleEntity& self,
                                     const GroundNodeEvaluator& evaluator,
                                     glm::vec3 projectedPosition) {
    const glm::ivec3 projected = feetCellAt(projectedPosition);
    for (const int deltaY : {0, 1, -1}) {
        if (evaluator.isStandable(world, self, projected + glm::ivec3{0, deltaY, 0})) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<GroundNodeEvaluation> GroundNodeEvaluator::evaluate(const world::World& world,
                                                                  const SimpleEntity& self,
                                                                  glm::ivec3 feet) const {
    if (!defaultStandable(world, self, feet)) {
        return std::nullopt;
    }
    return GroundNodeEvaluation{feet, GroundNodeType::Walkable, 0.0F};
}

std::optional<GroundNodeEvaluation> GroundNodeEvaluator::standableNear(const world::World& world,
                                                                       const SimpleEntity& self,
                                                                       glm::ivec3 requested,
                                                                       int verticalRange) const {
    for (int delta = 0; delta <= verticalRange; ++delta) {
        const std::array<int, 2> candidates{delta, -delta};
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (delta == 0 && index == 1U) {
                continue;
            }
            glm::ivec3 candidate = requested;
            candidate.y += candidates[index];
            if (auto evaluation = evaluate(world, self, candidate)) {
                return evaluation;
            }
        }
    }
    return std::nullopt;
}

std::optional<GroundNodeEvaluation> GroundNodeEvaluator::successor(const world::World& world,
                                                                   const SimpleEntity& self,
                                                                   glm::ivec3 current,
                                                                   glm::ivec2 direction) const {
    // Match the current land capabilities: prefer level travel, then a
    // one-block climb, then a one-block safe drop. A later evaluator can apply
    // species step/fall limits without changing the A* loop.
    for (const int deltaY : {0, 1, -1}) {
        const glm::ivec3 candidate{current.x + direction.x, current.y + deltaY,
                                   current.z + direction.y};
        if (auto evaluation = evaluate(world, self, candidate)) {
            return evaluation;
        }
    }
    return std::nullopt;
}

GroundPathSearchResult GroundPathFinder::find(const world::World& world, const SimpleEntity& self,
                                              const GroundNodeEvaluator& evaluator,
                                              glm::ivec3 requestedTarget) const {
    GroundPathSearchResult result;
    const auto startCell = evaluator.standableNear(world, self, feetCellAt(self.position), 2);
    const auto targetCell = evaluator.standableNear(world, self, requestedTarget);
    if (!startCell.has_value() || !targetCell.has_value()) {
        return result;
    }

    const PathNode start{startCell->position.x, startCell->position.y, startCell->position.z};
    const PathNode target{targetCell->position.x, targetCell->position.y, targetCell->position.z};
    if (start == target) {
        result.stats.reachedTarget = true;
        return result;
    }

    struct QueueEntry final {
        float score = 0.0F;
        PathNode node;
        [[nodiscard]] bool operator<(const QueueEntry& other) const { return score > other.score; }
    };
    struct Record final {
        float cost = std::numeric_limits<float>::infinity();
        PathNode parent{};
        bool hasParent = false;
        bool closed = false;
    };

    std::priority_queue<QueueEntry> open;
    std::unordered_map<PathNode, Record, PathNodeHash> records;
    records[start].cost = 0.0F;
    open.push({heuristic(start, target), start});

    constexpr std::array<glm::ivec2, 8> kDirections{{
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
        {1, 1},
        {1, -1},
        {-1, 1},
        {-1, -1},
    }};

    while (!open.empty() && result.stats.expandedNodes < maximumExpandedNodes_) {
        const PathNode current = open.top().node;
        open.pop();
        auto& currentRecord = records[current];
        if (currentRecord.closed) {
            continue;
        }
        currentRecord.closed = true;
        const float currentCost = currentRecord.cost;
        ++result.stats.expandedNodes;
        if (current == target) {
            result.stats.reachedTarget = true;
            break;
        }

        const glm::ivec3 currentPosition{current.x, current.y, current.z};
        for (const glm::ivec2 direction : kDirections) {
            ++result.stats.evaluatedSuccessors;
            const auto next = evaluator.successor(world, self, currentPosition, direction);
            if (!next.has_value()) {
                continue;
            }

            const bool diagonal = direction.x != 0 && direction.y != 0;
            if (diagonal) {
                // Vanilla's LandPathNodeMaker only accepts a diagonal when the
                // two adjacent cardinal successors exist and do not climb. This
                // prevents cutting between touching walls or across a ledge.
                const auto sideX =
                    evaluator.successor(world, self, currentPosition, glm::ivec2{direction.x, 0});
                const auto sideZ =
                    evaluator.successor(world, self, currentPosition, glm::ivec2{0, direction.y});
                if (!sideX.has_value() || !sideZ.has_value() || sideX->position.y > current.y ||
                    sideZ->position.y > current.y) {
                    continue;
                }
            }

            const PathNode neighbor{next->position.x, next->position.y, next->position.z};
            auto& neighborRecord = records[neighbor];
            if (neighborRecord.closed) {
                continue;
            }
            const float stepCost = heuristic(current, neighbor) + std::max(next->penalty, 0.0F);
            const float newCost = currentCost + stepCost;
            if (newCost >= neighborRecord.cost) {
                continue;
            }
            neighborRecord.cost = newCost;
            neighborRecord.parent = current;
            neighborRecord.hasParent = true;
            open.push({newCost + heuristic(neighbor, target), neighbor});
        }
    }

    if (!result.stats.reachedTarget) {
        return result;
    }
    PathNode current = target;
    while (!(current == start)) {
        result.nodes.push_back({current.x, current.y, current.z});
        const auto foundRecord = records.find(current);
        if (foundRecord == records.end() || !foundRecord->second.hasParent) {
            result.nodes.clear();
            result.stats.reachedTarget = false;
            return result;
        }
        current = foundRecord->second.parent;
    }
    std::reverse(result.nodes.begin(), result.nodes.end());
    return result;
}

void GoalSelector::add(int priority, std::unique_ptr<MobGoal> goal) {
    goals_.push_back({priority, std::move(goal), false});
}

void GoalSelector::stop(Entry& entry, SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    if (!entry.running) {
        return;
    }
    entry.goal->stop(self, context, brain);
    entry.running = false;
}

void GoalSelector::tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    for (auto& entry : goals_) {
        if (entry.running && !entry.goal->shouldContinue(self, context, brain)) {
            stop(entry, self, context, brain);
        }
    }

    for (auto& candidate : goals_) {
        if (candidate.running) {
            continue;
        }
        std::vector<Entry*> conflicts;
        bool canStart = true;
        for (auto& running : goals_) {
            if (!running.running || (running.goal->controls() & candidate.goal->controls()) == 0U) {
                continue;
            }
            if (running.priority <= candidate.priority || !running.goal->canStop()) {
                canStart = false;
                break;
            }
            conflicts.push_back(&running);
        }
        if (!canStart) {
            continue;
        }
        if (!candidate.goal->canStart(self, context, brain)) {
            continue;
        }
        for (Entry* conflict : conflicts) {
            stop(*conflict, self, context, brain);
        }
        candidate.running = true;
        candidate.goal->start(self, context, brain);
    }

    for (auto& entry : goals_) {
        if (entry.running) {
            entry.goal->tick(self, context, brain);
        }
    }
}

void GoalSelector::stopAll(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    for (auto& entry : goals_) {
        stop(entry, self, context, brain);
    }
}

bool GoalSelector::isRunning(std::string_view name) const {
    return std::any_of(goals_.begin(), goals_.end(), [name](const Entry& entry) {
        return entry.running && entry.goal->name() == name;
    });
}

GroundNavigation::GroundNavigation() : GroundNavigation(std::make_unique<GroundNodeEvaluator>()) {}

GroundNavigation::GroundNavigation(std::unique_ptr<GroundNodeEvaluator> evaluator)
    : evaluator_(evaluator != nullptr ? std::move(evaluator)
                                      : std::make_unique<GroundNodeEvaluator>()) {}

void GroundNavigation::recordSearch(const GroundPathSearchStats& stats) const {
    ++cumulativeSearchStats_.searches;
    cumulativeSearchStats_.expandedNodes += stats.expandedNodes;
    cumulativeSearchStats_.evaluatedSuccessors += stats.evaluatedSuccessors;
}

bool GroundNavigation::startMovingTo(const world::World& world, const SimpleEntity& self,
                                     glm::vec3 target, float speedMultiplier) {
    auto search = pathFinder_.find(world, self, *evaluator_, feetCellAt(target));
    recordSearch(search.stats);
    path_ = std::move(search.nodes);
    lastSearchStats_ = search.stats;
    pathIndex_ = 0U;
    speedMultiplier_ = std::max(speedMultiplier, 0.0F);
    stuckTicks_ = 0;
    lastProgressPosition_ = self.position;
    destination_ =
        path_.empty()
            ? std::nullopt
            : std::optional<glm::vec3>{glm::vec3{path_.back()} + glm::vec3{0.5F, 0.001F, 0.5F}};
    return !path_.empty();
}

bool GroundNavigation::canReach(const world::World& world, const SimpleEntity& self,
                                glm::vec3 target) const {
    const auto search = pathFinder_.find(world, self, *evaluator_, feetCellAt(target));
    recordSearch(search.stats);
    return search.stats.reachedTarget;
}

bool GroundNavigation::canStandAt(const world::World& world, const SimpleEntity& self,
                                  glm::vec3 target) const {
    return evaluator_->standableNear(world, self, feetCellAt(target)).has_value();
}

void GroundNavigation::tick(const world::World& world, SimpleEntity& self) {
    if (isIdle()) {
        return;
    }
    while (pathIndex_ < path_.size()) {
        const glm::vec3 waypoint = glm::vec3{path_[pathIndex_]} + glm::vec3{0.5F, 0.001F, 0.5F};
        const float nodeReachProximity = self.dimensions().width > 0.75F
                                             ? self.dimensions().width * 0.5F
                                             : 0.75F - self.dimensions().width * 0.5F;
        if (std::abs(self.position.x - waypoint.x) >= nodeReachProximity ||
            std::abs(self.position.z - waypoint.z) >= nodeReachProximity ||
            std::abs(self.position.y - waypoint.y) >= 1.0F) {
            break;
        }
        ++pathIndex_;
    }
    if (isIdle()) {
        // Do not let the locomotion velocity carry the mob beyond the final
        // safe path node and over an adjacent edge.
        self.velocity.x = 0.0F;
        self.velocity.z = 0.0F;
        stop(self);
        return;
    }

    const glm::vec3 waypoint = glm::vec3{path_[pathIndex_]} + glm::vec3{0.5F, 0.001F, 0.5F};
    const glm::vec3 delta = waypoint - self.position;
    self.yaw = std::atan2(delta.x, delta.z);
    const glm::vec3 projected =
        self.position + glm::vec3{self.velocity.x, 0.0F, self.velocity.z} +
        glm::vec3{std::sin(self.yaw) * 0.2F, 0.0F, std::cos(self.yaw) * 0.2F};
    if (!safeProjectedStep(world, self, *evaluator_, projected)) {
        self.velocity.x = 0.0F;
        self.velocity.z = 0.0F;
        stop(self);
        return;
    }
    self.moving = true;
    self.movementSpeedMultiplier = speedMultiplier_;

    if (horizontalDistanceSquared(self.position, lastProgressPosition_) < 0.0004F) {
        ++stuckTicks_;
        if (stuckTicks_ >= 40) {
            stop(self);
        }
    } else {
        stuckTicks_ = 0;
        lastProgressPosition_ = self.position;
    }
}

void GroundNavigation::stop(SimpleEntity& self) {
    path_.clear();
    pathIndex_ = 0U;
    destination_.reset();
    stuckTicks_ = 0;
    self.moving = false;
    self.movementSpeedMultiplier = 1.0F;
}

void MobBrain::tick(SimpleEntity& self, MobAiContext& context) {
    attackRequest_.reset();
    targets_.tick(self, context, *this);
    goals_.tick(self, context, *this);
    navigation_.tick(context.world(), self);
}

void MobBrain::stop(SimpleEntity& self, MobAiContext& context) {
    targets_.stopAll(self, context, *this);
    goals_.stopAll(self, context, *this);
    navigation_.stop(self);
    combatTarget_ = {};
    attackRequest_.reset();
}

void MobBrain::clearCombatTarget(ActorReference expected) {
    if (combatTarget_ == expected) {
        combatTarget_ = {};
    }
}

void MobBrain::requestAttack(ActorReference target, float amount) {
    if (target.valid() && amount > 0.0F) {
        attackRequest_ = AttackRequest{target, amount};
    }
}

std::optional<MobBrain::AttackRequest> MobBrain::takeAttackRequest() {
    auto request = attackRequest_;
    attackRequest_.reset();
    return request;
}

bool ActiveTargetPlayerGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    const auto& player = context.player();
    if (!player.present || !player.alive || player.creative ||
        nextRandom(self.rngState) % 10U != 0U) {
        return false;
    }
    const float followRange = self.kind().attributes().followRange;
    const glm::vec3 delta = player.position - self.position;
    return glm::dot(delta, delta) <= followRange * followRange &&
           context.canSee(self, ActorReference::player());
}

bool ActiveTargetPlayerGoal::shouldContinue(SimpleEntity& self, MobAiContext& context,
                                            MobBrain& brain) {
    const auto& player = context.player();
    if (brain.combatTarget() != ActorReference::player() || !player.present || !player.alive ||
        player.creative) {
        return false;
    }
    const float followRange = self.kind().attributes().followRange;
    const glm::vec3 delta = player.position - self.position;
    if (glm::dot(delta, delta) > followRange * followRange) {
        return false;
    }
    if (context.canSee(self, ActorReference::player())) {
        unseenTicks_ = 0;
    } else {
        ++unseenTicks_;
    }
    return unseenTicks_ <= 60;
}

void ActiveTargetPlayerGoal::start(SimpleEntity&, MobAiContext&, MobBrain& brain) {
    unseenTicks_ = 0;
    brain.setCombatTarget(ActorReference::player());
}

void ActiveTargetPlayerGoal::stop(SimpleEntity&, MobAiContext&, MobBrain& brain) {
    unseenTicks_ = 0;
    brain.clearCombatTarget(ActorReference::player());
}

bool MeleeAttackGoal::targetValid(SimpleEntity& self, MobAiContext& context,
                                  MobBrain& brain) const {
    const ActorReference target = brain.combatTarget();
    const auto position = context.actorPosition(target);
    if (!target.valid() || !position.has_value()) {
        return false;
    }
    if (target.kind == ActorReference::Kind::Player &&
        (!context.player().alive || context.player().creative)) {
        return false;
    }
    const float followRange = self.kind().attributes().followRange;
    const glm::vec3 delta = *position - self.position;
    return glm::dot(delta, delta) <= followRange * followRange;
}

float MeleeAttackGoal::squaredAttackRange(const SimpleEntity& self,
                                          const MobAiContext& context) const {
    const float doubledWidth = self.dimensions().width * 2.0F;
    return doubledWidth * doubledWidth + context.player().width;
}

bool MeleeAttackGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    // Vanilla's MeleeAttackGoal does not attempt an initial path every tick:
    // after one start check it waits 20 ticks before trying again. This bounds
    // the cost of a visible target stranded across a cliff or sealed region.
    if (startCheckCooldownTicks_ > 0) {
        --startCheckCooldownTicks_;
        return false;
    }
    startCheckCooldownTicks_ = 20;
    if (!targetValid(self, context, brain)) {
        return false;
    }
    const auto target = context.actorPosition(brain.combatTarget());
    const glm::vec3 delta = *target - self.position;
    return glm::dot(delta, delta) <= squaredAttackRange(self, context) ||
           brain.navigation().canReach(context.world(), self, *target);
}

bool MeleeAttackGoal::shouldContinue(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    return targetValid(self, context, brain);
}

void MeleeAttackGoal::start(SimpleEntity&, MobAiContext&, MobBrain&) {
    attackCooldownTicks_ = 0;
    repathCooldownTicks_ = 0;
}

void MeleeAttackGoal::stop(SimpleEntity& self, MobAiContext&, MobBrain& brain) {
    brain.navigation().stop(self);
    repathCooldownTicks_ = 0;
}

void MeleeAttackGoal::tick(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    if (attackCooldownTicks_ > 0) {
        --attackCooldownTicks_;
    }
    if (repathCooldownTicks_ > 0) {
        --repathCooldownTicks_;
    }
    const ActorReference target = brain.combatTarget();
    const auto targetPosition = context.actorPosition(target);
    if (!targetPosition.has_value()) {
        return;
    }
    const glm::vec3 delta = *targetPosition - self.position;
    self.lookYaw = std::atan2(delta.x, delta.z);
    const float distanceSquared = glm::dot(delta, delta);
    const bool visible = context.canSee(self, target);
    if (distanceSquared <= squaredAttackRange(self, context) && visible) {
        brain.navigation().stop(self);
        self.yaw = self.lookYaw;
        if (attackCooldownTicks_ <= 0) {
            brain.requestAttack(target, self.kind().attributes().attackDamage);
            attackCooldownTicks_ = 20;
        }
        return;
    }

    const glm::vec3 targetMovement = *targetPosition - lastTargetPosition_;
    if (repathCooldownTicks_ <= 0 || glm::dot(targetMovement, targetMovement) >= 1.0F) {
        const bool started = brain.navigation().startMovingTo(context.world(), self,
                                                              *targetPosition, speedMultiplier_);
        lastTargetPosition_ = *targetPosition;
        repathCooldownTicks_ = 4 + static_cast<int>(nextRandom(self.rngState) % 7U);
        // 1.16.1 adds fifteen ticks when navigation rejects the route, avoiding
        // a cluster of unreachable mobs all rebuilding failed paths at 4–10 tick
        // cadence while still letting them retry if the world changes.
        if (!started) {
            repathCooldownTicks_ += 15;
        }
    }
}

bool SwimGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    return touchingWater(context.world(), self);
}

bool SwimGoal::shouldContinue(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    return touchingWater(context.world(), self);
}

void SwimGoal::start(SimpleEntity& self, MobAiContext&, MobBrain& brain) {
    brain.navigation().stop(self);
}

void SwimGoal::stop(SimpleEntity&, MobAiContext&, MobBrain&) {}

void SwimGoal::tick(SimpleEntity& self, MobAiContext&, MobBrain&) {
    self.moving = true;
    // EntitySystem applies 0.08 gravity after goals. Supplying twice that here
    // leaves a real upward swim impulse in the current simplified fluid model.
    self.velocity.y = std::max(self.velocity.y, 0.16F);
}

bool EscapeDangerGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    if (self.lastHurtSequence == 0U || self.lastHurtSequence == handledHurtSequence_ ||
        self.recentAttackerTicks <= 0) {
        return false;
    }
    const glm::vec3 attacker =
        context.actorPosition(self.lastAttacker).value_or(self.lastAttackerPosition);
    glm::vec3 away = self.position - attacker;
    away.y = 0.0F;
    const float lengthSquared = glm::dot(away, away);
    if (lengthSquared < 1e-6F) {
        const float angle = randomUnit(self.rngState) * 6.28318530718F;
        away = {std::sin(angle), 0.0F, std::cos(angle)};
    } else {
        away /= std::sqrt(lengthSquared);
    }
    // Vanilla's target finder samples several distances and directions. Trying
    // only an exact eight-block endpoint made hits on hills and small plateaus
    // silently fail even when a shorter escape route existed.
    constexpr std::array<float, 7> kOffsets{0.0F, 0.45F, -0.45F, 0.9F, -0.9F, 1.3F, -1.3F};
    constexpr std::array<float, 3> kDistances{8.0F, 6.0F, 4.0F};
    for (const float distance : kDistances) {
        for (const float offset : kOffsets) {
            const float sine = std::sin(offset);
            const float cosine = std::cos(offset);
            const glm::vec3 direction{away.x * cosine - away.z * sine, 0.0F,
                                      away.x * sine + away.z * cosine};
            target_ = self.position + direction * distance;
            if (brain.navigation().canReach(context.world(), self, target_)) {
                return true;
            }
        }
    }
    return false;
}

bool EscapeDangerGoal::shouldContinue(SimpleEntity&, MobAiContext&, MobBrain& brain) {
    return !brain.navigation().isIdle();
}

void EscapeDangerGoal::start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    handledHurtSequence_ = self.lastHurtSequence;
    static_cast<void>(
        brain.navigation().startMovingTo(context.world(), self, target_, speedMultiplier_));
}

void EscapeDangerGoal::stop(SimpleEntity& self, MobAiContext&, MobBrain& brain) {
    brain.navigation().stop(self);
}

bool WanderAroundFarGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    if (!brain.navigation().isIdle() || self.wanderTimer > 0U) {
        return false;
    }
    self.wanderTimer = 40U + nextRandom(self.rngState) % 60U;
    if (randomUnit(self.rngState) < 0.25F) {
        return false;
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        target_ = randomLandTarget(self, 10);
        if (brain.navigation().canStandAt(context.world(), self, target_)) {
            return true;
        }
    }
    return false;
}

bool WanderAroundFarGoal::shouldContinue(SimpleEntity&, MobAiContext&, MobBrain& brain) {
    return !brain.navigation().isIdle();
}

void WanderAroundFarGoal::start(SimpleEntity& self, MobAiContext& context, MobBrain& brain) {
    static_cast<void>(
        brain.navigation().startMovingTo(context.world(), self, target_, speedMultiplier_));
}

void WanderAroundFarGoal::stop(SimpleEntity& self, MobAiContext&, MobBrain& brain) {
    brain.navigation().stop(self);
}

bool LookAtPlayerGoal::canStart(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    if (!context.player().present || randomUnit(self.rngState) >= 0.02F) {
        return false;
    }
    if (horizontalDistanceSquared(self.position, context.player().position) > range_ * range_) {
        return false;
    }
    remainingTicks_ = 40 + static_cast<int>(nextRandom(self.rngState) % 40U);
    return true;
}

bool LookAtPlayerGoal::shouldContinue(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    return remainingTicks_ > 0 && context.player().present &&
           horizontalDistanceSquared(self.position, context.player().position) <= range_ * range_;
}

void LookAtPlayerGoal::tick(SimpleEntity& self, MobAiContext& context, MobBrain&) {
    --remainingTicks_;
    const glm::vec3 delta = context.player().position - self.position;
    self.lookYaw = std::atan2(delta.x, delta.z);
}

bool LookAroundGoal::canStart(SimpleEntity& self, MobAiContext&, MobBrain&) {
    return randomUnit(self.rngState) < 0.02F;
}

bool LookAroundGoal::shouldContinue(SimpleEntity&, MobAiContext&, MobBrain&) {
    return remainingTicks_ > 0;
}

void LookAroundGoal::start(SimpleEntity& self, MobAiContext&, MobBrain&) {
    remainingTicks_ = 20 + static_cast<int>(nextRandom(self.rngState) % 20U);
    lookYaw_ = randomUnit(self.rngState) * 6.28318530718F;
}

void LookAroundGoal::tick(SimpleEntity& self, MobAiContext&, MobBrain&) {
    --remainingTicks_;
    self.lookYaw = lookYaw_;
}

} // namespace entities
} // namespace mc::gameplay
