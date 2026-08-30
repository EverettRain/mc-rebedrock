#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::assets {
class ResourceProvider;
}

namespace mc::ui {

// 描述 id 使用的非拥有型复合查找键
// entries_ 仍以字符串的形式拥有普通的 JSON 键
// 它那套透明的哈希与相等比较能直接比对这个三段式视图，因此不必拼出一个临时的 std::string
struct TranslationKeyView final {
    std::string_view prefix;
    std::string_view space;
    std::string_view path;
};

struct TranslationKeyHash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& key) const noexcept {
        return (*this)(std::string_view{key});
    }
    [[nodiscard]] std::size_t operator()(const TranslationKeyView& key) const noexcept;
};

struct TranslationKeyEqual final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left,
                                  std::string_view right) const noexcept {
        return left == right;
    }
    [[nodiscard]] bool operator()(const std::string& left,
                                  const TranslationKeyView& right) const noexcept;
    [[nodiscard]] bool operator()(const TranslationKeyView& left,
                                  const std::string& right) const noexcept {
        return (*this)(right, left);
    }
};

// 游戏自带翻译的那些语言代码
// en_us 是内置兜底，它永远不需要磁盘上有文件
inline constexpr const char* kDefaultLanguageCode = "en_us";

struct LanguageInfo final {
    std::string code;
    std::string name;
    std::string region;
    bool bidirectional = false;

    [[nodiscard]] std::string displayName() const;
};

// 只读每个 pack.mcmeta 里那一小段语言块
// 它尤其不会枚举或解析 lang/*.json，因此哪怕有几百种翻译，语言菜单打开的耗时实际上仍是常数
[[nodiscard]] std::vector<LanguageInfo>
availableLanguages(const assets::ResourceProvider& provider);

[[nodiscard]] std::vector<std::string>
availableLanguageCodes(const std::filesystem::path& localizationRoot);

[[nodiscard]] std::vector<std::string>
availableLanguageCodes(const assets::ResourceProvider& provider);

// 一张扁平的 Minecraft 式翻译表，形如 "block.minecraft.stone" 映到 "石头"
class Language final {
  public:
    Language() = default;

    // 解析一个 vanilla 的 <code>.json 语言文件
    // 读不出或格式不对时抛出，调用方因此能回落到英文
    [[nodiscard]] static Language fromFile(const std::filesystem::path& file);
    // 解析已经在内存里的语言 JSON
    // 合并路径走它，zip 资源包因此永远不必把语言文件落到磁盘上
    [[nodiscard]] static Language fromJsonText(std::string_view text);
    [[nodiscard]] static Language fromProvider(const assets::ResourceProvider& provider,
                                               std::string_view code);

    [[nodiscard]] const std::string& code() const { return code_; }
    void setCode(std::string code) { code_ = std::move(code); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // 返回该键的翻译，键不存在时返回兜底文本
    // 兜底让 vanilla 没有对应键的那少数几条字符串仍以英文可见
    [[nodiscard]] std::string_view translate(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] std::string_view translate(std::string_view prefix, std::string_view space,
                                             std::string_view path,
                                             std::string_view fallback) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    // 所有翻译文本用到的那些 256 字形的 unicode 字体页，渲染器因此只上传这些页
    [[nodiscard]] std::set<int> requiredUnicodePages() const;

  private:
    std::string code_ = kDefaultLanguageCode;
    std::unordered_map<std::string, std::string, TranslationKeyHash, TranslationKeyEqual> entries_;
};

// 格式化 Java 语言 JSON 使用的 %s 与 %1$s 占位符，%% 变成一个字面百分号
// 让它独立于渲染，选项标签因此能直接用 options.generic_value 与 options.percent_value
// 而不必围着被翻译的片段写死英文的标点与语序
[[nodiscard]] std::string formatTranslation(
    std::string_view pattern, std::span<const std::string_view> arguments);

struct LanguageLoadResult final {
    std::string code;
    Language language;
    std::string error;
};

// 在渲染线程之外准备好选中的翻译栈
// 结果由调用方在帧边界处应用，替换完成之前旧的 Language 一直有效且可见
class AsyncLanguageLoader final {
  public:
    explicit AsyncLanguageLoader(const assets::ResourceProvider& provider);
    ~AsyncLanguageLoader() = default;

    AsyncLanguageLoader(const AsyncLanguageLoader&) = delete;
    AsyncLanguageLoader& operator=(const AsyncLanguageLoader&) = delete;

    [[nodiscard]] bool start(std::string code);
    [[nodiscard]] std::optional<LanguageLoadResult> poll();
    [[nodiscard]] bool busy() const { return busy_; }

  private:
    const assets::ResourceProvider* provider_ = nullptr;
    std::future<LanguageLoadResult> pending_;
    bool busy_ = false;
};

// 带命名空间的标识符所对应的 Minecraft 翻译键
// 例如 ("block", "minecraft", "stone") 得到 "block.minecraft.stone"
// 后两段直接取自注册表条目的 core::Identifier
[[nodiscard]] std::string translationKey(std::string_view prefix, std::string_view space,
                                         std::string_view path);

} // namespace mc::ui
