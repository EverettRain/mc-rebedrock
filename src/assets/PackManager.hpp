#pragma once

// The management layer over ResourceProvider: PACK-0's "two ordered stacks,
// split by end". Everything below composes the EXISTING providers
// (StandardPackResourceProvider / LayeredResourceProvider) — this file adds no
// new IO, only bookkeeping (which packs exist, what order they are enabled in,
// which end — data or resources — each stack serves).
//
// Vanilla's own two providers per end mirror Java's own split: a data pack
// carries `data/<ns>/…` (tags, recipes, loot — server authoritative), a
// resource pack carries `assets/<ns>/…` (textures, models, sounds — client
// render). A single on-disk pack may carry either half, both, or neither;
// StandardPackResourceProvider already resolves both halves through
// ResourceLocation::type (PackType::ServerData vs ClientResources), so one
// provider per pack root serves both stacks — PackManager just decides which
// stacks that pack participates in.
//
// Two-end hard split (PACK REGULAR #1): a `PackManager` builds a **data**
// LayeredResourceProvider (authoritative — the dedicated server runs this) and
// a **resource** LayeredResourceProvider (render-only — the dedicated server
// never touches this). Nothing here reaches into Vulkan, atlas baking, or
// audio; a caller is expected to feed the data provider to the data-driven
// gameplay tables (BlockTags, RecipeTable, LootTable, entity attributes, biome
// spawn tables) and the resource provider to the renderer.
//
// Built-in = base pack (PACK REGULAR #3): the bundled vanilla content is
// always stack position 0 (the `base` argument to LayeredResourceProvider);
// PackManager never lets a caller reorder it out from under user packs, so the
// stack is always "bottom vanilla -> top user pack", the last-added pack
// winning a lookup.

#include "assets/PackMetadata.hpp"
#include "assets/ResourceProvider.hpp"
#include "core/VersionManifest.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc::assets {

// The two ends a pack can serve. A pack that ships only `data/` (a pure data
// pack) or only `assets/` (an ordinary resource pack) still gets an entry here
// — PackManager does not require both halves to exist on disk, exactly as
// StandardPackResourceProvider tolerates a missing half (locate() on the
// absent half simply never finds a file, so the stack falls through).
enum class PackStackKind : std::uint8_t {
    Data,      // data/ — authoritative, server-side, per-save (PACK-1 territory)
    Resources, // assets/ — client render, global (PACK-3 territory)
};

// pack.mcmeta's format range compared against this build's pack_format
// (META's VersionManifest::packVersion). Mirrors vanilla's own posture:
// out-of-range is a flagged caution, not a silent skip or a hard failure —
// PACK REGULAR #9 "记账 + 拒/降级，不静默乱载" is satisfied by *recording* the
// mismatch (PackCompatibility::compatible == false) so a caller can choose to
// warn, refuse to enable, or load anyway; PackManager itself never hides it.
struct PackCompatibility final {
    int packFormatMin = 0;
    int packFormatMax = 0;
    std::uint32_t buildPackVersion = 0U; // the field of core::PackVersion this pack half targets
    bool compatible = true;
};

// One pack on disk: a root directory (or zip, once ZipResourcePackProvider is
// handed in as the pack's provider — PackManager owns providers by pointer, it
// does not care whether the concrete type is StandardPackResourceProvider or
// something else, keeping ZipResourcePackProvider's Vulkan-target-only
// dependency out of mc_rebedrock_runtime) plus its parsed pack.mcmeta.
struct PackRecord final {
    std::string id;                 // stable key: enable/disable/order reference this, not the path
    const ResourceProvider* provider = nullptr; // never owned here — see registerPack()
    PackMetadata metadata;
    bool hasDataHalf = false;      // probed at registration: does data/ exist for this pack?
    bool hasResourceHalf = false;  // probed at registration: does assets/ exist for this pack?
};

// Holds two independent ordered stacks (data, resources) over a shared set of
// registered packs, and builds the corresponding LayeredResourceProvider for
// either stack on demand. PackManager owns no ResourceProvider instances —
// callers (Application, tests) own the concrete providers (they already keep
// them alive in a std::deque the way Application.cpp does today) and register
// a non-owning pointer plus the pack's metadata. This mirrors
// LayeredResourceProvider's own non-owning-pointer contract, so PackManager
// adds no lifetime model of its own.
class PackManager final {
  public:
    // Registers a pack's provider + parsed metadata under `id`. `hasDataHalf`
    // / `hasResourceHalf` record which stacks this pack is eligible to join —
    // enabling a pack in a stack it has no half for is harmless (the provider
    // simply never resolves anything on that stack) but is rejected here so a
    // caller cannot accidentally believe a pure-resource pack contributes
    // data. Registering the same id twice replaces the record (last write
    // wins), matching how a directory rescan would refresh a pack's metadata.
    void registerPack(std::string id, const ResourceProvider& provider, PackMetadata metadata,
                      bool hasDataHalf, bool hasResourceHalf);

    [[nodiscard]] const std::vector<PackRecord>& packs() const { return packs_; }
    [[nodiscard]] const PackRecord* find(const std::string& id) const;

    // Enables `id` on `stack`, appended at the top (highest priority) of that
    // stack's current order. No-op if already enabled on that stack. Aborts
    // (registryAbort-style, via packManagerAbort) if `id` is unregistered or
    // has no half for `stack` — see the class comment.
    void enable(PackStackKind stack, const std::string& id);

    // Removes `id` from `stack`'s order, wherever it sits. No-op if not
    // enabled.
    void disable(PackStackKind stack, const std::string& id);

    // Moves `id` to the top (highest priority) of `stack`. No-op if `id` is
    // not enabled on `stack`.
    void promoteToTop(PackStackKind stack, const std::string& id);

    // The enabled ids for `stack`, bottom (lowest priority, applied first) to
    // top (highest priority, wins a lookup) — the built-in base is implicit
    // and not listed here (see buildProvider()).
    [[nodiscard]] const std::vector<std::string>& order(PackStackKind stack) const;

    // Builds the LayeredResourceProvider for `stack`: `base` (the bundled
    // vanilla content — always the stack floor) with every enabled pack's
    // provider overlaid in order()'s sequence, highest priority
    // (last-in-order) searched first. This is the one place order() gets
    // reversed into LayeredResourceProvider's highest-first overlay
    // convention, so every other consumer of PackManager can reason in the
    // simpler "bottom to top" direction.
    //
    // The returned LayeredResourceProvider borrows `base` and every enabled
    // pack's provider (all non-owning pointers, per LayeredResourceProvider's
    // own contract) — they, and this PackManager's registered records, must
    // outlive the returned value. LayeredResourceProvider copies the overlay
    // pointer vector into itself, so there is no extra storage for the caller
    // to keep alive beyond the providers themselves.
    [[nodiscard]] LayeredResourceProvider buildProvider(PackStackKind stack,
                                                         const ResourceProvider& base) const;

    // pack.mcmeta compat check for one pack against this build's pack_format.
    // `stack` selects which half of PackVersion (resource vs data) is the
    // comparison target, mirroring Java's split resource/data pack_format
    // numbers.
    [[nodiscard]] static PackCompatibility checkCompatibility(const PackMetadata& metadata,
                                                              PackStackKind stack,
                                                              const core::PackVersion& buildVersion);

  private:
    [[nodiscard]] std::vector<std::string>& mutableOrder(PackStackKind stack);

    std::vector<PackRecord> packs_;
    std::vector<std::string> dataOrder_;
    std::vector<std::string> resourceOrder_;
};

} // namespace mc::assets
