#include "ui/ChatHistory.hpp"

#include <utility>

namespace mc::ui {

void ChatHistory::push(std::string text, bool successful, double createdAt) {
    if (text.empty()) {
        return;
    }
    if (messages_.size() == kCapacity) {
        messages_.erase(messages_.begin());
    }
    messages_.push_back({std::move(text), successful, createdAt});
}

} // namespace mc::ui
