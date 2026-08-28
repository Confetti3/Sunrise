#pragma once

#include "definition.h"

namespace sunrise::state::activity::destination {

/** @return True when the package name length fits its fixed storage. */
[[nodiscard]] constexpr bool valid(const DestinationSelection& selection) noexcept {
    return selection.packageNameLength <= selection.packageName.size();
}

} // namespace sunrise::state::activity::destination
