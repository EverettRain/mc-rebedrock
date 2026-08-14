#include "assets/SoundRegistry.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"

#include <algorithm>
#include <fstream>
#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace mc::assets {
namespace {

[[nodiscard]] SoundEntry parseEntry(const core::Json& entry) {
    SoundEntry result;
    if (entry.isString()) {
        result.name = entry.asString();
        return result;
    }
    // Object form: name is required, the rest default.
    result.name = entry["name"].asString();
    result.volume = entry["volume"].asFloat(1.0F);
    result.pitch = entry["pitch"].asFloat(1.0F);
    result.weight = static_cast<int>(entry["weight"].asNumber(1.0));
    result.stream = entry["stream"].asBool(false);
    // `type` is "sound" (a file, default) or "event" (a reference to another
    // event); only the reference case changes resolution.
    result.isEvent = entry["type"].isString() && entry["type"].asString() == "event";
    if (result.weight < 1) {
        result.weight = 1;
    }
    return result;
}

} // namespace

SoundRegistry SoundRegistry::load(const ResourceProvider& provider, std::string_view space) {
    SoundRegistry registry;
    const ResourceLocation location{std::string{space}, "sounds.json"};
    // Bytes, not paths: sounds.json is a merged resource, and going through
    // locateAll would make a zipped pack extract every pack's copy to disk.
    for (const auto& bytes : provider.readAllBytes(location)) {
        const std::string contents{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        try {
            registry.merge(parseWithReplace(contents));
        } catch (const std::exception& exception) {
            throw std::runtime_error(std::string{"Unable to parse sounds.json for "} +
                                     location.toString() + ": " + exception.what());
        }
    }
    return registry;
}

std::vector<SoundRegistry::ParsedEvent>
SoundRegistry::parseWithReplace(std::string_view soundsJson) {
    const core::Json root = core::Json::parse(soundsJson);
    std::vector<ParsedEvent> parsed;
    if (!root.isObject()) {
        return parsed;
    }
    for (const auto& [id, definition] : root.asObject()) {
        ParsedEvent entry;
        entry.event.id = id;
        entry.replace = definition["replace"].asBool(false);
        if (definition["subtitle"].isString()) {
            entry.event.subtitle = definition["subtitle"].asString();
        }
        const core::Json& sounds = definition["sounds"];
        if (sounds.isArray()) {
            for (std::size_t index = 0; index < sounds.size(); ++index) {
                entry.event.sounds.push_back(parseEntry(sounds[index]));
            }
        }
        parsed.push_back(std::move(entry));
    }
    return parsed;
}

std::vector<SoundEvent> SoundRegistry::parse(std::string_view soundsJson) {
    std::vector<SoundEvent> events;
    for (auto& parsed : parseWithReplace(soundsJson)) {
        events.push_back(std::move(parsed.event));
    }
    return events;
}

SoundEvent* SoundRegistry::findMutable(std::string_view id) {
    const auto found = eventIds_.find(id);
    return found == eventIds_.end() ? nullptr : &events_[found->second];
}

const SoundEvent* SoundRegistry::find(std::string_view id) const {
    const SoundEventId eventId = idOf(id);
    return eventId == kInvalidSoundEventId ? nullptr : &events_[eventId];
}

SoundEventId SoundRegistry::idOf(std::string_view id) const {
    const auto found = eventIds_.find(id);
    return found == eventIds_.end() ? kInvalidSoundEventId : found->second;
}

void SoundRegistry::merge(const std::vector<ParsedEvent>& packEvents) {
    for (const auto& parsed : packEvents) {
        SoundEvent* existing = findMutable(parsed.event.id);
        if (existing == nullptr) {
            // A new event: replace or append is moot, it lands whole.
            events_.push_back(parsed.event);
            eventIds_.emplace(events_.back().id,
                              static_cast<SoundEventId>(events_.size() - 1U));
            continue;
        }
        if (parsed.replace) {
            // Take over the base's candidates entirely.
            existing->sounds = parsed.event.sounds;
        } else {
            // Add this pack's candidates on top of the base's.
            existing->sounds.insert(existing->sounds.end(), parsed.event.sounds.begin(),
                                    parsed.event.sounds.end());
        }
        if (!parsed.event.subtitle.empty()) {
            existing->subtitle = parsed.event.subtitle;
        }
    }
    compile();
}

void SoundRegistry::compile() {
    // Rebuild defensively because SoundRegistry is movable and pack merging can
    // append enough events to relocate the backing vector. The map stores only
    // stable integer indices, never vector pointers.
    eventIds_.clear();
    eventIds_.reserve(events_.size());
    for (std::size_t index = 0; index < events_.size(); ++index) {
        eventIds_.emplace(events_[index].id, static_cast<SoundEventId>(index));
    }
    for (auto& event : events_) {
        event.totalWeight = 0;
        for (auto& entry : event.sounds) {
            event.totalWeight += entry.weight;
            entry.referencedEvent = entry.isEvent ? idOf(entry.name) : kInvalidSoundEventId;
        }
    }
}

const SoundEntry* SoundRegistry::pick(std::string_view id, std::uint32_t& randomState) const {
    return pick(idOf(id), randomState);
}

const SoundEntry* SoundRegistry::pick(SoundEventId id, std::uint32_t& randomState) const {
    std::array<SoundEventId, 32> chain{};
    return pick(id, randomState, chain.data(), 0U);
}

const SoundEntry* SoundRegistry::pick(SoundEventId id, std::uint32_t& randomState,
                                      SoundEventId* chain, std::size_t depth) const {
    if (id == kInvalidSoundEventId || id >= events_.size() || depth >= 32U ||
        std::find(chain, chain + depth, id) != chain + depth) {
        return nullptr;
    }
    const SoundEvent& event = events_[id];
    if (event.sounds.empty() || event.totalWeight <= 0) {
        return nullptr;
    }
    // xorshift, the same cheap generator the loot rolls use, so a sound event
    // varies its candidate from play to play without a std::random dependency.
    randomState ^= randomState << 13U;
    randomState ^= randomState >> 17U;
    randomState ^= randomState << 5U;
    int roll = static_cast<int>(randomState % static_cast<std::uint32_t>(event.totalWeight));
    const SoundEntry* chosen = nullptr;
    for (const auto& entry : event.sounds) {
        roll -= entry.weight;
        if (roll < 0) {
            chosen = &entry;
            break;
        }
    }
    if (chosen == nullptr) {
        return nullptr;
    }
    if (chosen->isEvent) {
        chain[depth] = id;
        return pick(chosen->referencedEvent, randomState, chain, depth + 1U);
    }
    return chosen;
}

} // namespace mc::assets
