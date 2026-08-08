#include "gameplay/Inventory.hpp"
#include "gameplay/ContentRegistry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mc::gameplay {

std::span<const ItemStack> creativeCatalog() {
    return contentRegistry().allCatalog();
}

std::span<const ItemStack> creativeBlockCatalog() {
    return contentRegistry().blockCatalog();
}

std::span<const ItemStack> creativeItemCatalog() {
    return contentRegistry().itemCatalog();
}

std::span<const ItemStack> creativeCatalog(CreativeCategory category) {
    return contentRegistry().catalog(category);
}

// A freshly created world starts with an empty inventory, exactly like vanilla.
// Creative players pull stacks out of the catalog; survival players mine them.
Inventory::Inventory() = default;

const ItemStack& Inventory::slot(std::size_t index) const {
    if (index >= slots_.size()) {
        throw std::out_of_range("Inventory slot index is outside 0..35");
    }
    return slots_[index];
}

world::Block Inventory::selectedBlock() const {
    const auto& selected = slots_[selectedHotbarSlot_];
    // The block item's own block wins when present; a legacy block stack (null
    // item) falls back to its block field.
    if (const auto* blockItem = asBlockItem(selected.item)) {
        return blockItem->block();
    }
    return selected.item == nullptr ? selected.block : world::Block::Air;
}

void Inventory::selectHotbar(std::size_t index) {
    if (index >= kHotbarSize) {
        throw std::out_of_range("Hotbar slot index is outside 0..8");
    }
    selectedHotbarSlot_ = index;
}

void Inventory::scrollHotbar(int steps) {
    constexpr int size = static_cast<int>(kHotbarSize);
    const int current = static_cast<int>(selectedHotbarSlot_);
    const int wrapped = ((current + steps) % size + size) % size;
    selectedHotbarSlot_ = static_cast<std::size_t>(wrapped);
}

void Inventory::clickSlot(
    std::size_t index,
    InventoryMouseButton button,
    bool shiftHeld) {
    if (index >= kSlotCount) {
        return;
    }
    if (shiftHeld) {
        quickMove(index, false);
        return;
    }
    clickExternalSlot(slots_[index], button);
}

void Inventory::clickExternalSlot(
    ItemStack& clicked,
    InventoryMouseButton button) {
    if (button == InventoryMouseButton::Right) {
        if (cursorStack_.empty() && !clicked.empty()) {
            const auto pickup = static_cast<std::uint8_t>((clicked.count + 1U) / 2U);
            cursorStack_ = clicked;
            cursorStack_.count = pickup;
            clicked.count = static_cast<std::uint8_t>(clicked.count - pickup);
            if (clicked.count == 0) {
                clicked = {};
            }
        } else if (!cursorStack_.empty() &&
                   (clicked.empty() || sameItem(clicked, cursorStack_))) {
            const auto maximum = itemMaximumStackSize(cursorStack_);
            if (clicked.empty()) {
                clicked = cursorStack_;
                clicked.count = 1U;
            } else if (clicked.count < maximum) {
                ++clicked.count;
            } else {
                return;
            }
            --cursorStack_.count;
            if (cursorStack_.count == 0) {
                cursorStack_ = {};
            }
        }
        return;
    }

    if (!cursorStack_.empty() && sameItem(clicked, cursorStack_)) {
        const auto maximum = itemMaximumStackSize(clicked);
        const auto available = static_cast<std::uint8_t>(maximum - clicked.count);
        const auto transferred = std::min(available, cursorStack_.count);
        clicked.count = static_cast<std::uint8_t>(clicked.count + transferred);
        cursorStack_.count = static_cast<std::uint8_t>(cursorStack_.count - transferred);
        if (cursorStack_.count == 0) {
            cursorStack_ = {};
        }
        return;
    }
    std::swap(cursorStack_, clicked);
}

