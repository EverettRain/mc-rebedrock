#pragma once

#include "gameplay/command/ArgumentType.hpp"
#include "gameplay/command/CommandSource.hpp"
#include "gameplay/command/StringReader.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::gameplay::command {

// A target selector variable, mirroring 1.16.1's EntitySelector base sets.
// @s is the executor, @p/@a/@r resolve over players, @e over every entity.
enum class SelectorVariable : std::uint8_t {
    Self,          // @s
    NearestPlayer, // @p
    AllPlayers,    // @a
    AllEntities,   // @e
    Random,        // @r
};

// The order matched candidates are reduced to, 1.16.1's `sort=` (plus the
// default each variable carries). Random draws from a deterministic stream, never
// the wall clock, so `@r` picks the same target for the same seed and state.
enum class SelectorSort : std::uint8_t {
    Arbitrary, // insertion order (the world's entity vector order)
    Nearest,
    Furthest,
    Random,
};

// A candidate the selector reasons about, decoupled from the gameplay player /
// entity structs so the command layer needs neither. The caller fills a flat
// list from its player map and entity vector; the selector is a pure predicate +
// sort + limit over it.
struct SelectorCandidate final {
    bool player = false;
    PlayerId playerId = 0;
    std::uint64_t entityId = 0;
    glm::vec3 position{0.0F};
    const entities::EntityType* type = nullptr; // null for a player
};

// One resolved target: which player or entity, and where. The command applies
// its effect (kill, teleport, give) by this handle.
struct SelectorTarget final {
    bool player = false;
    PlayerId playerId = 0;
    std::uint64_t entityId = 0;
    glm::vec3 position{0.0F};
};

// A parsed selector: the variable plus the filters it carried. It is a value
// (the DOD choice over an object with per-clause virtuals): resolve() runs it as
// one pass over the candidate list — a predicate, then a sort, then a limit — and
// never builds a spatial index, because the entity count is small.
struct EntitySelector final {
    SelectorVariable variable = SelectorVariable::AllEntities;
    // type=[!]<id>: a registered entity id, optionally negated. A player (null
    // type) never matches a species, so `type=<species>` excludes players and a
    // negated filter keeps them.
    std::optional<std::string> typeId;
    bool typeNegated = false;
    // distance=<min>..<max> against the source position (either end optional; a
    // bare value means min == max).
    std::optional<double> distanceMinimum;
    std::optional<double> distanceMaximum;
    // limit=/sort= override the variable's defaults when set.
    std::optional<std::size_t> limit;
    std::optional<SelectorSort> sort;

    // The sort applied when `sort=` was omitted: @p is nearest-first, @r random,
    // everything else keeps arbitrary (insertion) order.
    [[nodiscard]] SelectorSort effectiveSort() const {
        if (sort.has_value()) {
            return *sort;
        }
        switch (variable) {
        case SelectorVariable::NearestPlayer:
            return SelectorSort::Nearest;
        case SelectorVariable::Random:
            return SelectorSort::Random;
        default:
            return SelectorSort::Arbitrary;
        }
    }

    // The cap applied when `limit=` was omitted: the single-target variables
    // (@s/@p/@r) default to one, @a/@e to unlimited.
    [[nodiscard]] std::optional<std::size_t> effectiveLimit() const {
        if (limit.has_value()) {
            return limit;
        }
        switch (variable) {
        case SelectorVariable::Self:
        case SelectorVariable::NearestPlayer:
        case SelectorVariable::Random:
            return std::size_t{1};
        default:
            return std::nullopt;
        }
    }

    [[nodiscard]] std::vector<SelectorTarget> resolve(const CommandSource& source,
                                                      std::span<const SelectorCandidate> candidates,
                                                      std::uint64_t randomSeed) const;
};

// The splitmix64 finaliser (the same constant RedstoneWireEvaluator uses): a
// fixed integer hash that turns a seed into a well-mixed stream, so `@r` is
// deterministic — same seed and candidate order, same pick.
[[nodiscard]] constexpr std::uint64_t selectorMix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

