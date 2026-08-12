#include "assets/FontProviders.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mc::assets {
namespace {

[[nodiscard]] std::vector<char32_t> decodeUtf8(std::string_view text) {
    std::vector<char32_t> result;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1U;
        char32_t codepoint = lead;
        if (lead >= 0xF0U) {
            length = 4U;
            codepoint = lead & 0x07U;
        } else if (lead >= 0xE0U) {
            length = 3U;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xC0U) {
            length = 2U;
            codepoint = lead & 0x1FU;
        }
        if (index + length > text.size()) {
            result.push_back(0xFFFDU);
            break;
        }
        bool valid = lead < 0x80U || lead >= 0xC0U;
        for (std::size_t offset = 1U; offset < length && valid; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            valid = (continuation & 0xC0U) == 0x80U;
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        result.push_back(valid ? codepoint : 0xFFFDU);
        index += valid ? length : 1U;
    }
    return result;
}

[[nodiscard]] bool filterMatches(const core::Json& provider, bool uniform, bool japanese) {
    const core::Json& filter = provider["filter"];
    if (!filter.isObject()) {
        return true;
    }
    if (filter.contains("uniform") && filter["uniform"].asBool() != uniform) {
        return false;
    }
    if (filter.contains("jp") && filter["jp"].asBool() != japanese) {
        return false;
    }
    return true;
}

[[nodiscard]] core::Json readJson(const ResourceProvider& resources,
                                  const ResourceLocation& location) {
    const auto file = resources.locate(location);
    std::ifstream input{file, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Unable to open font definition " + location.toString());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return core::Json::parse(buffer.str());
}

void loadRecursive(const ResourceProvider& resources, const ResourceLocation& definition,
                   bool uniform, bool japanese, std::vector<std::string>& referenceChain,
                   std::vector<FontProviderDefinition>& result) {
    if (std::ranges::find(referenceChain, definition.toString()) != referenceChain.end()) {
        throw std::runtime_error("Cyclic font reference at " + definition.toString());
    }
    referenceChain.push_back(definition.toString());
    const core::Json root = readJson(resources, definition);
    const core::Json& providers = root["providers"];
    if (!providers.isArray()) {
        referenceChain.pop_back();
        return;
    }
    for (std::size_t index = 0; index < providers.size(); ++index) {
        const core::Json& provider = providers[index];
        if (!filterMatches(provider, uniform, japanese) || !provider["type"].isString()) {
            continue;
        }
        const std::string type = provider["type"].asString();
        if (type == "reference") {
            const auto reference = ResourceLocation::parse(provider["id"].asString());
            loadRecursive(resources,
                          ResourceLocation{reference.space, "font/" + reference.path + ".json"},
                          uniform, japanese, referenceChain, result);
            continue;
        }
        FontProviderDefinition parsed;
        if (type == "bitmap") {
            parsed.kind = FontProviderKind::Bitmap;
            const auto file = ResourceLocation::parse(provider["file"].asString());
            parsed.file = ResourceLocation{file.space, "textures/" + file.path};
            parsed.height = static_cast<int>(provider["height"].asNumber(8.0));
            parsed.ascent = static_cast<int>(provider["ascent"].asNumber(7.0));
            const core::Json& rows = provider["chars"];
            if (rows.isArray()) {
                for (std::size_t row = 0; row < rows.size(); ++row) {
                    parsed.chars.push_back(decodeUtf8(rows[row].asString()));
                }
            }
        } else if (type == "space") {
            parsed.kind = FontProviderKind::Space;
            const core::Json& advances = provider["advances"];
            if (advances.isObject()) {
                for (const auto& [characters, advance] : advances.asObject()) {
                    const auto codepoints = decodeUtf8(characters);
                    if (codepoints.size() == 1U) {
                        parsed.advances.emplace_back(codepoints.front(), advance.asFloat());
                    }
                }
            }
        } else if (type == "unihex") {
            parsed.kind = FontProviderKind::Unihex;
            parsed.file = ResourceLocation::parse(provider["hex_file"].asString());
            const core::Json& overrides = provider["size_overrides"];
            if (overrides.isArray()) {
                for (std::size_t overrideIndex = 0; overrideIndex < overrides.size();
                     ++overrideIndex) {
                    const auto from = decodeUtf8(overrides[overrideIndex]["from"].asString());
                    const auto to = decodeUtf8(overrides[overrideIndex]["to"].asString());
                    if (from.size() == 1U && to.size() == 1U) {
                        parsed.sizeOverrides.push_back(UnihexSizeOverride{
                            from.front(),
                            to.front(),
                            static_cast<int>(overrides[overrideIndex]["left"].asNumber()),
                            static_cast<int>(overrides[overrideIndex]["right"].asNumber()),
                        });
                    }
                }
            }
        } else {
            continue;
        }
        result.push_back(std::move(parsed));
    }
    referenceChain.pop_back();
}

} // namespace

std::vector<FontProviderDefinition> loadFontProviders(const ResourceProvider& resources,
                                                      std::string_view id, bool uniform,
                                                      bool japanese) {
    const auto parsedId = ResourceLocation::parse(id);
    std::vector<FontProviderDefinition> result;
    std::vector<std::string> chain;
    loadRecursive(resources, ResourceLocation{parsedId.space, "font/" + parsedId.path + ".json"},
                  uniform, japanese, chain, result);
    return result;
}

} // namespace mc::assets
