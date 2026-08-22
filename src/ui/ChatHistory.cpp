#include "ui/ChatHistory.hpp"

#include <utility>

namespace mc::ui {

void ChatHistory::push(std::string text, bool successful, double createdAt) {
    // Vanilla ChatHud stores one visual line per ChatHudLine: a message that
    // spans several lines (e.g. /help's newline-joined listing) is broken into
    // lines at insert time, each stored line sharing the source message's
    // creation tick so they fade together. The HUD renderer draws exactly one
    // stored entry per row and has no notion of an embedded '\n' — it would
    // render the newline as a stray glyph on a single run-on line. So the split
    // has to happen here, at the single seam where multi-line producer text
    // enters the line-oriented store; every producer (command feedback, chat)
    // reaches the HUD through this one method.
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(start, end - start);
        if (!line.empty()) {
            if (messages_.size() == kCapacity) {
                messages_.erase(messages_.begin());
            }
            messages_.push_back({std::move(line), successful, createdAt});
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
}

} // namespace mc::ui
