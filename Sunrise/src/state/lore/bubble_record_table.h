// GENERATED FILE -- do not hand-edit.
//
// Produced by gen_bubble_table.py from bubble_record_map.json. To change an entry, fix
// the map data and regenerate:
//   python3 collectible-triumph-map/gen_bubble_table.py
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace sunrise::state::lore::bubble_record_table {

/**
 * bubble -> candidate record rows, joined from the 86657-era manifest.
 *
 * Each bucket is one bubble a lore-collectible pickup can report, holding the record rows
 * (build row indices into the record table, not definition hashes) that bubble's pickups can
 * complete. Where a bucket holds more than one row, a pickup grants the first the account
 * does not already hold -- see grant_from_bubble_table in lore_grant.h.
 *
 * 59 bubbles, generated from bubble_record_map.json's non-empty buckets. The FNV-1a
 * offset basis (0x811C9DC5) is not a real bubble and never appears here --
 * see kConfessionsNode in lore_grant.h for the instanced-activity case that used to be
 * aliased onto it.
 */

namespace detail {

// bubble 0x09711AEB, destination 308080871, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows0 = {819};
// bubble 0x09777230, destination 308080871, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows1 = {800};
// bubble 0x0E5486FF, destination 290444260, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows2 = {1834};
// bubble 0x109F7CF6, destination 2779202173, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows3 = {778, 40};
// bubble 0x110EFBDC, destination 290444260, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows4 = {1849, 1829};
// bubble 0x18E267F9, destination 290444260, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows5 = {1826, 1827};
// bubble 0x1CFBD6EB, destination 1199524104, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows6 = {748};
// bubble 0x257C904B, destination 359854275, 3 candidate row(s).
inline constexpr std::array<std::uint16_t, 3> kRows7 = {790, 803, 744};
// bubble 0x272010E9, destination 1199524104, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows8 = {806};
// bubble 0x29A640EF, destination 1199524104, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows9 = {749};
// bubble 0x29B8886A, destination 359854275, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows10 = {788, 755};
// bubble 0x2A2301B8, destination 2779202173, 3 candidate row(s).
inline constexpr std::array<std::uint16_t, 3> kRows11 = {785, 783, 40};
// bubble 0x2DCABF42, destination 2779202173, 4 candidate row(s).
inline constexpr std::array<std::uint16_t, 4> kRows12 = {787, 772, 765, 43};
// bubble 0x31256234, destination 359854275, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows13 = {824};
// bubble 0x31943967, destination 359854275, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows14 = {791};
// bubble 0x3453E759, destination 290444260, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows15 = {1828};
// bubble 0x37A08717, destination 126924919, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows16 = {813};
// bubble 0x3976EE58, destination 2388758973, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows17 = {811};
// bubble 0x3C18FBCE, destination 290444260, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows18 = {1831, 1842};
// bubble 0x3CB36B81, destination 1993421442, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows19 = {798, 815};
// bubble 0x41006024, destination 359854275, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows20 = {821};
// bubble 0x410C4F97, destination 2779202173, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows21 = {784, 42};
// bubble 0x447E4EB5, destination 290444260, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows22 = {1832, 1844};
// bubble 0x4AC36E96, destination 308080871, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows23 = {820, 750};
// bubble 0x4CF9B596, destination 2388758973, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows24 = {810};
// bubble 0x5A95C41A, destination 359854275, 5 candidate row(s).
inline constexpr std::array<std::uint16_t, 5> kRows25 = {786, 802, 742, 741, 740};
// bubble 0x5E856821, destination 2779202173, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows26 = {761, 41};
// bubble 0x64BFD67C, destination 359854275, 3 candidate row(s).
inline constexpr std::array<std::uint16_t, 3> kRows27 = {792, 805, 746};
// bubble 0x6D64AA42, destination 290444260, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows28 = {1833, 1846};
// bubble 0x6DDAC118, destination 2388758973, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows29 = {809};
// bubble 0x75A68449, destination 290444260, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows30 = {1830};
// bubble 0x773F859E, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows31 = {48};
// bubble 0x7B3F5526, destination 359854275, 4 candidate row(s).
inline constexpr std::array<std::uint16_t, 4> kRows32 = {793, 823, 757, 756};
// bubble 0x7EE2011A, destination 359854275, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows33 = {789, 804};
// bubble 0x83682E45, destination 290444260, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows34 = {1843};
// bubble 0x85B17A04, destination 2218917881, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows35 = {818};
// bubble 0x890FA3E0, destination 359854275, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows36 = {822, 754};
// bubble 0x9F2597D3, destination 126924919, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows37 = {814};
// bubble 0xA4ADFE84, destination 2779202173, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows38 = {780, 781};
// bubble 0xA7CF4C10, destination 2218917881, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows39 = {797};
// bubble 0xAA40DF1C, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows40 = {48};
// bubble 0xADE65E0A, destination 1199524104, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows41 = {808};
// bubble 0xB76C4F9A, destination 1199524104, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows42 = {794};
// bubble 0xB76C4F9B, destination 1199524104, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows43 = {807, 747};
// bubble 0xB9E2BD60, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows44 = {48};
// bubble 0xBEE4E974, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows45 = {48};
// bubble 0xC1FAAA4C, destination 126924919, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows46 = {812};
// bubble 0xC64457D2, destination 290444260, 3 candidate row(s).
inline constexpr std::array<std::uint16_t, 3> kRows47 = {1841, 1822, 1823};
// bubble 0xC65A2A44, destination 2388758973, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows48 = {795};
// bubble 0xD1CF7944, destination 359854275, 2 candidate row(s).
inline constexpr std::array<std::uint16_t, 2> kRows49 = {753, 752};
// bubble 0xD1EF1EF5, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows50 = {779};
// bubble 0xEF2B6D69, destination 2218917881, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows51 = {816};
// bubble 0xEF2B6D6A, destination 2218917881, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows52 = {817};
// bubble 0xEFEF8119, destination 290444260, 3 candidate row(s).
inline constexpr std::array<std::uint16_t, 3> kRows53 = {1847, 1824, 1825};
// bubble 0xF1E4371C, destination 2779202173, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows54 = {782};
// bubble 0xF25CEA8D, destination 290444260, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows55 = {1845};
// bubble 0xF68613C1, destination 126924919, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows56 = {796};
// bubble 0xFA122719, destination 290444260, 4 candidate row(s).
inline constexpr std::array<std::uint16_t, 4> kRows57 = {1850, 1822, 1820, 1821};
// bubble 0xFA9B272B, destination 308080871, 1 candidate row(s).
inline constexpr std::array<std::uint16_t, 1> kRows58 = {751};

/** One bubble's candidate record rows, in table order. */
struct Bucket {
    std::uint32_t bubbleHash;
    std::span<const std::uint16_t> rows;
};

inline constexpr std::array<Bucket, 59> kBuckets{{
    {0x09711AEBU, kRows0},
    {0x09777230U, kRows1},
    {0x0E5486FFU, kRows2},
    {0x109F7CF6U, kRows3},
    {0x110EFBDCU, kRows4},
    {0x18E267F9U, kRows5},
    {0x1CFBD6EBU, kRows6},
    {0x257C904BU, kRows7},
    {0x272010E9U, kRows8},
    {0x29A640EFU, kRows9},
    {0x29B8886AU, kRows10},
    {0x2A2301B8U, kRows11},
    {0x2DCABF42U, kRows12},
    {0x31256234U, kRows13},
    {0x31943967U, kRows14},
    {0x3453E759U, kRows15},
    {0x37A08717U, kRows16},
    {0x3976EE58U, kRows17},
    {0x3C18FBCEU, kRows18},
    {0x3CB36B81U, kRows19},
    {0x41006024U, kRows20},
    {0x410C4F97U, kRows21},
    {0x447E4EB5U, kRows22},
    {0x4AC36E96U, kRows23},
    {0x4CF9B596U, kRows24},
    {0x5A95C41AU, kRows25},
    {0x5E856821U, kRows26},
    {0x64BFD67CU, kRows27},
    {0x6D64AA42U, kRows28},
    {0x6DDAC118U, kRows29},
    {0x75A68449U, kRows30},
    {0x773F859EU, kRows31},
    {0x7B3F5526U, kRows32},
    {0x7EE2011AU, kRows33},
    {0x83682E45U, kRows34},
    {0x85B17A04U, kRows35},
    {0x890FA3E0U, kRows36},
    {0x9F2597D3U, kRows37},
    {0xA4ADFE84U, kRows38},
    {0xA7CF4C10U, kRows39},
    {0xAA40DF1CU, kRows40},
    {0xADE65E0AU, kRows41},
    {0xB76C4F9AU, kRows42},
    {0xB76C4F9BU, kRows43},
    {0xB9E2BD60U, kRows44},
    {0xBEE4E974U, kRows45},
    {0xC1FAAA4CU, kRows46},
    {0xC64457D2U, kRows47},
    {0xC65A2A44U, kRows48},
    {0xD1CF7944U, kRows49},
    {0xD1EF1EF5U, kRows50},
    {0xEF2B6D69U, kRows51},
    {0xEF2B6D6AU, kRows52},
    {0xEFEF8119U, kRows53},
    {0xF1E4371CU, kRows54},
    {0xF25CEA8DU, kRows55},
    {0xF68613C1U, kRows56},
    {0xFA122719U, kRows57},
    {0xFA9B272BU, kRows58},
}};

/** @return True if no bucket names the FNV-1a offset basis as a bubble. */
[[nodiscard]] constexpr bool none_is_unset_sentinel() noexcept {
    for (const Bucket& bucket : kBuckets) {
        if (bucket.bubbleHash == 0x811C9DC5U) {
            return false;
        }
    }
    return true;
}
static_assert(none_is_unset_sentinel(),
              "generated bubble table must never contain the FNV-1a offset basis -- it is not "
              "a real bubble, see lore_grant.h's instanced-activity case instead");

} // namespace detail

/**
 * Looks up one bubble's candidate record rows.
 * @param bubble Bubble hash an incident reported.
 * @return The bucket's rows in table order, or an empty span when the bubble is not mapped.
 */
[[nodiscard]] constexpr std::span<const std::uint16_t> records_for_bubble(
    std::uint32_t bubble) noexcept {
    for (const detail::Bucket& bucket : detail::kBuckets) {
        if (bucket.bubbleHash == bubble) {
            return bucket.rows;
        }
    }
    return {};
}

} // namespace sunrise::state::lore::bubble_record_table
