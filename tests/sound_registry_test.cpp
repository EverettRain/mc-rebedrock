#include "assets/ResourceProvider.hpp"
#include "assets/SoundRegistry.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

// sounds.json maps a sound event to a weighted list of candidate files, and a
// pack merges its file over the base by replacing or appending per event. These
// pin the two entry forms, the weighted pick, the type:event chain, and the
// replace-vs-append merge rule the report (§4) describes.
int main() {
    using namespace mc::assets;

    // --- Both entry forms parse; the object form carries the parameters. ---
    {
        constexpr std::string_view json = R"({
            "block.stone.break": {
                "sounds": [
                    "dig/stone1",
                    {"name": "dig/stone2", "volume": 0.8, "pitch": 1.2, "weight": 3, "stream": false}
                ],
                "subtitle": "subtitles.block.generic.break"
            }
        })";
        const auto events = SoundRegistry::parse(json);
        assert(events.size() == 1U);
        assert(events[0].id == "block.stone.break");
        assert(events[0].subtitle == "subtitles.block.generic.break");
        assert(events[0].sounds.size() == 2U);
        // Bare string: name only, everything else defaulted.
        assert(events[0].sounds[0].name == "dig/stone1" && events[0].sounds[0].volume == 1.0F &&
               events[0].sounds[0].weight == 1);
        // Object form: parameters read through.
        assert(events[0].sounds[1].name == "dig/stone2" && events[0].sounds[1].volume == 0.8F &&
               events[0].sounds[1].pitch == 1.2F && events[0].sounds[1].weight == 3);
    }

    // --- replace overwrites the base's list; append (default) adds to it. ---
    {
        SoundRegistry registry;
        registry.merge(SoundRegistry::parseWithReplace(
            R"({"block.stone.break": {"sounds": ["dig/stone1", "dig/stone2"]},
                "music.game": {"sounds": ["music/game/calm1"]}})"));
        assert(registry.find("block.stone.break")->sounds.size() == 2U);

        // A pack that replaces block.stone.break and appends to music.game.
        registry.merge(SoundRegistry::parseWithReplace(
            R"({"block.stone.break": {"sounds": ["custom/rock"], "replace": true},
                "music.game": {"sounds": ["music/game/creative1"]}})"));

        // Replace: only the pack's candidate remains.
        const auto* stone = registry.find("block.stone.break");
        assert(stone->sounds.size() == 1U && stone->sounds[0].name == "custom/rock");
        // Append: both the base and the pack candidates are present.
        const auto* music = registry.find("music.game");
        assert(music->sounds.size() == 2U);
        assert(music->sounds[0].name == "music/game/calm1" &&
               music->sounds[1].name == "music/game/creative1");

        // An event only the pack declares lands whole.
        registry.merge(
            SoundRegistry::parseWithReplace(R"({"custom.event": {"sounds": ["custom/new"]}})"));
        assert(registry.find("custom.event") != nullptr);
        assert(registry.find("does.not.exist") == nullptr);
    }

    // --- pick honours weights and follows a type:event reference. ---
    {
        SoundRegistry registry;
        registry.merge(SoundRegistry::parseWithReplace(R"({
            "entity.generic.hurt": {"sounds": ["hurt1", {"name": "hurt2", "weight": 100}]},
            "entity.player.hurt": {"sounds": [{"name": "entity.generic.hurt", "type": "event"}]}
        })"));
        const SoundEventId genericHurt = registry.idOf("entity.generic.hurt");
        assert(genericHurt != kInvalidSoundEventId);
        assert(registry.idOf("no.such.event") == kInvalidSoundEventId);
        assert(registry.find("entity.generic.hurt")->totalWeight == 101);

        // With weights 1:100, a spread of seeds lands on hurt2 the vast majority
        // of the time; assert it is at least reachable and both are valid picks.
        std::uint32_t state = 0x1234U;
        bool sawHeavy = false;
        for (int i = 0; i < 50; ++i) {
            const auto* picked = registry.pick("entity.generic.hurt", state);
            assert(picked != nullptr);
            assert(picked->name == "hurt1" || picked->name == "hurt2");
            sawHeavy = sawHeavy || picked->name == "hurt2";
        }
        assert(sawHeavy); // the weight-100 candidate must show up

        // The compiled integer id takes the same selection path without a
        // runtime string lookup or weight summation.
        const auto* viaId = registry.pick(genericHurt, state);
        assert(viaId != nullptr && (viaId->name == "hurt1" || viaId->name == "hurt2"));

        // The player hurt event references the generic one, so pick resolves
        // through to a real file rather than returning the event entry.
        std::uint32_t state2 = 0x99U;
        const auto* viaRef = registry.pick("entity.player.hurt", state2);
        assert(viaRef != nullptr && !viaRef->isEvent);
        assert(viaRef->name == "hurt1" || viaRef->name == "hurt2");

        assert(registry.pick("no.such.event", state2) == nullptr);

        // Multi-event cycles are rejected just like direct self references.
        registry.merge(SoundRegistry::parseWithReplace(R"({
            "cycle.a": {"sounds": [{"name": "cycle.b", "type": "event"}]},
            "cycle.b": {"sounds": [{"name": "cycle.a", "type": "event"}]}
        })"));
        assert(registry.pick("cycle.a", state2) == nullptr);
    }

    // --- Loading consumes every sounds.json from low to high priority. ---
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "rebedrock_sound_registry_load_test";
        std::error_code cleanup;
        fs::remove_all(tmp, cleanup);
        const fs::path lowFile = tmp / "low" / "assets" / "minecraft" / "sounds.json";
        const fs::path highFile = tmp / "high" / "assets" / "minecraft" / "sounds.json";
        fs::create_directories(lowFile.parent_path());
        fs::create_directories(highFile.parent_path());
        {
            std::ofstream file{lowFile};
            file << R"({"event.append":{"sounds":["low"]},
                        "event.replace":{"sounds":["old"]}})";
        }
        {
            std::ofstream file{highFile};
            file << R"({"event.append":{"sounds":["high"]},
                        "event.replace":{"sounds":["new"],"replace":true}})";
        }
        const DirectoryResourceProvider emptyBase{tmp / "base"};
        const StandardPackResourceProvider low{tmp / "low"};
        const StandardPackResourceProvider high{tmp / "high"};
        const LayeredResourceProvider provider{emptyBase, {&high, &low}};
        const SoundRegistry loaded = SoundRegistry::load(provider);
        assert(loaded.find("event.append")->sounds.size() == 2U);
        assert(loaded.find("event.append")->sounds[0].name == "low");
        assert(loaded.find("event.append")->sounds[1].name == "high");
        assert(loaded.find("event.replace")->sounds.size() == 1U);
        assert(loaded.find("event.replace")->sounds[0].name == "new");
        fs::remove_all(tmp, cleanup);
    }

    return 0;
}
