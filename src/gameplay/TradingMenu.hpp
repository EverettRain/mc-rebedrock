#pragma once

// AR-M6: the merchant menu — the whole of "a player is trading with a villager",
// with no drawing in it.
//
// This is the backend a trade screen plugs into. It exists as its own type, and
// not as three loose fields on the session, for the same reason EnchantingMenu
// does: the screen is one player's, it lives on the ServerPlayer, and it is
// stowed back into the inventory when the screen closes. The interface a UI
// consumes is documented in
// `docs/content-dev/AR-content-realization/AR-M6-trading-backend-interface.md`.
//
// The shape is 26.1's MerchantMenu: two payment slots, one result slot, and a
// selected offer. Nothing here knows where any of that is drawn.

#include "gameplay/Inventory.hpp"
#include "gameplay/entities/Villager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

// The "nothing picked" selection. Deliberately a value a UI can send: closing
// the selection is a legal action, not an error.
inline constexpr std::size_t kNoTradeSelected = static_cast<std::size_t>(-1);

// MerchantMenu's three slots, by their vanilla indices.
inline constexpr std::size_t kTradePaymentSlotA = 0U;
inline constexpr std::size_t kTradePaymentSlotB = 1U;
inline constexpr std::size_t kTradeResultSlot = 2U;

// One offer as the screen needs to see it: what it costs, what it gives, and
// whether it can be taken right now. Derived from the villager's own table plus
// its level and per-offer use count, so a UI never has to know either.
//
// `wantsB` is always empty in this build (no offer in the farmer's table has a
// second cost), but it is part of the view because vanilla's MerchantOffer has
// costB and the screen draws two payment slots regardless — a UI written
// against this shape needs no change when a two-cost offer lands.
struct TradeOfferView final {
    ItemStack wantsA{};
    ItemStack wantsB{};
    ItemStack gives{};
    // The villager level that unlocks it (1..5).
    std::uint8_t level = 1U;
    // How many times it has been taken, and its ceiling.
    std::uint8_t uses = 0U;
    std::uint8_t maxUses = 0U;
    // Unlocked by the villager's current level. A locked offer is still listed —
    // vanilla shows it greyed out rather than hiding it — so the flag is part of
    // the view rather than a filter on it.
    bool unlocked = false;
    // Out of stock: uses have reached maxUses. Drawn with the "out of stock"
    // sprite in vanilla.
    bool outOfStock = false;

    [[nodiscard]] bool empty() const { return gives.empty(); }
    // Whether a click on this row could produce anything at all.
    [[nodiscard]] bool selectable() const { return !empty() && unlocked && !outOfStock; }
};

// The menu one player has open with one villager. Values, not pointers: the
// villager can die, be unloaded or wander off while the screen is open, and the
// menu must survive that long enough to give the payment back.
struct TradingMenu final {
    // The villager this screen belongs to; 0 when no screen is open. Everything
    // authoritative (level, experience, use counts) stays on the entity — this
    // is a handle, never a copy of the villager's state.
    std::uint64_t entityId = 0U;
    // Which row the player picked. Out of range means "none picked", which is
    // the state a freshly opened screen is in.
    std::size_t selectedOffer = kNoTradeSelected;
    // The two payment slots and the result. The result is DERIVED — it is
    // recomputed from (selected offer, payments) and never written by a click.
    ItemStack paymentA{};
    ItemStack paymentB{};
    ItemStack result{};

    // The offers as the screen sees them, refreshed every tick the screen is
    // open so a level-up or a use-count change shows immediately.
    std::array<TradeOfferView, entities::kMaxVillagerOffers> offers{};
    std::uint8_t offerCount = 0U;
    // The villager's own progress, mirrored for the level bar. `xpInLevel` and
    // `xpForNextLevel` are the bar's numerator and denominator; the denominator
    // is 0 at the ceiling, which is how a UI knows to draw no bar at all.
    std::uint8_t villagerLevel = 1U;
    int xpInLevel = 0;
    int xpForNextLevel = 0;

    [[nodiscard]] bool open() const { return entityId != 0U; }
    [[nodiscard]] bool hasSelection() const { return selectedOffer < offerCount; }
};

// --- the rules -------------------------------------------------------------

// Whether `payment` covers `cost`. An empty cost is covered by anything
// (including nothing), which is what makes a one-cost offer work with two
// payment slots.
[[nodiscard]] inline bool tradePaymentCovers(const ItemStack& payment, const ItemStack& cost) {
    if (cost.empty()) {
        return true;
    }
    return sameItem(payment, cost) && payment.count >= cost.count;
}

// MerchantMenu#tryMoveItems' result rule: the selected offer's goods appear in
// the result slot exactly when both costs are covered, and vanish otherwise.
// Order-insensitive on the two payment slots, matching vanilla's
// `isRequiredItem` check against either slot.
[[nodiscard]] inline ItemStack tradeResultFor(const TradeOfferView& offer,
                                              const ItemStack& paymentA,
                                              const ItemStack& paymentB) {
    if (!offer.selectable()) {
        return {};
    }
    const bool straight = tradePaymentCovers(paymentA, offer.wantsA) &&
                          tradePaymentCovers(paymentB, offer.wantsB);
    const bool swapped = tradePaymentCovers(paymentB, offer.wantsA) &&
                         tradePaymentCovers(paymentA, offer.wantsB);
    if (!straight && !swapped) {
        return {};
    }
    return offer.gives;
}

// Spends one offer's costs out of the two payment slots. Returns false and
// changes nothing when they do not cover it, so a caller can call this as the
// single "may I?" plus "do it" step the result slot's take needs.
[[nodiscard]] inline bool tradeSpendPayment(const TradeOfferView& offer, ItemStack& paymentA,
                                            ItemStack& paymentB) {
    const auto spend = [](ItemStack& slot, const ItemStack& cost) {
        if (cost.empty()) {
            return;
        }
        slot.count = static_cast<std::uint8_t>(slot.count - cost.count);
        if (slot.count == 0U) {
            slot = ItemStack{};
        }
    };
    if (tradePaymentCovers(paymentA, offer.wantsA) && tradePaymentCovers(paymentB, offer.wantsB)) {
        spend(paymentA, offer.wantsA);
        spend(paymentB, offer.wantsB);
        return true;
    }
    if (tradePaymentCovers(paymentB, offer.wantsA) && tradePaymentCovers(paymentA, offer.wantsB)) {
        spend(paymentB, offer.wantsA);
        spend(paymentA, offer.wantsB);
        return true;
    }
    return false;
}

} // namespace mc::gameplay
