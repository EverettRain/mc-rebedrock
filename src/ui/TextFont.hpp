#pragma once

#include "ui/BitmapFontMetrics.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::ui {

// 把 UTF-8 解码成码点
// 非法字节变成 U+FFFD，畸形字符串因此仍能画出些东西，而不是把后面每个字形都错位
[[nodiscard]] std::vector<char32_t> decodeUtf8(std::string_view text);

// 一个已在字体纹理数组上解析好的字形
struct FontGlyph final {
    // 在字体纹理数组里的层号，0 是 ASCII 页，unicode 各页按渲染器上传的顺序接在后面
    float layer = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    float uvWidth = 0.0F;
    float uvHeight = 0.0F;
    // 尺寸与步进值，单位是施加 GUI 缩放之前的 GUI 像素
    float pixelWidth = 0.0F;
    float pixelHeight = 0.0F;
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    float advance = 4.0F;
    bool visible = false;
};

// Java 26.1 的字体提供器栈
// 位图表提供它自己声明的那些码点，空格提供器提供步进值
// unihex 提供器从内嵌的 .hex 归档里填充 BMP 各页
// 打开 forceUnicode 会改选 font/uniform.json
class TextFont final {
  public:
    static constexpr std::size_t kUnicodePageCount = 256U;
    // 一个 unicode 页是 16x16 个格子，每格 16x16 像素，按一半尺寸绘制
    static constexpr float kUnicodePageSize = 256.0F;
    static constexpr float kUnicodeCellSize = 16.0F;
    static constexpr float kUnicodeOversample = 2.0F;

    void setAsciiMetrics(BitmapFontMetrics metrics) { ascii_ = metrics; }
    // 紧凑的 unihex 边界：高半字节是首个被用到的列，低半字节是最后一个
    void setUnicodeSizes(std::vector<std::uint8_t> sizes);
    // 登记某一页被上传到了纹理数组的哪一层
    // 从未上传的页保持 -1，回落到 ASCII 表
    void setUnicodePageLayer(int page, int layer);
    void clearUnicodePages();
    void clearBitmapGlyphs() { bitmapGlyphs_.clear(); }
    // 提供器按字体 JSON 里的顺序访问
    // 第一个提供该码点的提供器胜出，与 FontSet 的提供器优先级一致
    void addBitmapGlyph(char32_t codepoint, FontGlyph glyph) {
        bitmapGlyphs_.try_emplace(codepoint, glyph);
    }
    void clearSpaceAdvances() { spaceAdvances_.clear(); }
    void setSpaceAdvance(char32_t codepoint, float advance) {
        spaceAdvances_.insert_or_assign(codepoint, advance);
    }
    void setForceUnicode(bool force) { forceUnicode_ = force; }

    [[nodiscard]] bool forceUnicode() const { return forceUnicode_; }
    [[nodiscard]] bool hasUnicodePages() const { return !sizes_.empty(); }

    [[nodiscard]] FontGlyph glyph(char32_t codepoint) const;
    [[nodiscard]] float textWidth(std::string_view text, float scale) const;
    // 一行占据的高度，单位 GUI 像素，无论字形来自哪张表都保持 vanilla 的 8
    [[nodiscard]] static constexpr float lineHeight() { return 8.0F; }

  private:
    [[nodiscard]] bool useUnicodeFor(char32_t codepoint) const;
    [[nodiscard]] FontGlyph asciiGlyph(char32_t codepoint) const;
    [[nodiscard]] FontGlyph unicodeGlyph(char32_t codepoint) const;

    BitmapFontMetrics ascii_;
    std::vector<std::uint8_t> sizes_;
    std::array<int, kUnicodePageCount> pageLayers_{};
    std::unordered_map<char32_t, FontGlyph> bitmapGlyphs_;
    std::unordered_map<char32_t, float> spaceAdvances_;
    bool forceUnicode_ = false;
    bool pageLayersInitialized_ = false;
};

} // namespace mc::ui
