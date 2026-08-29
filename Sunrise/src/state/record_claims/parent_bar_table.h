#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::record_claims::parent_bar_table {

/**
 * The value bank index a lore book's parent-triumph bar reads, keyed by presentation node.
 *
 * Most indices were measured directly with distinct in-game marker values. The four entries marked
 * "decoded, unconfirmed" come from the parent objective's READ_VALUE expression; that extraction
 * reproduces nineteen of the twenty measured indices, but those four still need an in-game check.
 *
 * The table is keyed by node because The Lawless Frontier has no parent record. Ten books genuinely
 * use one slot for both their NOT-ZERO category gate and parent bar. The account encoder therefore
 * applies category gates after progress: an empty shared slot is raised without overwriting a
 * non-zero count. Four of those books grant chapters by incrementing that shared counter directly;
 * the remaining books publish their claimed-chapter count there.
 */
struct Bar {
    std::uint16_t nodeIndex;
    std::uint16_t valueIndex;
};

inline constexpr std::array<Bar, 35> kBars{{
    {838U, 2398U},  // Confessions (measured)
    {815U, 1932U},  // The Lawless Frontier — parent record is named "The Tangled Shore" (measured)
    {816U, 1933U},  // The Man They Call Cayde (measured)
    {817U, 1940U},  // Ghost Stories (measured)
    {818U, 1941U},  // Most Loyal (measured)
    {819U, 2266U},  // Letters from a Renegade (decoded)
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
    {824U, 2349U},  // The Warlock Aunor — decoded, unconfirmed (gate 2346)
    {828U, 2575U},  // Constellations — decoded, unconfirmed (gate 2574)
    {829U, 2665U},  // Duress and Egress — decoded, unconfirmed (gate 2664)
    {854U, 2583U},  // The Liar — decoded, unconfirmed (gate 2584)
    {835U, 2265U},  // The Book of Unmaking (measured)
    {850U, 2341U},  // A Man with No Name — gate and parent bar are one slot; see below
    {822U, 2342U},  // Dust — gate and parent bar are one slot; see below
    {823U, 2344U},  // Stolen Intelligence — gate and parent bar are one slot; see below
    {839U, 2514U},  // Unveiling — gate and parent bar are one slot; see below
    {852U, 2516U},  // Aspect — gate and parent bar are one slot; see below
    {840U, 2517U},  // Last Days on Kraken Mare — gate and parent bar are one slot; see below
    {853U, 2518U},  // Revelation — gate and parent bar are one slot; see below
    {841U, 2519U},  // Inquisition of the Damned — gate and parent bar are one slot; see below
    {825U, 2520U},  // Luna's Lost — gate and parent bar are one slot; see below
    {826U, 2521U},  // Letters from Eris — gate and parent bar are one slot; see below
}};

} // namespace sunrise::state::record_claims::parent_bar_table
