#include "core/VersionManifest.hpp"
#include "net/Handshake.hpp"

#include <cstdint>
#include <string_view>

namespace {

// Parse-free ISO 8601 shape check, mirroring the manifest's own compile-time
// guard but exercised at runtime against the actual baked value.
[[nodiscard]] bool looksLikeIso8601(std::string_view value) {
    if (value.size() < 10U) {
        return false;
    }
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    return isDigit(value[0]) && isDigit(value[1]) && isDigit(value[2]) && isDigit(value[3]) &&
           value[4] == '-' && isDigit(value[5]) && isDigit(value[6]) && value[7] == '-' &&
           isDigit(value[8]) && isDigit(value[9]);
}

}  // namespace

int main() {
    using mc::core::kVersion;

    // --- The manifest itself is a coherent identity. -------------------------
    if (kVersion.id.empty() || kVersion.name.empty() || kVersion.seriesId.empty()) {
        return 1;
    }
    if (kVersion.worldVersion == 0U || kVersion.protocolVersion == 0U) {
        return 2;
    }
    if (kVersion.packVersion.resource == 0U || kVersion.packVersion.data == 0U) {
        return 3;
    }
    // The current build's concrete numbers, so a silent drift trips the test.
    if (kVersion.worldVersion != 19U || kVersion.protocolVersion != 4U) {
        return 4;
    }
    if (kVersion.id != "26.1") {
        return 5;
    }

    // --- Single source: the wire protocol number has exactly one definition. --
    // net::kProtocolVersion is an alias of kVersion.protocolVersion; if a second
    // independent literal ever reappears in net/, they diverge and this catches it.
    if (mc::net::kProtocolVersion != kVersion.protocolVersion) {
        return 10;
    }

    // The handshake actually puts kVersion's number on the wire: encode a default
    // ClientHello (which defaults to kProtocolVersion) and confirm the decoded
    // protocol equals the manifest field — the consumer emits the single source.
    {
        const auto bytes = mc::net::encodeClientHello(mc::net::ClientHello{});
        const auto decoded = mc::net::decodeClientHello(bytes);
        if (!decoded.has_value()) {
            return 11;
        }
        if (decoded->protocolVersion != kVersion.protocolVersion) {
            return 12;
        }
    }

    // The ServerHello likewise carries the single source, not a private literal.
    if (mc::net::ServerHello{}.protocolVersion != kVersion.protocolVersion) {
        return 13;
    }

    // --- Build metadata: baked in, non-empty, and ISO 8601 shaped (or the
    //     documented "unknown" fallback outside a git checkout). ---------------
    if (kVersion.buildTime.empty() || kVersion.buildRef.empty()) {
        return 20;
    }
    if (kVersion.buildTime != std::string_view{"unknown"} && !looksLikeIso8601(kVersion.buildTime)) {
        return 21;
    }

    // --- Determinism: reading it twice yields the same value (it is rodata, not
    //     a wall-clock read that would differ between calls). -------------------
    if (mc::core::kVersion.buildTime != kVersion.buildTime ||
        mc::core::kVersion.buildRef != kVersion.buildRef) {
        return 22;
    }

    return 0;
}
