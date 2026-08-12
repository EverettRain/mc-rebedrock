#include "assets/SoundRegistry.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
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
    for (const auto& path : provider.locateAll(location)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open sounds.json: " + path.string());
        }
        const std::string contents{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        try {
            registry.merge(parseWithReplace(contents));
        } catch (const std::exception& exception) {
            throw std::runtime_error("Unable to parse sounds.json " + path.string() + ": " +
                                     exception.what());
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
    const auto found = std::ranges::find(events_, id, &SoundEvent::id);
    return found == events_.end() ? nullptr : &*found;
}

const SoundEvent* SoundRegistry::find(std::string_view id) const {
    const auto found = std::ranges::find(events_, id, &SoundEvent::id);
    return found == events_.end() ? nullptr : &*found;
}

void SoundRegistry::merge(const std::vector<ParsedEvent>& packEvents) {
    for (const auto& parsed : packEvents) {
        SoundEvent* existing = findMutable(parsed.event.id);
        if (existing == nullptr) {
            // A new event: replace or append is moot, it lands whole.
            events_.push_back(parsed.event);
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
}

const SoundEntry* SoundRegistry::pick(std::string_view id, std::uint32_t& randomState) const {
    std::vector<std::string_view> chain;
    return pick(id, randomState, chain);
}

const SoundEntry* SoundRegistry::pick(std::string_view id, std::uint32_t& randomState,
                                      std::vector<std::string_view>& chain) const {
    if (std::ranges::find(chain, id) != chain.end()) {
        return nullptr;
    }
    const SoundEvent* event = find(id);
    if (event == nullptr || event->sounds.empty()) {
        return nullptr;
    }
    int totalWeight = 0;
    for (const auto& entry : event->sounds) {
        totalWeight += entry.weight;
    }
    if (totalWeight <= 0) {
        return nullptr;
    }
    // xorshift, the same cheap generator the loot rolls use, so a sound event
    // varies its candidate from play to play without a std::random dependency.
    randomState ^= randomState << 13U;
    randomState ^= randomState >> 17U;
    randomState ^= randomState << 5U;
    int roll = static_cast<int>(randomState % static_cast<std::uint32_t>(totalWeight));
    const SoundEntry* chosen = nullptr;
    for (const auto& entry : event->sounds) {
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
        // `type: event` may form an arbitrary chain. Track every visited event
        // so malformed A -> B -> A references fail closed instead of recursing
        // forever.
        chain.push_back(id);
        const SoundEntry* resolved = pick(chosen->name, randomState, chain);
        chain.pop_back();
        return resolved;
    }
    return chosen;
}

} // namespace mc::assets
