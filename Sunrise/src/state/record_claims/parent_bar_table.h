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
    {822U, 2342U},  // Dust (measured)
    {823U, 2344U},  // Stolen Intelligence (field136)
    {824U, 2346U},  // The Warlock Aunor (field136)
    {825U, 2520U},  // Luna's Lost (field136)
    {826U, 2521U},  // Letters from Eris (field136)
    {828U, 2574U},  // Constellations (field136)
    {829U, 2664U},  // Duress and Egress (field136)
    {831U, 1931U},  // The Forsaken Prince (measured)
    {832U, 1936U},  // Truth to Power (measured)
    {833U, 1938U},  // A Drifter's Gambit (measured)
    {836U, 2347U},  // For Every Rose, a Thorn (measured)
    {837U, 2399U},  // The Chronicon (measured)
    {839U, 2514U},  // Unveiling (field136)
    {840U, 2517U},  // Last Days on Kraken Mare (field136)
    {841U, 2519U},  // Inquisition of the Damned (field136)
    {842U, 2585U},  // Trials and Tribulations (measured)
    {843U, 2663U},  // The Singular Exegete (measured)
    {845U, 1934U},  // The Dreaming City (measured)
    {846U, 1935U},  // Marasenna (measured)
    {847U, 1937U},  // The Awoken of the Reef (measured)
    {848U, 2267U},  // The Black Armory Papers (measured)
    {849U, 2348U},  // Ecdysis (measured)
    {850U, 2341U},  // A Man with No Name (field136)
    {851U, 2397U},  // Nothing Ends (measured)
    {852U, 2516U},  // Aspect (field136)
    {853U, 2518U},  // Revelation (field136)
    {854U, 2584U},  // The Liar (field136)
}};

} // namespace sunrise::state::record_claims::parent_bar_table
