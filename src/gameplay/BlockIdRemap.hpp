#pragma once

// Content identity remap for a cross-process connection (R0-4).
//
// Block identity crosses the wire as a dense BlockId — two bytes and one array
// subscript on decode — the way 26.1 sends block/state ids against a shared
// palette, not as a ~20-byte identifier string per field. Sending the id is only
// safe when both ends agree on which id names which block. Built-in (vanilla)
// content is version-locked and already agrees; external (mod/datapack) content
// may sit at a different id on each end because it registered in a different
// order. So the handshake ships each end's block registry as a name list in id
// order (BlockRegistrySnapshot), and the receiver builds this remap: peer BlockId
// -> local BlockId, aligned by name (never by the raw id, which is the whole
// point — see [[REGULAR.md]] R0 iron rule 2). A peer block this build has no name
// for maps to an invalid id, which the codec treats as unknown content and skips,
// exactly as it already skips an unknown identifier.
//
// On loopback both ends share one registry, so the peer snapshot equals the local
// order and the remap is the identity. A default-constructed (empty) remap *is*
// the identity, and a null remap pointer means the same thing, so the decode hot
// path pays nothing when there is nothing to remap — which is every single-player
// session.

#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::gameplay {

// A block registry as it crosses the handshake: the identifier of each block in
// BlockId order, so entry i is the block that end calls BlockId i.
using BlockRegistrySnapshot = std::vector<std::string>;

// This end's block registry as a snapshot, for the handshake to send.
[[nodiscard]] inline BlockRegistrySnapshot localBlockRegistrySnapshot() {
    const auto& registry = world::blockRegistry();
    BlockRegistrySnapshot snapshot;
    snapshot.reserve(registry.size());
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto id = world::BlockId::of(static_cast<world::BlockId::Value>(index));
        snapshot.push_back(registry.identifier(id).toString());
    }
    return snapshot;
}

class BlockIdRemap final {
  public:
    // The identity remap: peer id == local id. What loopback, and a peer whose
    // registry matches this build's exactly, use.
    BlockIdRemap() = default;

    // Builds peer-id -> local-id from the peer's snapshot, aligned by name. A peer
    // name this build's registry does not know resolves to an invalid id.
    explicit BlockIdRemap(const BlockRegistrySnapshot& peer) {
        const auto& registry = world::blockRegistry();
        peerToLocal_.reserve(peer.size());
        identity_ = peer.size() == registry.size();
        for (std::size_t peerId = 0; peerId < peer.size(); ++peerId) {
            const world::BlockId local = registry.byName(peer[peerId]);
            peerToLocal_.push_back(local);
            // The moment one entry does not map to the same id it came in as, the
            // remap is no longer a no-op and the fast path is off.
            if (!local.valid() || local.index() != peerId) {
                identity_ = false;
            }
        }
    }

    // Maps a peer's BlockId to this end's. An empty table (identity) hands the id
    // straight back; a peer id past the table, or one whose name this build lacks,
    // returns an invalid id the caller skips.
    [[nodiscard]] world::BlockId toLocal(std::uint16_t peerId) const {
        if (peerToLocal_.empty()) {
            return world::BlockId::of(peerId);
        }
        if (peerId >= peerToLocal_.size()) {
            return world::BlockId::invalid();
        }
        return peerToLocal_[peerId];
    }

    // Whether the remap is a no-op — the two registries agree name-for-name and
    // id-for-id. Lets a caller skip carrying the remap at all.
    [[nodiscard]] bool isIdentity() const { return identity_; }

  private:
    std::vector<world::BlockId> peerToLocal_;
    bool identity_ = true;
};

}  // namespace mc::gameplay
