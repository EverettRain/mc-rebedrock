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
    // UI-6d：**「实验性内容」那一页没有了**。雨、粒子、雨碰撞缓存是渲染表现项，
    // 26.1 里它们的同类就在视频设置那一屏；太阳阴影与动态光源归新的 AdvancedGraphics。
    // 留一个不可达的枚举项没有意义——删掉之后，凡是按 PageId 分派的地方都会被
    // -Wswitch 点名，一处不漏。
    AdvancedGraphics,
    // UI-6c：`Controls` 从前把 26.1 的**两屏**合成了一屏（偏差 D1）。
    // 26.1 的 §7.6 `ControlsScreen` 是个排版枢纽（两个跳转按钮加七个设置项），
    // §7.8 `KeyBindsScreen` 才是那张绑定列表。
    KeyBinds,
    // UI-6c：26.1 的 §7.11 `AccessibilityOptionsScreen`。View Bobbing 属于这里，
    // 不属于 Controls（偏差 D2）。
    Accessibility,
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
