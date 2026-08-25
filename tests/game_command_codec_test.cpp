// N4 message-boundary slice 1: a GameCommand is a byte stream — the client→server
// intent boundary — that survives a round trip and lets a transport split a
// stream back into commands, the way 26.1's ServerboundPlayerActionPacket does.
// Unknown frames (a newer build's command) are skipped by size, not fatal, the
// same forward-compatibility the save format already proves.

#include "gameplay/GameCommandCodec.hpp"

#include "gameplay/Item.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <vector>

using namespace mc;

namespace {

bool sameCommand(const gameplay::GameCommand& a, const gameplay::GameCommand& b) {
    if (a.index() != b.index()) {
        return false;
    }
    return std::visit(
        [&](const auto& alternative) {
            using T = std::decay_t<decltype(alternative)>;
            return alternative == std::get<T>(b);
        },
        a);
}

void checkRoundTrip(const gameplay::GameCommand& command) {
    const auto bytes = gameplay::encodeGameCommand(command);
    const auto decoded = gameplay::decodeGameCommand(bytes);
    assert(decoded.has_value());
    assert(sameCommand(command, *decoded));
}

}  // namespace

int main() {
    // --- Every command variant survives a byte round trip with its fields. ---
    checkRoundTrip(gameplay::GameCommand{gameplay::PlayerAction{
        gameplay::PlayerAction::Kind::StartDestroy, glm::ivec3{3, 64, -5}, false, 0U}});
    checkRoundTrip(gameplay::GameCommand{gameplay::PlayerAction{
        gameplay::PlayerAction::Kind::StartDestroy, std::nullopt, true, 41U}});
    checkRoundTrip(gameplay::GameCommand{gameplay::UseItemOn{
        glm::ivec3{1, 2, 3}, glm::ivec3{1, 3, 3}, world::BlockOrientation::Up,
        glm::vec3{1.25F, 2.5F, 3.75F}, glm::vec3{0.0F, 0.0F, -1.0F}}});
    // An entity-targeted UseItemOn (shear/dye/milk/feed): the entity flag and id
    // must survive, or the server demotes it to a block/empty use and never runs
    // performUseOnEntity.
    checkRoundTrip(gameplay::GameCommand{gameplay::UseItemOn{
        glm::ivec3{0, 0, 0}, glm::ivec3{0, 0, 0}, world::BlockOrientation::Up,
        glm::vec3{0.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 0.0F, -1.0F}, /*entity=*/true,
        /*entityId=*/56U}});
    checkRoundTrip(gameplay::GameCommand{gameplay::UseItem{gameplay::InteractionHand::Off}});
    checkRoundTrip(gameplay::GameCommand{gameplay::UseItemStop{}});
    checkRoundTrip(gameplay::GameCommand{gameplay::SwapSlot{7U}});
    checkRoundTrip(gameplay::GameCommand{gameplay::ClickSlot{
        gameplay::SlotKind::ChestStorage, 12U, 1, true}});
    checkRoundTrip(gameplay::GameCommand{gameplay::ChatCommand{"/give minecraft:diamond 3"}});
    // A block stack and a plain item stack both round-trip by their identifier.
    checkRoundTrip(gameplay::GameCommand{gameplay::ClickCreativeItem{
        gameplay::ItemStack{world::Block::Stone, 1U, gameplay::blockItemFor(world::Block::Stone)},
        gameplay::InventoryMouseButton::Left, false}});
    checkRoundTrip(gameplay::GameCommand{gameplay::ClickCreativeItem{
        gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Diamond},
        gameplay::InventoryMouseButton::Right, true}});
    checkRoundTrip(gameplay::GameCommand{gameplay::ClearCursor{}});
    checkRoundTrip(gameplay::GameCommand{gameplay::DropCursor{{1.0F, 2.0F, 3.0F}}});
    checkRoundTrip(gameplay::GameCommand{gameplay::DropSelected{false, {0.5F, -1.0F, 0.25F}}});
    checkRoundTrip(gameplay::GameCommand{gameplay::DragDistribute{
        gameplay::InventoryMouseButton::Right,
        {gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 0U},
         gameplay::SlotRef{gameplay::SlotKind::ChestStorage, 2U}}}});
    checkRoundTrip(gameplay::GameCommand{gameplay::PickupAll{
        {gameplay::SlotRef{gameplay::SlotKind::PlayerInventory, 3U}}}});

    // --- MovementInput (D0): the continuous intent round-trips with its floats
    // and all six intent bits, and its frame carries the movement tag. ---
    {
        gameplay::MovementInput input;
        input.forward = 1.0F;
        input.strafe = -1.0F;
        input.lookDirection = glm::vec3{0.25F, -0.5F, 0.8F};
        input.jumpHeld = true;
        input.descendHeld = false;
        input.sneakHeld = true;
        input.sprintHeld = false;
        input.jumpPressed = true;
        input.forwardPressed = true;
        input.autoJump = true;
        const auto bytes = gameplay::encodeMovementInput(input);
        assert(!bytes.empty());
        assert(bytes.front() == gameplay::kMovementInputTag);
        const auto decoded = gameplay::decodeMovementInput(bytes);
        assert(decoded.has_value());
        assert(*decoded == input);

        // The default-valued input round-trips too (all bits clear).
        const gameplay::MovementInput zero;
        const auto zeroDecoded = gameplay::decodeMovementInput(gameplay::encodeMovementInput(zero));
        assert(zeroDecoded.has_value() && *zeroDecoded == zero);

        // A command frame is not a movement frame and vice versa: the tag guards.
        assert(!gameplay::decodeMovementInput(
                    gameplay::encodeGameCommand(gameplay::GameCommand{gameplay::SwapSlot{1U}}))
                    .has_value());
        assert(!gameplay::decodeGameCommand(bytes).has_value());
    }

    // --- SessionCommand (D0): respawn (no payload) and a game-mode switch both
    // round-trip, and their frames carry tags in the session range, distinct from
    // a movement or game command. ---
    {
        const gameplay::SessionCommand respawn{gameplay::Respawn{}};
        const auto respawnBytes = gameplay::encodeSessionCommand(respawn);
        assert(respawnBytes.front() == gameplay::kSessionCommandTagBase);  // index 0
        const auto respawnDecoded = gameplay::decodeSessionCommand(respawnBytes);
        assert(respawnDecoded.has_value() && *respawnDecoded == respawn);

        const gameplay::SessionCommand setMode{gameplay::SetGameMode{gameplay::GameMode::Creative}};
        const auto modeBytes = gameplay::encodeSessionCommand(setMode);
        assert(modeBytes.front() == gameplay::kSessionCommandTagBase + 1U);  // index 1
        const auto modeDecoded = gameplay::decodeSessionCommand(modeBytes);
        assert(modeDecoded.has_value() && *modeDecoded == setMode);
        assert(std::get<gameplay::SetGameMode>(*modeDecoded).mode == gameplay::GameMode::Creative);

        // A game command is not a session command and vice versa: tags guard.
        assert(!gameplay::decodeSessionCommand(
                    gameplay::encodeGameCommand(gameplay::GameCommand{gameplay::SwapSlot{1U}}))
                    .has_value());
        assert(!gameplay::decodeGameCommand(modeBytes).has_value());
    }

    // --- Two commands in one byte stream split back at the frame boundary. ---
    {
        const gameplay::GameCommand first{gameplay::DropCursor{{1.0F, 2.0F, 3.0F}}};
        const gameplay::GameCommand second{gameplay::ChatCommand{"hi there"}};
        auto stream = gameplay::encodeGameCommand(first);
        const auto secondBytes = gameplay::encodeGameCommand(second);
        const auto firstSize = stream.size();
        stream.insert(stream.end(), secondBytes.begin(), secondBytes.end());

        const auto boundary = gameplay::encodedGameCommandSize(stream);
        assert(boundary == firstSize);
        const auto decodedFirst =
            gameplay::decodeGameCommand(std::span{stream.data(), boundary});
        assert(decodedFirst.has_value() && sameCommand(first, *decodedFirst));
        const auto decodedSecond = gameplay::decodeGameCommand(
            std::span{stream.data() + boundary, stream.size() - boundary});
        assert(decodedSecond.has_value() && sameCommand(second, *decodedSecond));
    }

    // --- An unknown tag (a newer build's command) is skipped by size, not
    // fatal; the transport advances via encodedGameCommandSize. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(99U);  // unknown tag
        persistence::appendInteger(bytes, std::uint32_t{3});
        bytes.push_back(1);
        bytes.push_back(2);
        bytes.push_back(3);
        assert(gameplay::encodedGameCommandSize(bytes) == 5U + 3U);
        assert(!gameplay::decodeGameCommand(bytes).has_value());
    }

    // --- A truncated frame reports an incomplete size and refuses to decode. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(0U);  // a known tag...
        persistence::appendInteger(bytes, std::uint32_t{1000U});  // ...but a huge size
        bytes.push_back(1);                                       // and almost no payload
        assert(gameplay::encodedGameCommandSize(bytes) == 0U);
        assert(!gameplay::decodeGameCommand(bytes).has_value());
    }

    std::cout << "PASS: game_command_codec_test\n";
    return 0;
}
