#pragma once

#include <cstddef>
#include <vector>

namespace mc::ui {

enum class PageId {
    Title,
    WorldList,
    CreateWorld,
    EditWorld,
    ConfirmDelete,
    Loading,
    Game,
    Pause,
    Death,
    Options,
    VideoSettings,
    Controls,
    Language,
    Experimental,
};

class PageStack final {
  public:
    explicit PageStack(PageId root = PageId::Title);

    [[nodiscard]] PageId current() const;
    [[nodiscard]] PageId root() const;
    [[nodiscard]] std::size_t depth() const { return pages_.size(); }
    [[nodiscard]] bool contains(PageId page) const;

    void push(PageId page);
    bool pop();
    void replace(PageId page);
    void reset(PageId root);

  private:
    std::vector<PageId> pages_;
};

} // namespace mc::ui
