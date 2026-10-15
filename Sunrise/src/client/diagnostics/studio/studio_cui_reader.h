#pragma once

#include <cstdint>
#include <span>

#include "studio_cui_protocol.h"

namespace sunrise::client::diagnostics::studio {

// Every reader runs on the UI frame thread, inside the CUI observer, under the CUI lock.

/** Result of one screen or node lookup. */
enum class CuiLookup : std::uint8_t { found, absent, faulted };

/** Header of one screen walk; rows hold the first count screens. */
struct CuiScreens final {
    std::uint32_t count{}, flags{};
    std::uint32_t focus0{cui_wire::kNoFocus}, focus1{cui_wire::kNoFocus};
};

/** Header of one tree walk; total counts every node seen, which can exceed count. */
struct CuiTree final {
    std::uint32_t count{}, total{}, flags{};
    std::uint32_t hierarchyTag{}, screenTag{}, viewHash{};
};

/**
 * Sets the screen manager and its ready byte for later reads.
 * @return False when either address is null.
 */
[[nodiscard]] bool initialize_cui_reader(std::uintptr_t manager, std::uintptr_t ready) noexcept;
void clear_cui_reader() noexcept;
/** True once the game has built the screen manager. */
[[nodiscard]] bool cui_manager_ready() noexcept;

/**
 * Copies the open screens, layer 0 then layer 1, each in draw order.
 * @return False when a read faulted; output.flags then carries walkFaulted.
 */
[[nodiscard]] bool read_cui_screens(std::span<cui_wire::ScreenRow> rows,
                                    CuiScreens& output) noexcept;
/** Finds the ordinal-th open screen with this hierarchy tag, counted as read_cui_screens does. */
[[nodiscard]] CuiLookup
find_cui_screen(std::uint32_t hierarchy, std::uint32_t ordinal, std::uintptr_t& screen) noexcept;
/**
 * Copies one screen's tree in depth-first pre-order; row 0 is the screen.
 * @return False when a read faulted; output.flags then carries walkFaulted.
 */
[[nodiscard]] bool
walk_cui_tree(std::uintptr_t screen, std::span<cui_wire::WidgetRow> rows, CuiTree& output) noexcept;
/** Finds the node with this hierarchy tag and id in one screen's tree; nested trees reuse ids. */
[[nodiscard]] CuiLookup find_cui_node(std::uintptr_t screen,
                                      std::uint32_t hierarchy,
                                      std::uint16_t nodeId,
                                      std::uintptr_t& node) noexcept;
/**
 * Fills the record fields of one node; sequence, response, status and time are left alone.
 * @return False when a read faulted.
 */
[[nodiscard]] bool read_cui_detail(std::uintptr_t node, cui_wire::DetailSection& output) noexcept;

} // namespace sunrise::client::diagnostics::studio
