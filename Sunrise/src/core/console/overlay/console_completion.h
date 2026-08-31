#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace sunrise::core::console::overlay {

/**
 * Matches one completion pass reports.
 *
 * A reader given more names than this has not narrowed anything, so the pass says it truncated
 * and lets them type another letter rather than printing a wall.
 */
inline constexpr std::size_t kCompletionCapacity = 32;

/**
 * What completing one prefix found.
 *
 * The matches borrow the names their modules declared as constants, not the registry's storage,
 * so they stay valid after the pass that found them.
 */
struct Completion {
    std::array<std::string_view, kCompletionCapacity> matches{};
    std::size_t count{};
    /** Set when more entries matched than this can carry. */
    bool truncated{};
    /**
     * The longest run every match begins with.
     *
     * Typing this is always safe, because no matching entry disagrees about it. It is what lets
     * one key take `mov` to `movement.` and stop exactly where the names diverge.
     */
    std::string_view shared{};
};

/**
 * Finds every entry name beginning with one prefix.
 * @param prefix Text typed so far. An empty prefix matches everything.
 * @return The matches and the run they share.
 */
[[nodiscard]] Completion complete(std::string_view prefix) noexcept;

/**
 * Finds every entry name containing one run of text.
 *
 * Completion matches from the start because that is what a reader is building. A reader who has
 * already been refused is in the opposite position: they typed `help` and want `console.help`,
 * which no prefix match can reach. The shared run is not reported, since text found in the middle
 * of a name says nothing about what to type next.
 *
 * @param needle Text an entry name must contain. An empty needle matches everything.
 * @return The matches, with no shared run.
 */
[[nodiscard]] Completion suggest(std::string_view needle) noexcept;

} // namespace sunrise::core::console::overlay
