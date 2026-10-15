#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::client::content::scenarios {

/**
 * Extracts every destination's bubble layout from the installed packages, once.
 * Each destination costs one tag read, because activity message 1 reads only the scenario blob.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the item build.
 * @return True when State already holds the domain or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

/** Destination layouts one caller owns. They carry no roster groups and are never published. */
class LayoutSet final {
public:
    LayoutSet() noexcept = default;
    ~LayoutSet() = default;

    LayoutSet(const LayoutSet&) = delete;
    LayoutSet& operator=(const LayoutSet&) = delete;
    LayoutSet(LayoutSet&&) = delete;
    LayoutSet& operator=(LayoutSet&&) = delete;

    /**
     * Collects every row and reads each bubble layout once, in one call.
     * The block keys must already be known, because a failed read is not retried.
     * @param source Package directory, block keys and read mode.
     * @param scratch Reader storage for the layout reads.
     * @return True when at least one row read.
     */
    [[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                             middleware::content::packages::reader::Scratch& scratch) noexcept;

    /**
     * Finds one destination's layout by package name.
     * @param name Destination name.
     * @param definition Receives the matching row.
     * @return True when the set holds that exact name.
     */
    [[nodiscard]] bool find(std::string_view name,
                            state::build_data::scenarios::Definition& definition) const noexcept;

    /** @return The rows the last build kept, valid until the next build. */
    [[nodiscard]] std::span<const state::build_data::scenarios::Definition> rows() const noexcept;

private:
    std::vector<state::build_data::scenarios::Definition> rows_{};
};

/**
 * Finds one destination's layout in a caller's set, or in State when the set is null.
 * @param set Caller-owned set, or null for the published layouts.
 * @param name Destination name.
 * @param definition Receives the matching row.
 * @return True when the chosen table holds that exact name.
 */
[[nodiscard]] bool find_layout(const LayoutSet* set,
                               std::string_view name,
                               state::build_data::scenarios::Definition& definition) noexcept;

} // namespace sunrise::client::content::scenarios