void Inventory::quickMoveInto(ItemStack& source) {
    if (source.empty()) {
        return;
    }
    const auto maximum = itemMaximumStackSize(source);
    // QUICK_MOVE fills the main grid before the hotbar, merging into existing
    // stacks first and empty slots second, and leaves whatever does not fit in
    // `source`.
    const auto merge = [&](ItemStack& target) {
        if (source.empty()) {
            return;
        }
        if (!target.empty()) {
            if (!sameItem(target, source) || target.count >= maximum) {
                return;
            }
            const auto moved = std::min(
                source.count, static_cast<std::uint8_t>(maximum - target.count));
            target.count = static_cast<std::uint8_t>(target.count + moved);
            source.count = static_cast<std::uint8_t>(source.count - moved);
            if (source.count == 0U) {
                source = {};
            }
            return;
        }
        target = source;
        source = {};
    };
    for (std::size_t index = kHotbarSize; index < kSlotCount && !source.empty(); ++index) {
        merge(slots_[index]);
    }
    for (std::size_t index = 0; index < kHotbarSize && !source.empty(); ++index) {
        merge(slots_[index]);
    }
}

ItemStack& Inventory::mutableSlot(std::size_t index) {
    if (index >= slots_.size()) {
        throw std::out_of_range("Inventory slot index is outside 0..35");
    }
    return slots_[index];
}

void Inventory::dragDistribute(
    std::span<ItemStack*> targets,
    InventoryMouseButton button) {
    if (cursorStack_.empty() || targets.empty()) {
        return;
    }
    // QUICK_CRAFT's canInsert check: a target takes the dragged item when it is
    // empty or already holds the same stack below its limit.
    const auto accepts = [&](const ItemStack* target) {
        return target->empty() ||
            (sameItem(*target, cursorStack_) &&
             target->count < itemMaximumStackSize(*target));
    };
    const auto maximum = itemMaximumStackSize(cursorStack_);
    if (button == InventoryMouseButton::Right) {
        // quickCraftStage 1: a single item per slot, like repeated right-clicks.
        for (ItemStack* target : targets) {
            if (cursorStack_.empty()) break;
            if (!accepts(target)) continue;
            if (target->empty()) {
                *target = cursorStack_;
                target->count = 1U;
            } else {
                ++target->count;
            }
            --cursorStack_.count;
            if (cursorStack_.count == 0U) {
                cursorStack_ = {};
            }
        }
        return;
    }
    // quickCraftStage 0: share the cursor stack as evenly as possible across
    // the accepting slots, with the remainder going to the first slots.
    std::size_t fillable = 0;
    for (const ItemStack* target : targets) {
        if (accepts(target)) ++fillable;
    }
    if (fillable == 0U) {
        return;
    }
    std::uint8_t perSlot = static_cast<std::uint8_t>(cursorStack_.count / fillable);
    std::uint8_t extra = static_cast<std::uint8_t>(cursorStack_.count % fillable);
    for (ItemStack* target : targets) {
        if (cursorStack_.empty()) break;
        if (!accepts(target)) continue;
        std::uint8_t amount = perSlot;
        if (extra > 0U) {
            ++amount;
            --extra;
        }
        amount = std::min(amount, static_cast<std::uint8_t>(maximum - target->count));
        if (target->empty()) {
            *target = cursorStack_;
            target->count = amount;
        } else {
            target->count = static_cast<std::uint8_t>(target->count + amount);
        }
        cursorStack_.count = static_cast<std::uint8_t>(cursorStack_.count - amount);
        if (cursorStack_.count == 0U) {
            cursorStack_ = {};
        }
    }
}

