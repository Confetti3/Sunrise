#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::record_claims::parent_bar_table {

/**
 * The value bank index a lore book's parent-triumph bar reads.
 *
 * Taken from the record row's own expression at field 136, read out of the shipped tables and
 * logged from the live build. Eighteen books name a value there; the other twelve name a flag
 * instead and are absent from this table -- their bars are driven by something else entirely.
 *
 * This replaces writing to the node's valueIndex, which resolves for only some books and left
 * Ecdysis, Trials and Tribulations and The Singular Exegete static despite correct counts.
 */
struct Bar {
    std::uint16_t recordRow;
    std::uint16_t valueIndex;
};

inline constexpr std::array<Bar, 18> kBars{{
    {1588U, 2344U},  // node 823 — Stolen Intelligence
    {1608U, 2346U},  // node 824 — The Warlock Aunor
    {1840U, 2520U},  // node 825 — Luna's Lost
    {1851U, 2521U},  // node 826 — Letters from Eris
    {2003U, 2574U},  // node 828 — Constellations
    {2209U, 2664U},  // node 829 — Duress and Egress
    {1412U, 4619U},  // node 835 — The Book of Unmaking
    {1578U, 2343U},  // node 836 — For Every Rose, a Thorn
    {1797U, 2514U},  // node 839 — Unveiling
    {1809U, 2517U},  // node 840 — Last Days on Kraken Mare
    {1819U, 2519U},  // node 841 — Inquisition of the Damned
    {2075U, 2586U},  // node 842 — Trials and Tribulations
    {2194U, 2662U},  // node 843 — The Singular Exegete
    {1598U, 2345U},  // node 849 — Ecdysis
    {1558U, 2341U},  // node 850 — A Man with No Name
    {1866U, 2516U},  // node 852 — Aspect
    {1876U, 2518U},  // node 853 — Revelation
    {2085U, 2584U},  // node 854 — The Liar
}};

} // namespace sunrise::state::record_claims::parent_bar_table
