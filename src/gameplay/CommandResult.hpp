#pragma once

#include <string>

namespace mc::gameplay {

// The result of executing one command line, consumed by the chat history on the
// render thread. Failed commands render their message in red, mirroring 1.16.1's
// chat feedback. Shared by the command tree and the game systems that implement
// commands (e.g. GameRules), so it lives outside the command module.
struct CommandResult final {
    bool success = false;
    std::string message;
};

} // namespace mc::gameplay