void Inventory::gatherAllIntoCursor(std::span<ItemStack*> sources) {
    if (cursorStack_.empty()) {
        return;
    }
    const auto maximum = itemMaximumStackSize(cursorStack_);
    for (ItemStack* source : sources) {
        if (source == nullptr || source->empty() || !sameItem(*source, cursorStack_)) {
            continue;
        }
        if (cursorStack_.count >= maximum) {
            break;
        }
        const auto take = std::min(
            static_cast<std::uint8_t>(maximum - cursorStack_.count), source->count);
        source->count = static_cast<std::uint8_t>(source->count - take);
        if (source->count == 0U) {
            *source = {};
        }
        cursorStack_.count = static_cast<std::uint8_t>(cursorStack_.count + take);
    }
}

bool Inventory::mergeIntoCursor(ItemStack stack) {
    if (stack.empty()) {
        return false;
    }
    if (cursorStack_.empty()) {
        cursorStack_ = stack;
        return true;
    }
    if (!sameItem(cursorStack_, stack)) {
        return false;
    }
    const auto maximum = itemMaximumStackSize(cursorStack_);
    if (static_cast<unsigned int>(cursorStack_.count) + stack.count > maximum) {
        return false;
    }
    cursorStack_.count = static_cast<std::uint8_t>(cursorStack_.count + stack.count);
    return true;
}

void Inventory::clickCreativeItem(
    std::size_t catalogIndex,
    InventoryMouseButton button,
    bool shiftHeld) {
    const auto catalog = creativeCatalog();
    if (catalogIndex >= catalog.size()) {
        return;
    }
    clickCreativeItem(catalog[catalogIndex], button, shiftHeld);
}

void Inventory::clickCreativeItem(
    ItemStack catalogStack,
    InventoryMouseButton button,
    bool shiftHeld) {
    if (catalogStack.empty()) {
        return;
    }
    if (cursorStack_.empty()) {
        cursorStack_ = catalogStack;
        if (shiftHeld) {
            cursorStack_.count = itemMaximumStackSize(cursorStack_);
        }
        return;
    }
    if (sameItem(cursorStack_, catalogStack)) {
        if (button == InventoryMouseButton::Left) {
            cursorStack_.count = shiftHeld
                ? itemMaximumStackSize(cursorStack_)
                : std::min<std::uint8_t>(
                      itemMaximumStackSize(cursorStack_),
                      static_cast<std::uint8_t>(cursorStack_.count + 1U));
        } else if (cursorStack_.count > 1U) {
            --cursorStack_.count;
        } else {
            cursorStack_ = {};
        }
        return;
    }
    if (button == InventoryMouseButton::Left) {
        cursorStack_ = {};
    } else if (cursorStack_.count > 1U) {
        --cursorStack_.count;
    } else {
        cursorStack_ = {};
    }
}

ItemStack Inventory::takeCursorStack(bool wholeStack) {
    if (cursorStack_.empty()) {
        return {};
    }
    if (wholeStack) {
        ItemStack result = cursorStack_;
        cursorStack_ = {};
        return result;
    }
    ItemStack result = cursorStack_;
    result.count = 1U;
    --cursorStack_.count;
    if (cursorStack_.count == 0U) {
        cursorStack_ = {};
    }
    return result;
}

void Inventory::quickMove(std::size_t index, bool singleItem) {
    ItemStack& source = slots_[index];
    if (source.empty()) {
        return;
    }
    const std::size_t destinationBegin = index < kHotbarSize ? kHotbarSize : 0U;
    const std::size_t destinationEnd = index < kHotbarSize ? kSlotCount : kHotbarSize;
    std::uint8_t remaining = singleItem ? 1U : source.count;
    const auto maximum = itemMaximumStackSize(source);

    for (std::size_t destination = destinationBegin;
         destination < destinationEnd && remaining > 0;
         ++destination) {
        ItemStack& target = slots_[destination];
        if (!sameItem(target, source) || target.count >= maximum) {
            continue;
        }
        const auto moved = std::min(
            remaining, static_cast<std::uint8_t>(maximum - target.count));
        target.count = static_cast<std::uint8_t>(target.count + moved);
        remaining = static_cast<std::uint8_t>(remaining - moved);
    }
    for (std::size_t destination = destinationBegin;
         destination < destinationEnd && remaining > 0;
         ++destination) {
        ItemStack& target = slots_[destination];
        if (!target.empty()) {
            continue;
        }
        target = source;
        target.count = remaining;
        remaining = 0;
    }
    const std::uint8_t movedTotal = static_cast<std::uint8_t>(
        (singleItem ? 1U : source.count) - remaining);
    source.count = static_cast<std::uint8_t>(source.count - movedTotal);
    if (source.count == 0) {
        source = {};
    }
}

