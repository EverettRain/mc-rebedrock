#include "assets/ResourcePackLibrary.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace mc::assets {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

// 一个包在栈里可能贡献不止一个 provider（主 assets 之上还有版本门控的 overlay），
// 而 PackManager 是逐 provider 记账的。overlay 的 id 由包 id 派生：它们不是独立的
// 包，玩家在界面上看不到也选不了，派生 id 只是为了不和任何真包的 id 撞上。
[[nodiscard]] std::string overlayId(const std::string& packId, std::size_t index) {
    return packId + "\noverlay#" + std::to_string(index);
}

} // namespace

ResourcePackLibrary::ResourcePackLibrary(std::filesystem::path selectionFile)
    : selectionFile_(std::move(selectionFile)) {}

std::size_t ResourcePackLibrary::indexOf(const std::string& id) const {
    const auto it = std::ranges::find(packs_, id, &ResourcePackEntry::id);
    return it == packs_.end() ? packs_.size() : static_cast<std::size_t>(it - packs_.begin());
}

const ResourcePackEntry* ResourcePackLibrary::find(const std::string& id) const {
    const std::size_t index = indexOf(id);
    return index == packs_.size() ? nullptr : &packs_[index];
}

std::string ResourcePackLibrary::deriveId(const std::filesystem::path& path) const {
    std::string base = path.filename().string();
    if (base.empty()) {
        // 末尾带分隔符的路径（`.../foo/`）filename() 是空的，退回上一级
        base = path.parent_path().filename().string();
    }
    std::string id = base;
    for (int suffix = 2; find(id) != nullptr; ++suffix) {
        id = base + " (" + std::to_string(suffix) + ")";
    }
    return id;
}

std::string ResourcePackLibrary::titleFromId(std::string_view id) {
    if (id.size() > 4U && id.substr(id.size() - 4U) == ".zip") {
        id.remove_suffix(4U);
    }
    return std::string{id};
}

void ResourcePackLibrary::addPack(ResourcePackEntry entry, const ResourceProvider& provider,
                                  std::vector<const ResourceProvider*> overlays) {
    // 同 id 重复登记按「后来者替换」处理，与 PackManager::registerPack 一致；
    // 扫描到重名（一个 foo/ 目录和一个 --pack 指到别处的 foo/）时才会发生。
    const std::size_t index = indexOf(entry.id);
    Registered record{&provider, std::move(overlays), PackMetadata{}};
    record.metadata.minFormat = entry.minFormat;
    record.metadata.maxFormat = entry.maxFormat;
    record.metadata.description = entry.description;
    if (index != packs_.size()) {
        packs_[index] = std::move(entry);
        registered_[index] = std::move(record);
        return;
    }
    packs_.push_back(std::move(entry));
    registered_.push_back(std::move(record));
}

void ResourcePackLibrary::loadSelection() {
    draft_.clear();
    std::ifstream input{selectionFile_, std::ios::binary};
    if (!input) {
        // ★ 没有这份文件 = 本特性之前的行为：发现到的包全部启用，顺序即扫描顺序。
        //   这一支覆盖所有旧游戏目录，以及从没打开过选择界面的新玩家。
        //   注意首次运行**不写**这份文件：写了就等于把「当前这批包」冻结成名单，
        //   之后再往 resourcepacks/ 里丢一个包会默认不启用，与旧行为不符。
        //   只有玩家真的提交过一次选择，文件才存在。
        for (const auto& pack : packs_) {
            draft_.push_back(pack.id);
        }
        active_ = draft_;
        return;
    }
    std::ostringstream text;
    text << input.rdbuf();
    for (const auto& id : parseSelection(text.str())) {
        if (find(id) == nullptr) {
            continue; // 包被删掉了：名单里那一行就此作废，不留幽灵条目
        }
        if (std::ranges::find(draft_, id) == draft_.end()) {
            draft_.push_back(id);
        }
    }
    // `--pack` 点名的包不受名单约束：一条命令说了用哪个包就是哪个包，而且置顶。
    for (const auto& pack : packs_) {
        if (!pack.pinned) {
            continue;
        }
        std::erase(draft_, pack.id);
        draft_.push_back(pack.id);
    }
    active_ = draft_;
}

bool ResourcePackLibrary::isEnabled(const std::string& id) const {
    return std::ranges::find(draft_, id) != draft_.end();
}

bool ResourcePackLibrary::setEnabled(const std::string& id, bool enabled) {
    const ResourcePackEntry* entry = find(id);
    if (entry == nullptr) {
        return false;
    }
    if (enabled) {
        if (isEnabled(id)) {
            return false;
        }
        draft_.push_back(id); // 新启用的接在栈顶，优先级最高
        return true;
    }
    if (entry->pinned) {
        return false; // 命令行点名的包不给关
    }
    const auto it = std::ranges::find(draft_, id);
    if (it == draft_.end()) {
        return false;
    }
    draft_.erase(it);
    return true;
}

