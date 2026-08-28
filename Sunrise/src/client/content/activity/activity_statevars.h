#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../inspection/activity_logic_catalog.h"

namespace sunrise::client::content::activity::statevars {

/** Typed StateVar component rows admitted from one 0x80809C0F owner. */
struct OwnerRow final {
    std::uint32_t configTag{};
};

/** Decodes the owner canonical/config descriptor rows without package I/O. */
[[nodiscard]] bool parse_owner_rows(std::span<const std::byte> ownerBlob,
                                    std::uint32_t ownerClass,
                                    std::vector<OwnerRow>& rows,
                                    std::vector<std::uint32_t>& canonicalConfigs,
                                    std::string& error);

/** Decodes one typed 0x80809C36 StateVar config blob. */
[[nodiscard]] bool parse_config(std::span<const std::byte> configBlob,
                                std::uint32_t configClass,
                                std::uint32_t configTag,
                                inspection::activity_logic_catalog::StateVar& stateVar,
                                std::string& error);

} // namespace sunrise::client::content::activity::statevars
