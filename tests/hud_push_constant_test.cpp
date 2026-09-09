// One block of push constants, three consumers, and no one checking they agree.
//
// The regression this file exists to prevent: RN-14 needed somewhere to put the
// icon's box and UV corners, took `hud.color` and `hud.uvRect`, and documented
// the change in hud.vert. hud.frag went on reading `hud.color` as a tint, so
// every block icon's colour became its box's minimum corner and every face's
// alpha became a UV component — which for a box at the origin is black, and for
// two of its three faces is alpha zero. Every icon in the inventory was a black
// diamond. The suite was 238/238 green throughout, because item_cube_uv_test
// cross-checks hud.VERT against the C++ tables and nothing anywhere reads
// hud.frag.
//
// The invariant, stated so it can be tested: A PUSH-CONSTANT FIELD'S MEANING
// MUST NOT CHANGE WITH THE DRAW MODE. A field may go unused in a mode; it may
// never be reinterpreted. The mechanical half of that — every consumer declares
// the same block — is what the parsing below asserts, and it also catches the
// second symptom of the same commit: the two shaders declared four fields and
// five.
//
// And its second half, which the first round did not cover and which cost a
// second P0: DECLARATIONS AGREEING IS NOT PRODUCERS AGREEING. ItemPush's two
// block-item modes declare an identical block — the check above passes — and
// filled it differently. Mode 10 put the face's UV rect in data.yzw plus
// positionSize.w, which is where item_entity.vert reads it; mode 11 put all four
// numbers into positionSize.xyzw, so a held block got a rect of zero width in U
// and every face stretched one column of texels across itself. Where one
// consumer branch has several producers, they go through ONE construction, and
// that construction is what gets tested.

#include "render/vulkan/HudTypes.hpp"
#include "ui/HudLayout.hpp"
#include "ui/ScreenBackground.hpp"
#include "world/Block.hpp"
#include "world/ItemModel.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif
#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif

namespace {

const std::filesystem::path kShaderDir{MC_REBEDROCK_SHADER_SRC_DIR};
const std::filesystem::path kSourceDir{MC_REBEDROCK_SOURCE_DIR};

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    assert(stream && "shader or header source must be readable");
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

struct Field final {
    std::string type;
    std::string name;
    [[nodiscard]] bool operator==(const Field&) const = default;
};

// Strips `//` and `/* */` so a field named inside a comment is not mistaken for a
// declaration. Both blocks below are heavily commented, which is the point.
[[nodiscard]] std::string stripComments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) ++i;
            i = i + 2 < source.size() ? i + 2 : source.size();
        } else {
            out.push_back(source[i]);
            ++i;
        }
    }
    return out;
}

// The `{ ... }` that follows `marker`, comments removed.
[[nodiscard]] std::string blockAfter(std::string_view source, std::string_view marker) {
    const std::string clean = stripComments(source);
    const auto start = clean.find(marker);
    assert(start != std::string::npos && "the declaration must be present");
    const auto open = clean.find('{', start);
    assert(open != std::string::npos);
    int depth = 0;
    for (std::size_t i = open; i < clean.size(); ++i) {
        if (clean[i] == '{') ++depth;
        if (clean[i] == '}') {
            --depth;
            if (depth == 0) {
                return clean.substr(open + 1, i - open - 1);
            }
        }
    }
    assert(false && "unbalanced braces");
    return {};
}

// `<type> <name>;` pairs, in declaration order. Order matters: a push-constant
// block is a memory layout, so two blocks that agree on names but not on order
// are two different structs.
[[nodiscard]] std::vector<Field> parseFields(std::string_view body) {
    std::vector<Field> fields;
    std::istringstream stream{std::string{body}};
    std::string token;
    std::vector<std::string> words;
    while (stream >> token) {
        if (token.back() == ';') {
            token.pop_back();
            if (!token.empty()) {
                words.push_back(token);
            }
            if (words.size() >= 2) {
                fields.push_back({words[words.size() - 2], words.back()});
            }
            words.clear();
        } else {
            words.push_back(token);
        }
    }
    return fields;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// `name = value;` pairs out of a `---- <label>: begin ----` / `end` block. The
// marker-block shape is this repo's existing way of holding a shader constant
// against a C++ one (kFaceInfoCorner set the precedent).
[[nodiscard]] std::vector<std::pair<std::string, float>> markedConstants(
    const std::string& source, std::string_view label) {
    const std::string begin = std::string{"---- "} + std::string{label} + ": begin ----";
    const std::string end = std::string{"---- "} + std::string{label} + ": end ----";
    const auto from = source.find(begin);
    const auto to = source.find(end);
    assert(from != std::string::npos && to != std::string::npos && from < to);
    const std::string body = source.substr(from + begin.size(), to - from - begin.size());
    std::vector<std::pair<std::string, float>> out;
    std::size_t cursor = 0;
    while ((cursor = body.find("kItemMode", cursor)) != std::string::npos) {
        const auto nameEnd = body.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", cursor);
        const std::string name = body.substr(cursor, nameEnd - cursor);
        const auto equals = body.find('=', nameEnd);
        const auto semicolon = body.find(';', equals);
        assert(equals != std::string::npos && semicolon != std::string::npos);
        out.emplace_back(name, std::stof(body.substr(equals + 1, semicolon - equals - 1)));
        cursor = semicolon;
    }
    return out;
}

// Which named modes a predicate's body lists, in the shader source. The predicate
// is a disjunction of `isItemMode(kItemModeX)`, so its membership is readable.
[[nodiscard]] std::vector<std::string> predicateMembers(const std::string& source,
                                                        std::string_view declaration) {
    const auto from = source.find(declaration);
    assert(from != std::string::npos && "the predicate must exist with this spelling");
    const auto to = source.find(';', from);
    assert(to != std::string::npos);
    const std::string body = source.substr(from, to - from);
    std::vector<std::string> members;
    std::size_t cursor = 0;
    while ((cursor = body.find("isItemMode(kItemMode", cursor)) != std::string::npos) {
        cursor += std::string_view{"isItemMode("}.size();
        const auto nameEnd = body.find(')', cursor);
        members.push_back(body.substr(cursor, nameEnd - cursor));
        cursor = nameEnd;
    }
    std::sort(members.begin(), members.end());
    return members;
}

[[nodiscard]] std::size_t occurrencesIn(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find(needle, cursor)) != std::string::npos) {
        ++count;
        cursor += needle.size();
    }
    return count;
}

