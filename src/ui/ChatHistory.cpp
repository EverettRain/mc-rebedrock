#include "ui/ChatHistory.hpp"

#include <utility>

namespace mc::ui {

void ChatHistory::push(std::string text, bool successful, double createdAt) {
    // vanilla 的 ChatHud 里一个 ChatHudLine 存一个视觉行
    // 跨多行的消息在插入时就被拆开，比如 /help 那种用换行拼起来的清单
    // 拆出的每一行共用源消息的创建 tick，它们因此一起淡出
    // HUD 渲染器一行只画一条存储项，它根本不认识内嵌的 '\n'
    // 那个换行会被当成一个孤零零的字形，画在一整条连成串的行上
    // 所以拆分必须发生在这里，也就是多行文本进入这个按行组织的存储的唯一接缝
    // 每一个产出方都经由这一个方法抵达 HUD，无论是命令回执还是聊天
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
