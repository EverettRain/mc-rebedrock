#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::assets {

using SoundEventId = std::uint32_t;
inline constexpr SoundEventId kInvalidSoundEventId = std::numeric_limits<SoundEventId>::max();

struct TransparentStringHash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view{value});
    }
};

struct TransparentStringEqual final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept {
        return left == right;
    }
};

class ResourceProvider;

// One entry in a sound event's candidate list. In sounds.json this is either a
// bare string (just `name`, defaults everywhere else) or an object carrying the
// playback parameters. `type == "event"` means `name` refers to another event
// rather than a file, which the resolver follows.
struct SoundEntry final {
    std::string name; // file path under `sounds/…` (no extension), or an event id
    float volume = 1.0F;
    float pitch = 1.0F;
    int weight = 1;       // relative odds of being picked
    bool stream = false;  // decode on the fly (music) rather than fully in memory
    bool isEvent = false; // name references another event, not a file

    // Compiled once after pack merging. Runtime selection follows an integer
    // edge instead of looking the referenced event up by string on every play.
    SoundEventId referencedEvent = kInvalidSoundEventId;

    [[nodiscard]] bool operator==(const SoundEntry&) const = default;
};

// A sound event ("block.stone.break") and the candidates one of its plays picks
// from. `subtitle` is the accessibility caption, if the pack gave one.
struct SoundEvent final {
    std::string id;
    std::vector<SoundEntry> sounds;
    std::string subtitle;

    // Sum of candidate weights, compiled once after merging rather than for
    // every footstep or ambient sound.
    int totalWeight = 0;

    [[nodiscard]] bool operator==(const SoundEvent&) const = default;
};

// The merged sound event table, built by layering each enabled pack's
// sounds.json over the base the way vanilla's SoundManager does. The merge rule
// is per event: an event marked `"replace": true` overwrites the base's
// candidate list, otherwise its candidates are appended — so a pack can either
// take over a sound completely or add variations to it. (Report §4: the vanilla
// pack ships 1871 non-music events with replace:true and 8 music events without,
// which append.)
class SoundRegistry final {
  public:
    // Loads every assets/<namespace>/sounds.json exposed by the provider from
    // lowest to highest pack priority and applies vanilla's per-event merge
    // rules. Missing files simply produce an empty registry.
    [[nodiscard]] static SoundRegistry load(const ResourceProvider& provider,
                                            std::string_view space = "minecraft");
    // Parses one sounds.json's text into its events, preserving author order.
    // Throws std::runtime_error on malformed JSON.
    [[nodiscard]] static std::vector<SoundEvent> parse(std::string_view soundsJson);

    // Whether a parsed event replaced the base or appended to it. Returned
    // alongside parse via parseWithReplace so merge() can apply the rule; kept
    // separate from SoundEvent because it is a merge directive, not sound data.
    struct ParsedEvent final {
        SoundEvent event;
        bool replace = false;
    };
    [[nodiscard]] static std::vector<ParsedEvent> parseWithReplace(std::string_view soundsJson);

    // Layers one pack's events onto this registry: replace overwrites, otherwise
    // append. Call once per pack, base first.
    void merge(const std::vector<ParsedEvent>& packEvents);

    [[nodiscard]] const SoundEvent* find(std::string_view id) const;
    [[nodiscard]] SoundEventId idOf(std::string_view id) const;
    [[nodiscard]] std::size_t size() const { return events_.size(); }
    [[nodiscard]] const std::vector<SoundEvent>& events() const { return events_; }

    // Picks a candidate file for an event by weight, following a `type:event`
    // reference to the event it names. `randomState` is advanced so repeated
    // plays vary. Returns nullptr if the event is unknown or resolves to nothing.
    [[nodiscard]] const SoundEntry* pick(std::string_view id, std::uint32_t& randomState) const;
    [[nodiscard]] const SoundEntry* pick(SoundEventId id, std::uint32_t& randomState) const;

  private:
    [[nodiscard]] const SoundEntry* pick(SoundEventId id, std::uint32_t& randomState,
                                         SoundEventId* chain, std::size_t depth) const;
    [[nodiscard]] SoundEvent* findMutable(std::string_view id);
    void compile();

    std::vector<SoundEvent> events_;
    std::unordered_map<std::string, SoundEventId, TransparentStringHash, TransparentStringEqual>
        eventIds_;
};

} // namespace mc::assets
