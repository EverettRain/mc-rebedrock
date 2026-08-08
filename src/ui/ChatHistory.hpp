#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace mc::ui {

struct ChatMessage final {
    std::string text;
    bool successful = true;
    double createdAt = 0.0;
};

class ChatHistory final {
  public:
    static constexpr std::size_t kCapacity = 30U;

    void push(std::string text, bool successful, double createdAt);
    [[nodiscard]] std::span<const ChatMessage> messages() const { return messages_; }
    void clear() { messages_.clear(); }

  private:
    std::vector<ChatMessage> messages_;
};

} // namespace mc::ui
