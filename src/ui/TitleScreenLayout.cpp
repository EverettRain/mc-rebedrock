#include "ui/TitleScreenLayout.hpp"

#include <array>
#include <stdexcept>

namespace mc::ui {

TitleScreenLayout titleScreenLayout(int logicalWidth, int logicalHeight, int versionTextWidth,
                                    int copyrightTextWidth) {
    // 整数除法，逐字照 26.1：`this.width / 2`、`this.height / 4`。写成浮点再取整会在
    // 奇数画布上差 1px，而奇数画布正是非整除 GUI 缩放的常态（1280/3 -> 427）。
    const int halfWidth = logicalWidth / 2;
    const int menuTop = logicalHeight / 4 + kTitleMenuBaseline;
    // 三个主按钮之后 topPos 停在 j + 2 * spacing，图标行再往下 36。
    const int iconRowY = menuTop + 2 * kTitleMenuSpacing + kTitleIconRowDrop;
    const int footerY = logicalHeight - kTitleFooterTextHeight;

    TitleScreenLayout layout;
    layout.logo = {halfWidth - kTitleLogoWidth / 2, kTitleLogoTop, kTitleLogoWidth,
                   kTitleLogoHeight};
    layout.edition = {halfWidth - kTitleEditionWidth / 2,
                      kTitleLogoTop + kTitleLogoHeight - kTitleEditionOverlap,
                      kTitleEditionWidth, kTitleEditionHeight};
    layout.singleplayer = {halfWidth - kTitleButtonWidth / 2, menuTop, kTitleButtonWidth,
                           kTitleButtonHeight};
    layout.multiplayer = {halfWidth - kTitleButtonWidth / 2, menuTop + kTitleMenuSpacing,
                          kTitleButtonWidth, kTitleButtonHeight};
    layout.realms = {halfWidth - kTitleButtonWidth / 2, menuTop + 2 * kTitleMenuSpacing,
                     kTitleButtonWidth, kTitleButtonHeight};
    layout.language = {halfWidth - 124, iconRowY, kTitleIconButtonSize, kTitleIconButtonSize};
    layout.options = {halfWidth - 100, iconRowY, kTitleHalfButtonWidth, kTitleButtonHeight};
    layout.quit = {halfWidth + 2, iconRowY, kTitleHalfButtonWidth, kTitleButtonHeight};
    layout.accessibility = {halfWidth + 104, iconRowY, kTitleIconButtonSize,
                            kTitleIconButtonSize};
    layout.version = {2, footerY, versionTextWidth, kTitleFooterTextHeight};
    layout.copyright = {logicalWidth - copyrightTextWidth - 2, footerY, copyrightTextWidth,
                        kTitleFooterTextHeight};
    return layout;
}

TitleRect titleWidgetRect(const TitleScreenLayout& layout, std::size_t index) {
    const std::array<TitleRect, kTitleWidgetCount> order{
        layout.singleplayer, layout.multiplayer, layout.realms,      layout.language,
        layout.options,      layout.quit,        layout.accessibility,
    };
    if (index >= order.size()) {
        throw std::out_of_range("title screen widget index is outside 0..6");
    }
    return order[index];
}

} // namespace mc::ui
