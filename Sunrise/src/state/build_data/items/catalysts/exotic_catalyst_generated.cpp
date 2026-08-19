#include "exotic_catalyst_builder.h"

#include <array>

namespace sunrise::state::build_data::items::catalysts {
namespace {

/** PE identity of the supported 86657.20.08.23 client. */
constexpr std::uint32_t kImageTimestamp = 0x5F43138BU;
constexpr std::uint32_t kImageSize = 0x08A5EA00U;
/** Later catalysts use this build's shared empty socket plug. */
constexpr std::uint32_t kEmptyCatalystPlugHash = 0x5957A904U;

/** Legacy completed-plug facts generated from the pinned target manifest. */
constexpr std::array<std::uint32_t, 30> kLegacyCompletionPlugHashes{
    0x151E1554U, 0x151E1555U, 0x174B3FEBU, 0x206EDFE0U, 0x206EDFE1U,
    0x288A777DU, 0x2FB02CF0U, 0x32F442E6U, 0x4FE33F81U, 0x6096F61AU,
    0x6096F61BU, 0x61935831U, 0x641204BFU, 0x69A468F9U, 0x70B8A117U,
    0x7D463B2FU, 0x7FB372AAU, 0x8808888CU, 0x8808888DU, 0x8F90F557U,
    0x9C8C0A61U, 0xA651C900U, 0xAA5EEFD0U, 0xAA5EEFD1U, 0xC9C0E4C0U,
    0xE2CB93CBU, 0xE3700214U, 0xE681F877U, 0xFC5C45A8U, 0xFC5C45A9U,
};

/** Released Season 11 catalyst facts, in definition-hash order. */
constexpr std::array<std::uint32_t, 45> kReleasedWeaponHashes{
    0x012248BAU, 0x14B465B2U, 0x17D8FEABU, 0x2E43BDEEU, 0x3092080DU,
    0x4F5CCF1DU, 0x50384F32U, 0x50384F33U, 0x5141601FU, 0x59EFED62U,
    0x5BDBCC56U, 0x634C6957U, 0x6F22FCECU, 0x70BEF156U, 0x83A19696U,
    0x8843C72AU, 0x8C8180D6U, 0x8CD074B1U, 0x93E680C3U, 0xA7DBFF3AU,
    0xAA45882AU, 0xAD4746D4U, 0xAD4746D5U, 0xB824C63DU, 0xBB46CCD2U,
    0xBB46CCD3U, 0xBF704917U, 0xCB6F6266U, 0xCB7B5EDFU, 0xCCE7D927U,
    0xD15517D4U, 0xD38BCABAU, 0xD38BCABBU, 0xD5704484U, 0xD5704485U,
    0xD84E04AAU, 0xD84E04ABU, 0xE0794C51U, 0xE5296126U, 0xE86A25CFU,
    0xEF7D3366U, 0xF0923C79U, 0xF5DE4480U, 0xF9C0B6B0U, 0xFDA23E68U,
};

} // namespace

Facts generated_facts() noexcept {
    return {kImageTimestamp,
            kImageSize,
            kEmptyCatalystPlugHash,
            kLegacyCompletionPlugHashes,
            kReleasedWeaponHashes};
}

bool matches_target_build(const BuildIdentity& build) noexcept {
    return build.imageTimestamp == kImageTimestamp && build.imageSize == kImageSize;
}

} // namespace sunrise::state::build_data::items::catalysts
