#include "render/TestScene.hpp"

#include <array>
#include <cassert>
#include <stdexcept>

int main() {
    using namespace std::string_view_literals;
    const std::array arguments{"--test-scene"sv, "minecraft:furnace"sv,
                               "--stage"sv, "3"sv};
    const auto scene = mc::render::parseTestSceneArguments(arguments);
    assert(scene.has_value());
    assert(scene->block == mc::world::Block::Furnace);
    assert(scene->stage == 3);
    const std::array none{"--unrelated"sv};
    assert(!mc::render::parseTestSceneArguments(none).has_value());
    bool rejected = false;
    try {
        const std::array invalid{"--test-scene"sv, "missing"sv};
        static_cast<void>(mc::render::parseTestSceneArguments(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
