#include "input/InputSystem.hpp"

#include "input/BindingConfig.hpp"

// Anchors the InputSystem translation unit in mc_rebedrock_runtime so the whole
// Vulkan-free input core is compiled under the runtime's -Wconversion flags and
// available to non-GUI consumers (headless tests, a future dedicated tool). The
// core is otherwise header-only; the static assertions below fail the build if
// the enum/table invariants the hot path relies on are ever broken.

namespace mc::input {
namespace {

constexpr BindingTable kDefaults = BindingTable::defaults();

// The nine hotbar actions are contiguous — the slot-number arithmetic in
// hotbarAction()/hotbarSlot() breaks silently otherwise.
static_assert(hotbarSlot(InputAction::Hotbar9) == 8);
static_assert(hotbarAction(0) == InputAction::Hotbar1);
static_assert(isHotbarAction(InputAction::Hotbar5));
static_assert(!isHotbarAction(InputAction::Jump));

// The default table binds jump to Space and sneak to LeftShift; this catches the
// classic index-misalignment sabotage at compile time as well as in the tests.
static_assert(kDefaults.binding(InputAction::Jump) == keyboard(Key::Space));
static_assert(kDefaults.binding(InputAction::Sneak) == keyboard(Key::LeftShift));
static_assert(kDefaults.binding(InputAction::MoveForward) == keyboard(Key::W));

}  // namespace

// A non-inline symbol so the object file is never discarded as empty.
const BindingTable& defaultBindings() noexcept {
    return kDefaults;
}

}  // namespace mc::input