[[nodiscard]] std::vector<std::string> sortedNames(std::initializer_list<std::string_view> names) {
    std::vector<std::string> out;
    for (const auto name : names) out.emplace_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::string describe(const std::vector<Field>& fields) {
    std::string out;
    for (const Field& field : fields) {
        out += field.type + ' ' + field.name + "; ";
    }
    return out;
}

// glm::vec4 spells its type differently from GLSL's vec4; nothing else in these
// blocks differs, and if something does the assertion should fail rather than the
// mapping quietly growing to accommodate it.
[[nodiscard]] std::vector<Field> normalizeCxx(std::vector<Field> fields) {
    for (Field& field : fields) {
        if (field.type.starts_with("glm::")) {
            field.type = field.type.substr(5);
        }
        // `glm::mat4 viewModelTransform{1.0F}` — the brace initialiser is part of
        // the token, not of the name.
        const auto brace = field.name.find('{');
        if (brace != std::string::npos) {
            field.name = field.name.substr(0, brace);
        }
    }
    return fields;
}

// `layout(location = N) [flat] out|in <type> <name>;`, with the direction keyword
// removed so the two stages' lists are directly comparable. The interpolation
// qualifier is KEPT: Vulkan requires it to match for the interfaces to match, and
// a `flat` on one side only is how a per-vertex value silently stops varying.
[[nodiscard]] std::vector<std::string> varyings(const std::string& source,
                                                std::string_view direction) {
    const std::string clean = stripComments(source);
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while ((cursor = clean.find("layout(location", cursor)) != std::string::npos) {
        const auto end = clean.find(';', cursor);
        if (end == std::string::npos) break;
        std::istringstream words{clean.substr(cursor, end - cursor)};
        cursor = end;
        std::string normalized;
        std::string word;
        bool matches = false;
        while (words >> word) {
            if (word == direction) matches = true;
            if (word == "out" || word == "in") continue;
            normalized += word;
            normalized += ' ';
        }
        if (matches) out.push_back(normalized);
    }
    return out;
}

void assertStagesAgree(std::string_view vertexName, std::string_view fragmentName) {
    const std::string vertex = readFile(kShaderDir / vertexName);
    const std::string fragment = readFile(kShaderDir / fragmentName);
    const auto outputs = varyings(vertex, "out");
    const auto inputs = varyings(fragment, "in");
    assert(!outputs.empty());
    if (outputs != inputs) {
        std::cerr << vertexName << " outputs and " << fragmentName << " inputs disagree:\n";
        for (const auto& line : outputs) std::cerr << "  vert: " << line << "\n";
        for (const auto& line : inputs) std::cerr << "  frag: " << line << "\n";
    }
    assert(outputs == inputs);
}

} // namespace

