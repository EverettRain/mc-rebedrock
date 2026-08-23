#include "assets/PackManager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mc::assets {
namespace {

[[noreturn]] void packManagerAbort(const char* message) {
    std::fputs("pack manager fatal: ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

void PackManager::registerPack(std::string id, const ResourceProvider& provider,
                               PackMetadata metadata, bool hasDataHalf, bool hasResourceHalf) {
    const auto existing =
        std::ranges::find(packs_, id, &PackRecord::id);
    PackRecord record{std::move(id), &provider, std::move(metadata), hasDataHalf, hasResourceHalf};
    if (existing != packs_.end()) {
        *existing = std::move(record);
        return;
    }
    packs_.push_back(std::move(record));
}

const PackRecord* PackManager::find(const std::string& id) const {
    const auto it = std::ranges::find(packs_, id, &PackRecord::id);
    return it == packs_.end() ? nullptr : &*it;
}

std::vector<std::string>& PackManager::mutableOrder(PackStackKind stack) {
    return stack == PackStackKind::Data ? dataOrder_ : resourceOrder_;
}

const std::vector<std::string>& PackManager::order(PackStackKind stack) const {
    return stack == PackStackKind::Data ? dataOrder_ : resourceOrder_;
}

void PackManager::enable(PackStackKind stack, const std::string& id) {
    const PackRecord* record = find(id);
    if (record == nullptr) {
        packManagerAbort("enable() names a pack id that was never registered");
    }
    const bool hasHalf = stack == PackStackKind::Data ? record->hasDataHalf : record->hasResourceHalf;
    if (!hasHalf) {
        packManagerAbort(
            "enable() would put a pack with no half for this stack onto it "
            "(a pure resource pack has no data/ to join the data stack, and "
            "vice versa)");
    }
    auto& list = mutableOrder(stack);
    if (std::ranges::find(list, id) != list.end()) {
        return; // already enabled; leave its current position
    }
    list.push_back(id);
}

void PackManager::disable(PackStackKind stack, const std::string& id) {
    auto& list = mutableOrder(stack);
    std::erase(list, id);
}

void PackManager::promoteToTop(PackStackKind stack, const std::string& id) {
    auto& list = mutableOrder(stack);
    const auto it = std::ranges::find(list, id);
    if (it == list.end()) {
        return;
    }
    list.erase(it);
    list.push_back(id);
}

LayeredResourceProvider PackManager::buildProvider(PackStackKind stack,
                                                   const ResourceProvider& base) const {
    const auto& list = order(stack);
    // LayeredResourceProvider wants highest priority first; order() is stored
    // bottom-to-top (lowest priority first), so this is the one place that
    // gets reversed — see the header comment on buildProvider().
    std::vector<const ResourceProvider*> overlays;
    overlays.reserve(list.size());
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const PackRecord* record = find(*it);
        if (record == nullptr) {
            // A pack was disabled/unregistered out from under an order list
            // without going through disable() (should not happen through the
            // public API, but do not resolve a stale id to a stale pointer).
            continue;
        }
        overlays.push_back(record->provider);
    }
    return LayeredResourceProvider{base, std::move(overlays)};
}

PackCompatibility PackManager::checkCompatibility(const PackMetadata& metadata, PackStackKind stack,
                                                  const core::PackVersion& buildVersion) {
    const std::uint32_t target =
        stack == PackStackKind::Data ? buildVersion.data : buildVersion.resource;
    PackCompatibility result;
    result.packFormatMin = metadata.minFormat;
    result.packFormatMax = metadata.maxFormat;
    result.buildPackVersion = target;
    // A pack with no pack.mcmeta pack block at all parses to {0, 0}
    // (PackMetadata's documented zeroed-format case); treat that as "declares
    // no opinion" rather than flagging every legacy/loose directory as
    // incompatible.
    if (metadata.minFormat == 0 && metadata.maxFormat == 0) {
        result.compatible = true;
        return result;
    }
    const auto formatValue = static_cast<int>(target);
    result.compatible = metadata.supportsFormat(formatValue);
    return result;
}

} // namespace mc::assets