// Parses a double the way the coordinate reader does (std::from_chars, whole
// token). Returns false when the text is not exactly a number.
[[nodiscard]] inline bool parseSelectorDouble(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

// True when a candidate's species matches `id` by either its rebedrock id or its
// vanilla alias (the same tolerance `/kill <species>` had). A player (null type)
// matches nothing.
[[nodiscard]] inline bool candidateSpeciesMatches(const SelectorCandidate& candidate,
                                                  std::string_view id) {
    return candidate.type != nullptr &&
           (candidate.type->id().matches(id) || candidate.type->vanillaId().matches(id));
}

inline std::vector<SelectorTarget> EntitySelector::resolve(
    const CommandSource& source, std::span<const SelectorCandidate> candidates,
    std::uint64_t randomSeed) const {
    // 1. The base set the variable draws from. @s is the executor itself (a
    //    player in single-player), synthesised so it needs no lookup; @p/@a draw
    //    from players; @r from players unless a species filter widens it to every
    //    entity; @e from everything.
    std::vector<SelectorCandidate> pool;
    if (variable == SelectorVariable::Self) {
        if (source.executorIsEntity) {
            // The executor is a mob (execute as <entity>): look it up in the live
            // set so @s carries its real position and species (and yields nothing
            // if it has since died). Never synthesise it — a stale position would
            // make `@s` lie.
            for (const SelectorCandidate& candidate : candidates) {
                if (!candidate.player && candidate.entityId == source.entityId) {
                    pool.push_back(candidate);
                    break;
                }
            }
        } else {
            SelectorCandidate self;
            self.player = true;
            self.playerId = source.playerId;
            self.position = source.position;
            pool.push_back(self);
        }
    } else {
        const bool playersOnly =
            (variable == SelectorVariable::NearestPlayer ||
             variable == SelectorVariable::AllPlayers ||
             (variable == SelectorVariable::Random && !typeId.has_value()));
        for (const SelectorCandidate& candidate : candidates) {
            if (playersOnly && !candidate.player) {
                continue;
            }
            pool.push_back(candidate);
        }
    }

    // 2. Predicate: species and distance, both against the source.
    std::vector<SelectorCandidate> matched;
    matched.reserve(pool.size());
    for (const SelectorCandidate& candidate : pool) {
        if (typeId.has_value()) {
            const bool speciesMatch = candidateSpeciesMatches(candidate, *typeId);
            if (speciesMatch == typeNegated) { // negated flips the accept
                continue;
            }
        }
        if (distanceMinimum.has_value() || distanceMaximum.has_value()) {
            const double distance = glm::length(candidate.position - source.position);
            if (distanceMinimum.has_value() && distance < *distanceMinimum) {
                continue;
            }
            if (distanceMaximum.has_value() && distance > *distanceMaximum) {
                continue;
            }
        }
        matched.push_back(candidate);
    }

    // 3. Sort. Nearest/furthest key on the source distance (stable, so equal
    //    distances keep the deterministic candidate order); random is a
    //    seed-driven Fisher-Yates over the matched indices.
    const SelectorSort order = effectiveSort();
    if (order == SelectorSort::Nearest || order == SelectorSort::Furthest) {
        const glm::vec3 origin = source.position;
        std::stable_sort(matched.begin(), matched.end(),
                         [origin, order](const SelectorCandidate& left,
                                         const SelectorCandidate& right) {
                             const float leftDistance = glm::length(left.position - origin);
                             const float rightDistance = glm::length(right.position - origin);
                             return order == SelectorSort::Nearest ? leftDistance < rightDistance
                                                                   : leftDistance > rightDistance;
                         });
    } else if (order == SelectorSort::Random) {
        std::uint64_t state = selectorMix(randomSeed);
        for (std::size_t index = matched.size(); index > 1; --index) {
            state = selectorMix(state);
            const std::size_t swap = static_cast<std::size_t>(state % index);
            std::swap(matched[index - 1U], matched[swap]);
        }
    }

    // 4. Limit, then hand back targets.
    const auto cap = effectiveLimit();
    std::vector<SelectorTarget> targets;
    for (const SelectorCandidate& candidate : matched) {
        if (cap.has_value() && targets.size() >= *cap) {
            break;
        }
        targets.push_back(
            {candidate.player, candidate.playerId, candidate.entityId, candidate.position});
    }
    return targets;
}

// Parses one selector token (`@e[type=cow,distance=..5,limit=1,sort=nearest]`)
// from the reader, positioned at the leading `@`. The filter block's punctuation
// (`[ ] = , !`) is outside the unquoted-string character set, so this scans it by
// hand instead of leaning on readString. Supports type/distance/limit/sort;
// name/tag/x/y/z/dx/dy/dz/level and scores/nbt are rejected with a clear message
// rather than silently ignored (they have no backing system yet).
[[nodiscard]] inline ArgumentParseResult parseEntitySelector(StringReader& reader) {
    if (!reader.canRead() || reader.peek() != '@') {
        return parseFail("Expected a selector (@s, @p, @a, @e, @r)", reader);
    }
    reader.skip(); // '@'
    if (!reader.canRead()) {
        return parseFail("Expected a selector variable after @", reader);
    }
    EntitySelector selector;
    switch (reader.read()) {
    case 's': selector.variable = SelectorVariable::Self; break;
    case 'p': selector.variable = SelectorVariable::NearestPlayer; break;
    case 'a': selector.variable = SelectorVariable::AllPlayers; break;
    case 'e': selector.variable = SelectorVariable::AllEntities; break;
    case 'r': selector.variable = SelectorVariable::Random; break;
    default:
        return parseFail("Unknown selector variable (use @s, @p, @a, @e, @r)", reader);
    }
    if (!reader.canRead() || reader.peek() != '[') {
        return parseOk(selector); // a bare selector, no filters
    }
    reader.skip(); // '['
    // An empty filter block (`@e[]`) is allowed.
    if (reader.canRead() && reader.peek() == ']') {
        reader.skip();
        return parseOk(selector);
    }
    while (true) {
        std::string key;
        while (reader.canRead() && ((reader.peek() >= 'a' && reader.peek() <= 'z') ||
                                    (reader.peek() >= 'A' && reader.peek() <= 'Z'))) {
            key.push_back(reader.read());
        }
        if (key.empty()) {
            return parseFail("Expected a selector option name", reader);
        }
        if (!reader.canRead() || reader.read() != '=') {
            return parseFail("Selector option " + key + " needs a value", reader);
        }
        std::string value;
        while (reader.canRead() && reader.peek() != ',' && reader.peek() != ']') {
            value.push_back(reader.read());
        }
        if (key == "type") {
            std::string_view id = value;
            bool negated = false;
            if (!id.empty() && id.front() == '!') {
                negated = true;
                id.remove_prefix(1);
            }
            if (id.empty() || entities::entityTypeRegistry().byId(id) == nullptr) {
                return parseFail("Unknown entity type: " + std::string{id}, reader);
            }
            selector.typeId = std::string{id};
            selector.typeNegated = negated;
        } else if (key == "distance") {
            const auto separator = value.find("..");
            if (separator != std::string::npos) {
                const std::string_view low{value.data(), separator};
                const std::string_view high{value.data() + separator + 2U,
                                            value.size() - separator - 2U};
                double bound = 0.0;
                if (!low.empty()) {
                    if (!parseSelectorDouble(low, bound) || bound < 0.0) {
                        return parseFail("Invalid distance: " + value, reader);
                    }
                    selector.distanceMinimum = bound;
                }
                if (!high.empty()) {
                    if (!parseSelectorDouble(high, bound) || bound < 0.0) {
                        return parseFail("Invalid distance: " + value, reader);
                    }
                    selector.distanceMaximum = bound;
                }
                if (low.empty() && high.empty()) {
                    return parseFail("Invalid distance: " + value, reader);
                }
            } else {
                double exact = 0.0;
                if (!parseSelectorDouble(value, exact) || exact < 0.0) {
                    return parseFail("Invalid distance: " + value, reader);
                }
                selector.distanceMinimum = exact;
                selector.distanceMaximum = exact;
            }
        } else if (key == "limit") {
            std::int64_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || parsed <= 0) {
                return parseFail("Selector limit must be a positive whole number", reader);
            }
            selector.limit = static_cast<std::size_t>(parsed);
        } else if (key == "sort") {
            if (value == "nearest") {
                selector.sort = SelectorSort::Nearest;
            } else if (value == "furthest") {
                selector.sort = SelectorSort::Furthest;
            } else if (value == "random") {
                selector.sort = SelectorSort::Random;
            } else if (value == "arbitrary") {
                selector.sort = SelectorSort::Arbitrary;
            } else {
                return parseFail("Unknown sort: " + value, reader);
            }
        } else {
            // name/tag/x/y/z/dx/dy/dz/level/scores/nbt: no backing system yet, so
            // reject rather than resolve to a wrong (silently unfiltered) set.
            return parseFail("Unsupported selector option: " + key, reader);
        }
        if (!reader.canRead()) {
            return parseFail("Unterminated selector filter (missing ])", reader);
        }
        const char delimiter = reader.read();
        if (delimiter == ']') {
            break;
        }
        if (delimiter != ',') {
            return parseFail("Expected , or ] in selector filter", reader);
        }
    }
    return parseOk(selector);
}

} // namespace mc::gameplay::command