int main() {
    // --- The three declarations of HudPush must be one declaration. ---
    {
        const auto vertexFields = parseFields(
            blockAfter(readFile(kShaderDir / "hud.vert"), "layout(push_constant) uniform HudPush"));
        const auto fragmentFields = parseFields(
            blockAfter(readFile(kShaderDir / "hud.frag"), "layout(push_constant) uniform HudPush"));
        const auto cxxFields = normalizeCxx(parseFields(
            blockAfter(readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"),
                       "struct HudPush final")));

        assert(!vertexFields.empty());
        if (vertexFields != fragmentFields) {
            std::cerr << "hud.vert declares [" << describe(vertexFields) << "]\n"
                      << "hud.frag declares [" << describe(fragmentFields) << "]\n";
        }
        // The whole regression in one line: the two stages read the same bytes,
        // so they must read them as the same fields. Field COUNT alone would have
        // caught RN-14's four-versus-five; names and order catch a rename or a
        // reordering, which corrupt the layout just as thoroughly and are the more
        // likely future mistake.
        assert(vertexFields == fragmentFields);
        assert(vertexFields == cxxFields);
        assert(sizeof(mc::render::HudPush) == vertexFields.size() * sizeof(glm::vec4));
        assert(sizeof(mc::render::HudPush) <= 128U);
    }

    // --- UI-5: GradientPush 同样是三处声明，同样必须是一处。 ---
    //
    // 它之所以另开一个块而不是做成 HudPush 的又一个绘制模式：一条渐变要**两个**颜色，
    // 而 HudPush 已经正好 128 字节，一个自由分量都不剩。硬塞只能让某个字段随模式改变
    // 含义——那正是这个文件顶上那条铁律禁止的事。
    {
        const auto vertexFields = parseFields(blockAfter(
            readFile(kShaderDir / "gradient.vert"), "layout(push_constant) uniform GradientPush"));
        const auto fragmentFields = parseFields(blockAfter(
            readFile(kShaderDir / "gradient.frag"), "layout(push_constant) uniform GradientPush"));
        const auto cxxFields = normalizeCxx(parseFields(blockAfter(
            readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"), "struct GradientPush final")));
        assert(!vertexFields.empty());
        assert(vertexFields == fragmentFields);
        assert(vertexFields == cxxFields);
        assert(sizeof(mc::render::GradientPush) == vertexFields.size() * sizeof(glm::vec4));
    }

    // --- UI-5: 渐变的**生产者**也只有一个，而且它没把上下两端接反。 ---
    //
    // 声明一致不等于填法一致（这个文件顶上第二条教训）。makeGradientPush 是那唯一的
    // 构造，而"上下接反"在画面上只是"渐变方向反了"——死亡屏仍然是一片暗红，
    // 只是重的那头跑到了上面。没有任何自然的断言会因此变红，所以在这里钉住。
    {
        const mc::ui::UiRect clip{-1.0F, -1.0F, 2.0F, 2.0F};
        const auto push = mc::render::makeGradientPush(clip, mc::ui::kDeathBackgroundStops);
        assert(push.rect.x == -1.0F && push.rect.y == -1.0F);
        assert(push.rect.z == 2.0F && push.rect.w == 2.0F);
        // 顶 = 0x60500000：alpha 0x60、红 0x50、绿蓝 0
        assert(push.topColor.a == 96.0F / 255.0F);
        assert(push.topColor.r == 80.0F / 255.0F);
        assert(push.topColor.g == 0.0F && push.topColor.b == 0.0F);
        // 底 = 0xA0803030：更不透明、更亮
        assert(push.bottomColor.a == 160.0F / 255.0F);
        assert(push.bottomColor.r == 128.0F / 255.0F);
        assert(push.bottomColor.g == 48.0F / 255.0F);
        // ★ 方向：底端必须比顶端重。接反了这一条就红。
        assert(push.bottomColor.a > push.topColor.a);
        assert(push.bottomColor.r > push.topColor.r);
        // 容器那一档只有 alpha 在变，RGB 两端相同——它抓不住"接反"，所以上面用死亡屏
        const auto dim = mc::render::makeGradientPush(clip, mc::ui::kTransparentBackgroundStops);
        assert(dim.topColor.r == dim.bottomColor.r);
        assert(dim.bottomColor.a > dim.topColor.a);
        // gradient.vert 里那次 mix 的 t 是 corner.y：0 是矩形上缘。把 mix 的两个参数
        // 对调与把这里的两个字段对调是同一个缺陷，所以着色器那半也钉一下。
        const std::string vertex = stripComments(readFile(kShaderDir / "gradient.vert"));
        assert(vertex.find("mix(gradient.topColor, gradient.bottomColor, corner.y)") !=
               std::string::npos);
    }

    // --- Each named draw mode lands in the branch it is named for. ---
    //
    // Naming the modes removed one hazard and introduced another: hud.frag
    // dispatches with `hud.data.x > N` against half-way thresholds, so a constant
    // and a threshold are again two numbers that have to agree. The thresholds
    // are read out of the shader and the constants run through them here.
    {
        const std::string fragment = stripComments(readFile(kShaderDir / "hud.frag"));
        std::vector<float> thresholds;
        std::size_t cursor = 0;
        while ((cursor = fragment.find("hud.data.x > ", cursor)) != std::string::npos) {
            cursor += std::string_view{"hud.data.x > "}.size();
            thresholds.push_back(std::stof(fragment.substr(cursor, 8)));
        }
        // UI-2 加了 kHudModeTitleTexture（主菜单 logo / edition），阈值随之从五个变六个。
        assert(thresholds.size() == 6);
        std::sort(thresholds.begin(), thresholds.end());
        // The if-chain is nested rather than flat (the icon and the gui sprite
        // share an outer branch), so this does not model the control flow. It
        // asserts the property the control flow depends on: the thresholds cut
        // the line into intervals, and no two modes may share one — two modes in
        // one interval is two draws taking the same branch.
        const std::array modes{mc::render::kHudModeFlat,        mc::render::kHudModeBlockTexture,
                               mc::render::kHudModeFontGlyph,   mc::render::kHudModeGuiSprite,
                               mc::render::kHudModeBlockIcon,   mc::render::kHudModeCrosshair,
                               mc::render::kHudModeTitleTexture};
        std::vector<std::size_t> buckets;
        for (const float mode : modes) {
            std::size_t bucket = 0;
            for (const float threshold : thresholds) {
                if (mode > threshold) {
                    ++bucket;
                }
                // Sitting on a threshold is the reason to keep the modes at whole
                // numbers (and 4.25, which is not one): a mode that lands on a
                // comparison is one rounding away from changing branch.
                assert(std::abs(mode - threshold) > 0.2F);
            }
            buckets.push_back(bucket);
        }
        std::sort(buckets.begin(), buckets.end());
        assert(std::adjacent_find(buckets.begin(), buckets.end()) == buckets.end());
        // And they are in the order the names imply, which is what makes the
        // constants readable at the call sites.
        assert(std::is_sorted(modes.begin(), modes.end()));
    }

    // --- OutlinePush, the third block, held the same way. ---
    //
    // RN-16 changed what one outline draw IS: from a box (origin + min + max,
    // twelve edges generated in the shader) to a single LINE of the merged
    // shape's edge list. Same three vec4s, entirely different meaning for two of
    // them — which is the exact shape of the regression this file exists for. A
    // shader left reading `boundsMin`/`boundsMax` while the renderer pushed
    // segment endpoints would still compile, still draw, and draw a wireframe of
    // a box stretched between two arbitrary corners of the shape.
    //
    // block_outline.frag reads no push block, so as with ItemPush there is no
    // second consumer to disagree; the two declarations that DO exist are held
    // together here, and the sole producer below.
    {
        const auto shaderFields = parseFields(blockAfter(
            readFile(kShaderDir / "block_outline.vert"), "layout(push_constant) uniform OutlinePush"));
        const auto cxxFields = normalizeCxx(parseFields(blockAfter(
            readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"), "struct OutlinePush final")));
        assert(!shaderFields.empty());
        if (shaderFields != cxxFields) {
            std::cerr << "block_outline.vert declares [" << describe(shaderFields) << "]\n"
                      << "OutlinePush declares [" << describe(cxxFields) << "]\n";
        }
        assert(shaderFields == cxxFields);
        assert(sizeof(mc::render::OutlinePush) == shaderFields.size() * sizeof(glm::vec4));
        assert(sizeof(mc::render::OutlinePush) <= 128U);
        assert(!contains(readFile(kShaderDir / "block_outline.frag"), "push_constant") &&
               "block_outline.frag reads no push constants; if it starts to, it joins the "
               "comparison above");

        // The producer, on the CPU: the endpoints go through untouched and the
        // origin is the block. Nothing here may transform the segment — the
        // shader adds the origin, and doing it on both sides would double it.
        const mc::render::OutlineSegment segment{{0.25F, 0.5F, 0.75F}, {0.25F, 1.0F, 0.75F}};
        const mc::render::OutlinePush push =
            mc::render::makeOutlineSegmentPush(glm::ivec3{12, -34, 56}, segment, 3840.0F, 2160.0F);
        assert(glm::vec3(push.blockOrigin) == glm::vec3(12.0F, -34.0F, 56.0F));
        assert(glm::vec3(push.segmentStart) == segment.start);
        assert(glm::vec3(push.segmentEnd) == segment.end);
        // RN-45：三个 w 分量装的是撑宽那一步要的两个数。线宽是**帧缓冲宽度的函数**，
        // 不是常数——4K 上是 5 像素，1080p 上是 2.5
        assert(push.blockOrigin.w == mc::render::outlineLineWidthPixels(3840.0F));
        assert(push.blockOrigin.w == 5.0F);
        assert(push.segmentStart.w == 3840.0F && push.segmentEnd.w == 2160.0F);
        // ★ 宽与高不能写反：写反的症状是描边在非正方形窗口上按错误的比例变粗，
        // 而正方形的出图（512x512）**看不出来**——这一条正是出图夹具的盲区
        assert(push.segmentStart.w != push.segmentEnd.w);
        const mc::render::OutlinePush narrow =
            mc::render::makeOutlineSegmentPush(glm::ivec3{0, 0, 0}, segment, 1280.0F, 720.0F);
        assert(narrow.blockOrigin.w == 2.5F && "1920 以下恒为 2.5 像素");
        // A start and an end that came out equal is a zero-length line: the two
        // fields have to stay two fields, which is what a copy-paste in the maker
        // would undo.
        assert(push.segmentStart != push.segmentEnd);
    }

    // --- ItemPush, the sibling block, held the same way. ---
    //
    // The dropped-item and held-item path that RN-14 also rewrote. It is
    // structurally immune to the regression above — item_entity.frag declares no
    // push block at all, so there is no second reader to disagree — but the C++
    // struct and the vertex shader are still two declarations of one layout.
    {
        const auto shaderFields = parseFields(blockAfter(
            readFile(kShaderDir / "item_entity.vert"), "layout(push_constant) uniform ItemPush"));
        const auto cxxFields = normalizeCxx(parseFields(blockAfter(
            readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"), "struct ItemPush final")));
        assert(!shaderFields.empty());
        if (shaderFields != cxxFields) {
            std::cerr << "item_entity.vert declares [" << describe(shaderFields) << "]\n"
                      << "ItemPush declares [" << describe(cxxFields) << "]\n";
        }
        assert(shaderFields == cxxFields);
        assert(!contains(readFile(kShaderDir / "item_entity.frag"), "push_constant") &&
               "item_entity.frag reads no push constants; if it starts to, it joins the "
               "comparison above");
    }

    // --- The draw modes: one list, and a real partition over it. ---
    //
    // C block. `data.x` selects one of fifteen kinds of draw, and the shader used
    // to express both "which mode is this" and "which modes share this property"
    // with bare thresholds. The two are different things and only the first is
    // exclusive — `blockItemBox` covers modes 10 and 11 deliberately, and making
    // it exclusive would restore the regression above. What is asserted here is
    // that the modes partition, that each category's membership is a list someone
    // wrote, and that the shader and C++ agree about both.
    {
        const std::string shader = stripComments(readFile(kShaderDir / "item_entity.vert"));
        const std::string types =
            stripComments(readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"));

        // (a) One list of modes, two declarations of it.
        // Raw, not stripped: the begin/end markers themselves live in comments.
        const auto shaderModes =
            markedConstants(readFile(kShaderDir / "item_entity.vert"), "item draw modes");
        const auto cxxModes = markedConstants(
            readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"), "item draw modes");
        if (shaderModes != cxxModes) {
            std::cerr << "item draw modes disagree between item_entity.vert and HudTypes.hpp\n";
            for (const auto& [name, value] : shaderModes)
                std::cerr << "  shader: " << name << " = " << value << "\n";
            for (const auto& [name, value] : cxxModes)
                std::cerr << "  c++:    " << name << " = " << value << "\n";
        }
        assert(shaderModes == cxxModes);
        assert(shaderModes.size() == mc::render::kItemModes.size());
        // Distinct values, at least 1.0 apart — the comparison is a +/-0.5 window
        // around each, so anything closer makes two modes ambiguous.
        for (std::size_t i = 0; i < shaderModes.size(); ++i) {
            for (std::size_t j = i + 1; j < shaderModes.size(); ++j) {
                assert(std::abs(shaderModes[i].second - shaderModes[j].second) >= 1.0F);
            }
        }
        // And every raw threshold is gone: the only comparison against data.x
        // left in the shader is the one inside isItemMode.
        assert(occurrencesIn(shader, "item.data.x >") + occurrencesIn(shader, "item.data.x <") == 2);

        // (b) COMPLETE and (c) EXCLUSIVE: every mode takes exactly one top-level
        // branch. A mode that fell into two, or into none, is a draw that renders
        // twice or not at all.
        for (const float mode : mc::render::kItemModes) {
            const int branches = static_cast<int>(mc::render::itemBranchGeneratedItem(mode)) +
                                 static_cast<int>(mc::render::itemBranchShadow(mode)) +
                                 static_cast<int>(mc::render::itemBranchCuboid(mode)) +
                                 static_cast<int>(mc::render::itemBranchBillboard(mode));
            if (branches != 1) {
                std::cerr << "mode " << mode << " takes " << branches
                          << " top-level branches; expected exactly 1\n";
            }
            assert(branches == 1);
        }
        // A value BETWEEN two modes must not be claimed by the cuboid branch's
        // membership list — that is what an inserted mode looks like before anyone
        // adds it to a category on purpose.
        assert(!mc::render::itemBranchCuboid(11.5F));
        assert(!mc::render::itemBranchGeneratedItem(7.5F));

        // (d) NO ORPHANS. What the shader knows, minus what the renderer sends,
        // must be exactly the list that says why each one is still there.
        {
            std::vector<std::string> produced;
            for (const auto& file : {"src/render/vulkan/WorldRenderer.hpp",
                                     "src/render/vulkan/HudRenderer.hpp"}) {
                const std::string body = stripComments(readFile(kSourceDir / file));
                for (const auto& [name, value] : cxxModes) {
                    static_cast<void>(value);
                    if (contains(body, name) &&
                        std::find(produced.begin(), produced.end(), name) == produced.end()) {
                        produced.push_back(name);
                    }
                }
            }
            // The two block-item modes are produced through their makers, which
            // the section below asserts are called exactly once each.
            produced.emplace_back("kItemModeBlockItemDropped");
            produced.emplace_back("kItemModeBlockItemHeld");
            std::vector<std::string> orphans;
            for (const auto& [name, value] : cxxModes) {
                static_cast<void>(value);
                if (std::find(produced.begin(), produced.end(), name) == produced.end()) {
                    orphans.push_back(name);
                }
            }
            std::sort(orphans.begin(), orphans.end());
            std::vector<std::string> accounted;
            for (const float mode : mc::render::kItemModesWithoutProducer) {
                for (const auto& [name, value] : cxxModes) {
                    if (value == mode) accounted.push_back(name);
                }
            }
            std::sort(accounted.begin(), accounted.end());
            if (orphans != accounted) {
                std::cerr << "modes the shader knows but nothing pushes have changed:\n";
                for (const auto& name : orphans) std::cerr << "  found:    " << name << "\n";
                for (const auto& name : accounted) std::cerr << "  accounted: " << name << "\n";
            }
            // Not "there are none" — there are six, and they are listed with a
            // reason in kItemModesWithoutProducer. What must not happen is a new
            // one appearing without anyone saying why.
            assert(orphans == accounted);
            std::cout << "item modes: " << cxxModes.size() << " known, "
                      << produced.size() << " produced, " << orphans.size()
                      << " accounted as unused\n";
        }

        // (e) CATEGORIES: membership is a list, asserted mode by mode, in both
        // declarations. Inserting a mode is then a decision about every category
        // rather than a side effect of where a threshold sits.
        struct CategoryCase final {
            std::string_view declaration;
            std::vector<std::string> members;
            bool (*mirror)(float);
        };
        const std::vector<CategoryCase> categories{
            {"bool blockItemBox =",
             sortedNames({"kItemModeBlockItemDropped", "kItemModeBlockItemHeld"}),
             &mc::render::itemBlockItemBox},
            {"bool heldInViewSpace =",
             sortedNames({"kItemModeHeldSprite", "kItemModeViewSkinCuboid"}),
             nullptr}, // matrixViewModel and blockItemHeld arrive via named bools
            // RN-40：闭合盒的清单。这一条是补出来的——原来着色器里写的是单一模式，
            // 没有任何东西比对它与「哪些模式画的是闭合盒」这份认识
            {"bool closedBox =",
             sortedNames({"kItemModeBlockCube", "kItemModeBlockItemDropped",
                          "kItemModeBlockItemHeld"}),
             &mc::render::itemClosedBox},
            {"bool heldBillboard =", sortedNames({"kItemModeHeldBillboard"}), nullptr},
            {"bool atlasBillboard =", sortedNames({"kItemModeAtlasBillboard"}),
             &mc::render::itemAtlasBillboard},
        };
        for (const CategoryCase& category : categories) {
            const auto members = predicateMembers(shader, category.declaration);
            if (members != category.members) {
                std::cerr << "category " << category.declaration << " lists:\n";
                for (const auto& m : members) std::cerr << "  shader: " << m << "\n";
                for (const auto& m : category.members) std::cerr << "  expected: " << m << "\n";
            }
            assert(members == category.members);
        }
        // And the C++ mirrors, mode by mode rather than by threshold. This is the
        // assertion that would fail if anyone "tidied" blockItemBox into an
        // exclusive test: it must be true for BOTH block-item modes.
        assert(mc::render::itemBlockItemBox(mc::render::kItemModeBlockItemDropped));
        assert(mc::render::itemBlockItemBox(mc::render::kItemModeBlockItemHeld));
        for (const float mode : mc::render::kItemModes) {
            const bool expected = mode == mc::render::kItemModeBlockItemDropped ||
                                  mode == mc::render::kItemModeBlockItemHeld;
            assert(mc::render::itemBlockItemBox(mode) == expected);
        }
        // RN-40：手持的方块物品必须在闭合盒里，掉落的也是。生物模型必须不在——
        // 它镜像，按 gl_FrontFacing 丢会剃掉半个身体
        assert(mc::render::itemClosedBox(mc::render::kItemModeBlockItemHeld));
        assert(mc::render::itemClosedBox(mc::render::kItemModeBlockItemDropped));
        assert(!mc::render::itemClosedBox(mc::render::kItemModeBoxUvEntity));
        for (const float mode : mc::render::kItemModes) {
            const bool expected = mode == mc::render::kItemModeBlockCube ||
                                  mode == mc::render::kItemModeBlockItemDropped ||
                                  mode == mc::render::kItemModeBlockItemHeld;
            assert(mc::render::itemClosedBox(mode) == expected);
        }
        for (const float mode : mc::render::kItemModes) {
            const bool expected = mode == mc::render::kItemModeMatrixViewModel ||
                                  mode == mc::render::kItemModeWorldMatrixCuboid ||
                                  mode == mc::render::kItemModeBoxUvEntity ||
                                  mode == mc::render::kItemModeBlockItemHeld;
            assert(mc::render::itemUsesMatrix(mode) == expected);
        }
        for (const float mode : mc::render::kItemModes) {
            const bool expected = mode == mc::render::kItemModeHeldSprite ||
                                  mode == mc::render::kItemModeViewSkinCuboid ||
                                  mode == mc::render::kItemModeMatrixViewModel ||
                                  mode == mc::render::kItemModeBlockItemHeld;
            assert(mc::render::itemHeldInViewSpace(mode) == expected);
        }
        for (const float mode : mc::render::kItemModes) {
            const bool expected = mode == mc::render::kItemModeHeldBillboard ||
                                  mode == mc::render::kItemModeMatrixHeldBillboard;
            assert(mc::render::itemHeldBillboard(mode) == expected);
        }
        // playerSkinCuboid's fourth member is conditional on data.w, which is why
        // it takes two arguments. Both halves asserted.
        for (const float mode : mc::render::kItemModes) {
            const bool unconditional = mode == mc::render::kItemModeViewSkinCuboid ||
                                       mode == mc::render::kItemModeArticulatedCuboid ||
                                       mode == mc::render::kItemModeWorldMatrixCuboid;
            assert(mc::render::itemPlayerSkinCuboid(mode, 0.0F) == unconditional);
            const bool withFlag =
                unconditional || mode == mc::render::kItemModeMatrixViewModel;
            assert(mc::render::itemPlayerSkinCuboid(mode, 1.0F) == withFlag);
        }
    }

    // --- Both block-item producers put the face's UV rect in the same fields. ---
    //
    // The observation surface of the second regression, on the CPU: build each
    // mode's push through the shared writer and read it back exactly as
    // item_entity.vert does. Every present face of every box in the roster, not
    // one block — the held path was wrong for all of them and it still took an
    // eye on a screen to notice.
    {
        // The read is a mirror of a shader line, so the shader line is checked
        // too. Drift here would make the rest of this section agree with itself
        // and with nothing that runs on the GPU.
        assert(contains(stripComments(readFile(kShaderDir / "item_entity.vert")),
                        "vec4(item.data.y, item.data.z, item.data.w, item.positionSize.w)"));

        std::size_t facesChecked = 0;
        for (std::size_t index = 0; index < mc::world::kItemModelBoxes.size(); ++index) {
            const mc::world::ItemModelBox& box = mc::world::kItemModelBoxes[index];
            for (const mc::world::ItemModelFace& face : box.face) {
                if (!face.present) {
                    continue;
                }
                const glm::vec4 expected{face.uv.minU / 16.0F, face.uv.minV / 16.0F,
                                         face.uv.maxU / 16.0F, face.uv.maxV / 16.0F};
                // Two modes, two very different pushes around the same rect: the
                // dropped one carries a world position in positionSize.xyz and a
                // yaw, the held one a matrix. Neither may move the rect.
                const mc::render::ItemPush dropped = mc::render::makeDroppedBlockItemFacePush(
                    face, /*atlasLayer=*/3.0F, /*boxCentre=*/{12.0F, 34.0F, 56.0F},
                    /*size=*/{0.3F, 0.3F, 0.3F}, /*yawRadians=*/1.25F, /*packedLight=*/200.0F);
                const mc::render::ItemPush held = mc::render::makeHeldBlockItemFacePush(
                    face, /*atlasLayer=*/3.0F, /*size=*/{0.4F, 0.4F, 0.4F},
                    /*packedLight=*/200.0F, glm::mat4{1.0F});

                const glm::vec4 droppedRect = mc::render::blockItemFaceRectOf(dropped);
                const glm::vec4 heldRect = mc::render::blockItemFaceRectOf(held);
                if (droppedRect != expected || heldRect != expected) {
                    static constexpr std::array kFieldNames{"data.y (minU)", "data.z (minV)",
                                                            "data.w (maxU)",
                                                            "positionSize.w (maxV)"};
                    std::cerr << "block-item face rect misplaced, box " << index << ":\n";
                    for (int c = 0; c < 4; ++c) {
                        if (droppedRect[c] != expected[c] || heldRect[c] != expected[c]) {
                            std::cerr << "  " << kFieldNames[static_cast<std::size_t>(c)]
                                      << ": expected " << expected[c] << ", dropped got "
                                      << droppedRect[c] << ", held got " << heldRect[c] << "\n";
                        }
                    }
                }
                assert(droppedRect == expected);
                assert(heldRect == expected);
                // The two modes differ in everything except the rect. A shared
                // writer that also flattened the rest would pass the lines above
                // and break both paths.
                assert(dropped.positionSize.x == 12.0F);
                assert(held.positionSize.x == 0.0F);
                assert(dropped.data.x == mc::render::kItemModeBlockItemDropped);
                assert(held.data.x == mc::render::kItemModeBlockItemHeld);
                // The quadrant is the other number both modes carry, and it rides
                // textureLayersRotation.y in each.
                assert(dropped.textureLayersRotation.y == static_cast<float>(face.quadrant));
                assert(held.textureLayersRotation.y == static_cast<float>(face.quadrant));
                ++facesChecked;
            }
        }
        assert(facesChecked > 100);
        std::cout << "block-item faces checked: " << facesChecked << "\n";
    }

    // --- Neither producer spells the rect out for itself. ---
    //
    // The section above tests the shared writer; this one tests that the two call
    // sites still use it. Source text, in the style this repo already uses to hold
    // shader constants against C++ tables — there is no other way to reach code
    // inside WorldRenderer.hpp, which no test links.
    {
        const std::string source =
            stripComments(readFile(kSourceDir / "src/render/vulkan/WorldRenderer.hpp"));
        const std::string types =
            stripComments(readFile(kSourceDir / "src/render/vulkan/HudTypes.hpp"));
        const auto occurrences = occurrencesIn;
        // The two block-item modes appear nowhere in the renderer: their pushes
        // are assembled in HudTypes.hpp. A mode name at a call site is someone
        // about to assemble one by hand.
        for (const std::string_view mode :
             {"kItemModeBlockItemDropped", "kItemModeBlockItemHeld"}) {
            assert(occurrences(types, mode) > 0);
            if (occurrences(source, mode) != 0) {
                std::cerr << "WorldRenderer.hpp names " << mode
                          << "; the block-item push is assembled in HudTypes.hpp\n";
            }
            assert(occurrences(source, mode) == 0);
        }
        // Each maker is called from exactly one place.
        for (const std::string_view maker :
             {"makeDroppedBlockItemFacePush", "makeHeldBlockItemFacePush",
              "makeOutlineSegmentPush"}) {
            const std::size_t uses = occurrences(source, maker);
            if (uses != 1) {
                std::cerr << "WorldRenderer.hpp calls " << maker << ' ' << uses
                          << " time(s); expected exactly one\n";
            }
            assert(uses == 1);
        }
        // And no one writes the rect by hand any more. `face.uv.` appearing in
        // this file is exactly what the two producers used to disagree about.
        const std::size_t handWritten = occurrences(source, "face.uv.");
        if (handWritten != 0) {
            std::cerr << "WorldRenderer.hpp still spells out face.uv " << handWritten
                      << " time(s); the rect has one writer\n";
        }
        assert(handWritten == 0);

        // RN-16: nor does anyone assemble the outline push by hand. It used to be
        // a bare `std::array<glm::vec4, 3>` right at the draw call — three
        // unnamed fields, which is why nothing could hold them against the
        // shader's three named ones until now.
        const std::size_t looseOutlinePush = occurrences(source, "std::array<glm::vec4, 3>");
        if (looseOutlinePush != 0) {
            std::cerr << "WorldRenderer.hpp builds an unnamed three-vec4 push " << looseOutlinePush
                      << " time(s); OutlinePush has one writer\n";
        }
        assert(looseOutlinePush == 0);
        assert(occurrences(types, "makeOutlineSegmentPush") > 0);
    }

    // --- The varyings between each pair of stages, likewise. ---
    //
    // Same commit, same shape, and it does not show up as a black diamond so it
    // would have gone unnoticed for longer: RN-14 made fragmentLight per-vertex
    // ("so the corner AO can shade the face with a gradient") and hud.frag kept
    // declaring it `flat`. Vulkan requires the interpolation decoration to match
    // for the interfaces to match; where it does not, the gradient is dropped at
    // best and the behaviour is undefined at worst.
    //
    // item_entity is checked alongside it because it is the other half of what
    // RN-14 touched, and because the check costs one line.
    assertStagesAgree("hud.vert", "hud.frag");
    assertStagesAgree("item_entity.vert", "item_entity.frag");

    // --- The icon path's tint is white, on every face of every box. ---
    //
    // This is the observation surface of the regression, on the CPU: under the
    // bug the tint was the box's minimum corner (black for a block at the origin)
    // and the alpha was a UV component (zero for two of the three faces). Both
    // are read straight out of world::kItemIconBoxes, so no GPU is involved.
    {
        const mc::ui::UiRect clip{0.1F, 0.2F, 0.3F, 0.4F};
        std::size_t facesChecked = 0;
        for (std::size_t index = 0; index < static_cast<std::size_t>(mc::world::Block::Count);
             ++index) {
            const auto block = static_cast<mc::world::Block>(index);
            const auto range = mc::world::itemModelRange(block);
            for (std::size_t b = 0; b < range.count; ++b) {
                const mc::world::IconBox& icon =
                    mc::world::kItemIconBoxes[static_cast<std::size_t>(range.first) + b];
                for (std::size_t f = 0; f < mc::world::kIconFaces.size(); ++f) {
                    if (!icon.present[f]) {
                        continue;
                    }
                    const mc::render::HudPush push =
                        mc::render::makeBlockIconPush(clip, icon, f, 7.0F);
                    ++facesChecked;
                    // hud.frag does `color.rgb *= texel.rgb * light` and
                    // `color.a *= texel.a`. A tint of anything but opaque white
                    // means the icon is showing something other than its texture.
                    assert(push.color == glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));
                    // The final alpha is color.a * texel.a. Whatever the texel is,
                    // a zero here makes the face invisible — which is exactly what
                    // "only the top diamond survived" was.
                    assert(push.color.a != 0.0F);
                    // The clip rectangle is the clip rectangle in every mode.
                    assert(push.rect == glm::vec4(clip.x, clip.y, clip.width, clip.height));
                    // And the mode and the layer are where hud.frag looks for them.
                    assert(push.data.x == mc::render::kHudModeBlockIcon);
                    assert(push.data.y == 7.0F);
                }
            }
        }
        // A guard on the guard: if the roster or the icon tables were ever emptied
        // the loop above would pass by doing nothing.
        assert(facesChecked > 100);
        std::cout << "icon faces checked: " << facesChecked << "\n";
    }

    // --- The geometry is in the icon fields, and only there. ---
    {
        mc::world::IconBox icon{};
        icon.from = {0.25F, 0.5F, 0.75F};
        icon.to = {1.0F, 0.875F, 0.5F};
        icon.present = {true, false, false};
        icon.uvCorner[0] = {glm::vec2{0.1F, 0.2F}, glm::vec2{0.3F, 0.4F}, glm::vec2{0.5F, 0.6F},
                            glm::vec2{0.7F, 0.8F}};
        const mc::render::HudPush push =
            mc::render::makeBlockIconPush(mc::ui::UiRect{}, icon, 0, 3.0F);
        assert(glm::vec3(push.iconBoxMin) == icon.from);
        assert(glm::vec3(push.iconBoxMax) == icon.to);
        assert(push.iconUv01 == glm::vec4(0.1F, 0.2F, 0.3F, 0.4F));
        assert(push.iconUv23 == glm::vec4(0.5F, 0.6F, 0.7F, 0.8F));
        // uvRect is the sprite rectangle in the modes that have one, and unused
        // here. What it must never be again is the box's maximum corner.
        assert(push.uvRect == glm::vec4(0.0F));
    }

    return 0;
}
