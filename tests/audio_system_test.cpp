#include "audio/AudioSystem.hpp"

#include <cassert>
#include <string_view>

int main() {
    using mc::audio::BlockSoundFamily;
    using mc::audio::blockSoundFamily;
    using mc::world::Block;

    assert(blockSoundFamily(Block::Stone) == BlockSoundFamily::Stone);
    assert(blockSoundFamily(Block::OakLog) == BlockSoundFamily::Wood);
    assert(blockSoundFamily(Block::OakLeaves) == BlockSoundFamily::Grass);
    assert(blockSoundFamily(Block::RedSand) == BlockSoundFamily::Sand);
    assert(blockSoundFamily(Block::Gravel) == BlockSoundFamily::Gravel);
    assert(blockSoundFamily(Block::RedWool) == BlockSoundFamily::Cloth);
    assert(blockSoundFamily(Block::Glass) == BlockSoundFamily::Glass);
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Glass)} == "glass");
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Wood)} == "wood");
    // The gameplay family remains Cloth, but 26.1 names the event block.wool.*.
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Cloth)} == "wool");
    return 0;
}