void Inventory::stowCursorStack() {
    if (cursorStack_.empty()) {
        return;
    }
    const auto maximum = itemMaximumStackSize(cursorStack_);
    for (auto& stack : slots_) {
        if (!sameItem(stack, cursorStack_) || stack.count >= maximum) {
            continue;
        }
        const auto moved = std::min(
            cursorStack_.count,
            static_cast<std::uint8_t>(maximum - stack.count));
        stack.count = static_cast<std::uint8_t>(stack.count + moved);
        cursorStack_.count = static_cast<std::uint8_t>(cursorStack_.count - moved);
        if (cursorStack_.count == 0) {
            cursorStack_ = {};
            return;
        }
    }
    for (auto& stack : slots_) {
        if (stack.empty()) {
            std::swap(stack, cursorStack_);
            return;
        }
    }
}

ItemStack Inventory::takeSelected(bool wholeStack) {
    ItemStack& selected = slots_[selectedHotbarSlot_];
    if (selected.empty()) {
        return {};
    }
    if (wholeStack) {
        ItemStack result = selected;
        selected = {};
        return result;
    }
    ItemStack result = selected;
    result.count = 1U;
    --selected.count;
    if (selected.count == 0) {
        selected = {};
    }
    return result;
}

bool Inventory::consumeSelected(std::uint8_t count) {
    ItemStack& selected = slots_[selectedHotbarSlot_];
    if (selected.empty() || selected.count < count) {
        return false;
    }
    selected.count = static_cast<std::uint8_t>(selected.count - count);
    if (selected.count == 0U) {
        selected = {};
    }
    return true;
}

bool Inventory::damageSelected(std::uint16_t amount) {
    ItemStack& selected = slots_[selectedHotbarSlot_];
    const std::uint16_t maximumDamage = itemMaximumDamage(selected);
    if (selected.empty() || maximumDamage == 0U || amount == 0U) {
        return false;
    }
    // Vanilla breaks the item once the damage passes the material's durability,
    // so the last point of durability is a usable swing.
    if (selected.damage + amount > maximumDamage) {
        selected = {};
        return true;
    }
    selected.damage = static_cast<std::uint16_t>(selected.damage + amount);
    return false;
}

bool Inventory::add(ItemStack& stack) {
    if (stack.empty()) {
        return true;
    }
    const auto maximum = itemMaximumStackSize(stack);
    for (auto& target : slots_) {
        if (!sameItem(target, stack) || target.count >= maximum) {
            continue;
        }
        const auto moved = std::min(
            stack.count, static_cast<std::uint8_t>(maximum - target.count));
        target.count = static_cast<std::uint8_t>(target.count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        if (stack.count == 0) {
            stack = {};
            return true;
        }
    }
    for (auto& target : slots_) {
        if (target.empty()) {
            target = stack;
            stack = {};
            return true;
        }
    }
    return false;
}

void Inventory::restore(
    const std::array<ItemStack, kSlotCount>& slots,
    std::size_t selectedHotbarSlot) {
    if (selectedHotbarSlot >= kHotbarSize) {
        throw std::out_of_range("Restored hotbar slot is outside 0..8");
    }
    slots_ = slots;
    selectedHotbarSlot_ = selectedHotbarSlot;
    cursorStack_ = {};
}

} // namespace mc::gameplay
