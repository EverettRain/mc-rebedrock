#include "ui/ChatHistory.hpp"

#include <cassert>
#include <string>

// ChatHistory is line-oriented like vanilla ChatHud: the HUD renderer draws one
// stored entry per row and cannot break an embedded '\n', so push() must split
// multi-line producer text (notably /help) into one stored line per row.
int main() {
    using mc::ui::ChatHistory;

    // A single-line message stays a single entry, verbatim.
    {
        ChatHistory history;
        history.push("hello world", true, 1.0);
        const auto messages = history.messages();
        assert(messages.size() == 1U);
        assert(messages[0].text == "hello world");
        assert(messages[0].successful);
        assert(messages[0].createdAt == 1.0);
    }

    // A newline-joined message (e.g. /help) becomes one entry per line, in
    // order, each sharing the source message's success flag and creation tick
    // so they fade together.
    {
        ChatHistory history;
        history.push("/gamemode <mode>\n/tp <x> <y> <z>\n/help [<command>]", false, 42.0);
        const auto messages = history.messages();
        assert(messages.size() == 3U);
        assert(messages[0].text == "/gamemode <mode>");
        assert(messages[1].text == "/tp <x> <y> <z>");
        assert(messages[2].text == "/help [<command>]");
        for (const auto& message : messages) {
            assert(!message.successful);
            assert(message.createdAt == 42.0);
        }
    }

    // Empty lines (leading/trailing/consecutive newlines) never produce blank
    // rows — the renderer would otherwise draw an empty background quad.
    {
        ChatHistory history;
        history.push("\nfirst\n\nsecond\n", true, 0.0);
        const auto messages = history.messages();
        assert(messages.size() == 2U);
        assert(messages[0].text == "first");
        assert(messages[1].text == "second");
    }

    // A wholly empty (or newline-only) push adds nothing, as before.
    {
        ChatHistory history;
        history.push("", true, 0.0);
        history.push("\n\n", true, 0.0);
        assert(history.messages().empty());
    }

    // Splitting still respects the ring-buffer capacity: the oldest lines are
    // dropped so the newest kCapacity lines survive, even when one push exceeds
    // the whole capacity.
    {
        ChatHistory history;
        std::string oversized;
        for (std::size_t line = 0; line < ChatHistory::kCapacity + 5U; ++line) {
            oversized += "line" + std::to_string(line) + "\n";
        }
        history.push(oversized, true, 0.0);
        const auto messages = history.messages();
        assert(messages.size() == ChatHistory::kCapacity);
        // The first surviving line is line5 (0..4 evicted); the last is the newest.
        assert(messages.front().text == "line5");
        assert(messages.back().text ==
               "line" + std::to_string(ChatHistory::kCapacity + 4U));
    }

    return 0;
}