bool ResourcePackLibrary::movePriorityUp(const std::string& id) {
    const auto it = std::ranges::find(draft_, id);
    if (it == draft_.end() || it + 1 == draft_.end()) {
        return false;
    }
    std::iter_swap(it, it + 1);
    return true;
}

bool ResourcePackLibrary::movePriorityDown(const std::string& id) {
    const auto it = std::ranges::find(draft_, id);
    if (it == draft_.end() || it == draft_.begin()) {
        return false;
    }
    std::iter_swap(it, it - 1);
    return true;
}

void ResourcePackLibrary::discardDraft() { draft_ = active_; }

PackCommitOutcome ResourcePackLibrary::commit() {
    PackCommitOutcome outcome;
    // 写不回来的 id 宁可拒绝落盘，也不要写出一份读回去意思变了的名单——
    // 那种错误在下一次启动才发作，而且看起来像「游戏忘了我的选择」。
    for (const auto& id : draft_) {
        if (!idIsPersistable(id)) {
            outcome.error = "资源包 id 无法写入启用列表: " + id;
            outcome.restartRequired = draftDiffersFromActive();
            return outcome;
        }
    }
    std::error_code error;
    if (!selectionFile_.parent_path().empty()) {
        std::filesystem::create_directories(selectionFile_.parent_path(), error);
    }
    std::ofstream output{selectionFile_, std::ios::binary | std::ios::trunc};
    if (!output) {
        outcome.error = "无法写入 " + selectionFile_.string();
        outcome.restartRequired = draftDiffersFromActive();
        return outcome;
    }
    output << serializeSelection(draft_);
    output.flush();
    outcome.written = output.good();
    if (!outcome.written) {
        outcome.error = "写入 " + selectionFile_.string() + " 时出错";
    }
    // ★ active_ 有意**不**更新：本次运行生效的仍是启动时装配的那一份。
    //   界面据此显示「重启后生效」，而不是让开关看起来已经起作用了。
    outcome.restartRequired = draftDiffersFromActive();
    return outcome;
}

bool ResourcePackLibrary::idIsPersistable(std::string_view id) {
    if (id.empty() || id.front() == '#') {
        return false; // `#` 开头会被读成注释
    }
    if (id.find_first_of("\r\n") != std::string_view::npos) {
        return false; // 一行一个 id 的格式表达不了换行
    }
    return trim(id) == id; // 首尾空白会在读回时被去掉
}

std::vector<std::string> ResourcePackLibrary::parseSelection(std::string_view text) {
    std::vector<std::string> order;
    std::size_t cursor = 0U;
    while (cursor <= text.size()) {
        const auto end = text.find('\n', cursor);
        const auto line = trim(text.substr(cursor, end == std::string_view::npos
                                                       ? std::string_view::npos
                                                       : end - cursor));
        if (!line.empty() && line.front() != '#') {
            order.emplace_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1U;
    }
    return order;
}

std::string ResourcePackLibrary::serializeSelection(const std::vector<std::string>& order) {
    std::string text =
        "# ReBedrock 资源包启用列表\n"
        "# 一行一个包 id（resourcepacks/ 下的目录名或 .zip 文件名），自下而上：\n"
        "# 越靠后优先级越高，覆盖前面的包。\n"
        "# 删掉本文件即回到「发现到的包全部启用」。\n";
    for (const auto& id : order) {
        text += id;
        text += '\n';
    }
    return text;
}

void ResourcePackLibrary::buildStack(const ResourceProvider& base) {
    // 每次都从空的 PackManager 起，而不是在旧栈上增量改：启用集合可以任意变化，
    // 而「照草稿从头叠一遍」只有一种结果，增量路径则有 2^n 种到达方式。
    manager_ = PackManager{};
    for (const auto& id : draft_) {
        const std::size_t index = indexOf(id);
        if (index == packs_.size()) {
            continue;
        }
        const Registered& record = registered_[index];
        manager_.registerPack(id, *record.provider, record.metadata,
                              /*hasDataHalf=*/false, /*hasResourceHalf=*/true);
        manager_.enable(PackStackKind::Resources, id);
        // overlay 紧跟在自己那个包之上：一个包的版本门控层只能盖住这个包，
        // 不能越过下一个包。
        for (std::size_t overlay = 0U; overlay < record.overlays.size(); ++overlay) {
            const std::string derived = overlayId(id, overlay);
            manager_.registerPack(derived, *record.overlays[overlay], record.metadata,
                                  /*hasDataHalf=*/false, /*hasResourceHalf=*/true);
            manager_.enable(PackStackKind::Resources, derived);
        }
    }
    // emplace 就地重建，optional 的地址不变——已经把 &provider() 记下来的调用方
    // （渲染器就是）在重建之后仍指向同一个对象。
    stack_.emplace(manager_.buildProvider(PackStackKind::Resources, base));
    active_ = draft_;
}

const ResourceProvider& ResourcePackLibrary::provider() const {
    if (!stack_.has_value()) {
        std::fputs("resource pack library fatal: provider() before buildStack()\n", stderr);
        std::abort();
    }
    return *stack_;
}

} // namespace mc::assets
