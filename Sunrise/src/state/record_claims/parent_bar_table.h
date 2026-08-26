#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::record_claims::parent_bar_table {

/**
 * The value bank index a lore book's parent-triumph bar reads, keyed by presentation node.
 *
 * Measured in game for most of these: a marker sweep authored distinct values across the bank and
 * each book displayed the one belonging to its own slot, naming it outright. That was necessary
 * because the expression at record field 136 names the wrong index for several books -- Ecdysis,
 * Trials and Tribulations, The Chronicon, For Every Rose -- and names only a flag for twelve more,
 * which is why those bars never moved however correct the count was.
 *
 * Keyed by node rather than by parent record: The Tangled Shore has no parent record at all, so
 * there is nothing to key it on, yet its bar is at a measured slot like any other.
 *
 * The allocation runs in content ship order -- the Year 1 books hold a contiguous run at 1931-1941,
 * Year 2 seasons follow in the 2200-2400s, Year 3 later still.
 *
 * Fourteen entries were removed from this table and then restored with a corrected reading, so
 * the history is worth stating. They came from record field 136, which names the category's
 * VISIBILITY GATE -- a value slot tested against zero -- and not, as was assumed, a bar. Because
 * apply_node_progress writes the claimed-chapter count into every entry here, a book with no
 * claimed chapters had its own gate written to zero on every account image and stayed redacted
 * forever. That is why those bars never moved however correct the count was.
 *
 * The correction is that ten of the fourteen are not mis-sourced at all: for those books one slot
 * does both jobs. The parent record's tracked objective reads the very slot that gates the
 * category, so the book reveals itself once a chapter is claimed and the same value drives the
 * bar. Dust settles it -- an in-game marker sweep measured its bar at 2342, which is precisely its
 * gate. Those ten are listed below as "gate is bar".
 *
 * The remaining four do name a distinct bar, recovered from the parent record's tracked objective
 * at byte offset +8 (a one-instruction READ_VALUE expression; the field has no named constant in
 * this codebase). That method reproduces nineteen of the twenty measured rows above exactly, which
 * is why these four are trusted enough to carry, but none of the four is itself measured yet --
 * they are marked "decoded" and should be confirmed in game before being relied on.
 *
 * Node 819, Letters from a Renegade, is the one row the +8 method fails: its shipped objective
 * tracks an unrelated record's value. Its entry below is measured and stands.
 *
 * An entry naming the node's own gate is therefore legitimate, not a defect. What keeps such a
 * book visible at zero chapters is ordering alone: nodes::apply_category_gates runs after this
 * pass and raises a zero gate back to one. That call must stay last -- see account_encoder.
 */
struct Bar {
    std::uint16_t nodeIndex;
    std::uint16_t valueIndex;
};

inline constexpr std::array<Bar, 34> kBars{{
    {838U, 2398U},  // Confessions (measured)
    {815U, 1932U},  // The Lawless Frontier — parent record is named "The Tangled Shore" (measured)
    {816U, 1933U},  // The Man They Call Cayde (measured)
    {817U, 1940U},  // Ghost Stories (measured)
    {818U, 1941U},  // Most Loyal (measured)
    {819U, 1939U},  // Letters from a Renegade (measured)
    {821U, 2273U},  // Dawning Delights (measured)
    {831U, 1931U},  // The Forsaken Prince (measured)
    {832U, 1936U},  // Truth to Power (measured)
    {833U, 1938U},  // A Drifter's Gambit (measured)
    {836U, 2347U},  // For Every Rose, a Thorn (measured)
    {837U, 2399U},  // The Chronicon (measured)
    {842U, 2585U},  // Trials and Tribulations (measured)
    {843U, 2663U},  // The Singular Exegete (measured)
    {845U, 1934U},  // The Dreaming City (measured)
    {846U, 1935U},  // Marasenna (measured)
    {847U, 1937U},  // The Awoken of the Reef (measured)
    {848U, 2267U},  // The Black Armory Papers (measured)
    {849U, 2348U},  // Ecdysis (measured)
    {851U, 2397U},  // Nothing Ends (measured)
    {822U, 2342U},  // Dust — gate is bar (measured 2342, equals its gate)
    {823U, 2344U},  // Stolen Intelligence — gate is bar
    {825U, 2520U},  // Luna's Lost — gate is bar
    {826U, 2521U},  // Letters from Eris — gate is bar
    {839U, 2514U},  // Unveiling — gate is bar
    {840U, 2517U},  // Last Days on Kraken Mare — gate is bar
    {841U, 2519U},  // Inquisition of the Damned — gate is bar
    {850U, 2341U},  // A Man with No Name — gate is bar
    {852U, 2516U},  // Aspect — gate is bar
    {853U, 2518U},  // Revelation — gate is bar
    {824U, 2349U},  // The Warlock Aunor — decoded, unconfirmed (gate 2346)
    {828U, 2575U},  // Constellations — decoded, unconfirmed (gate 2574)
    {829U, 2665U},  // Duress and Egress — decoded, unconfirmed (gate 2664)
    {854U, 2583U},  // The Liar — decoded, unconfirmed (gate 2584)
}};

} // namespace sunrise::state::record_claims::parent_bar_table
