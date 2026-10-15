#include <string_view>

#include "../registry/ui_module_registry.h"
#include "../ui_module_descriptor.h"
#include "internal.h"
#include "mods.h"

namespace sunrise::core::ui::modules::mods {
namespace {

/** Namespaced stable ID keeps this page distinct from feature modules. */
constexpr std::string_view kStableId = "core.mods";
/** Menu label for the mod list. */
constexpr std::string_view kDisplayName = "Mods";

registry::PageRegistration g_page;

} // namespace

/** @return True when the Core Mods page owns its registry slot. */
bool initialize() noexcept {
    return g_page.acquire(Owner::core, kStableId, kDisplayName, &internal::draw);
}

/** Removes the Core Mods page. */
void shutdown() noexcept {
    g_page.release();
}

} // namespace sunrise::core::ui::modules::mods
