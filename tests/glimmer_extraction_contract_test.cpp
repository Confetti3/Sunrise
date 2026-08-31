#include <cassert>

#include "middleware/bap/activity_message/glimmer_extraction_contract.h"

namespace contract =
    sunrise::middleware::bap::activity_message::glimmer_extraction;

int main() {
    static_assert(contract::kBuildId == 86657);
    static_assert(contract::kBubble == 51 && contract::kSlice == 408);
    static_assert(contract::kResourceSet == 0x80B2FC6A);
    static_assert(contract::kResource == 0x80BE91F7);
    static_assert(contract::kGroup == 0x1F6C5054);
    static_assert(contract::kIntroSequence == 13 && contract::kActiveSequence == 14);
    static_assert(contract::kNormalFailureChild == 0x80C016AE
                  && contract::kNormalSuccessChild == 0x80C016B3);
    static_assert(contract::kNormalFailureVariant == 0x815AE58A
                  && contract::kNormalSuccessVariant == 0x815AE58F);
    static_assert(contract::kNormalFailureLabel == 0xE73265DD
                  && contract::kNormalSuccessLabel == 0xD8120976);
    static_assert(contract::package_name_hash("failure") == contract::kNormalFailureLabel
                  && contract::package_name_hash("success") == contract::kNormalSuccessLabel);
    static_assert(contract::kCrossBuildHopOnDescriptorBits == 97);
    static_assert(contract::kCrossBuildHopOnPackedBytes == 13);
    static_assert(contract::kCrossBuildHopOnContainingBitsMax == 98);
    static_assert(contract::kSites[0].dropship.slot == 28);
    static_assert(contract::kSites[0].enterCommand.slot == 394
                  && contract::kSites[0].exitCommand.slot == 397);
    static_assert(contract::kSites[1].dropship.slot == 92);
    static_assert(contract::kSites[1].enterCommand.slot == 331
                  && contract::kSites[1].exitCommand.slot == 343);
    static_assert(contract::kSites[2].dropship.slot == 156);
    static_assert(contract::kSites[2].enterCommand.slot == 355
                  && contract::kSites[2].exitCommand.slot == 356);
    static_assert(contract::kSites[0].normalChest.slot == 67
                  && contract::kSites[1].normalChest.slot == 131
                  && contract::kSites[2].normalChest.slot == 195);
    static_assert(contract::kSites[0].defenders.slot == 207
                  && contract::kSites[1].defenders.slot == 208
                  && contract::kSites[2].defenders.slot == 209);
    static_assert(contract::kDefenderAnchors[0] == 210
                  && contract::kDefenderAnchors[1] == 211
                  && contract::kDefenderAnchors[2] == 212);
    static_assert(contract::kNormalBosses[0] == 213
                  && contract::kNormalBosses[1] == 215
                  && contract::kNormalBosses[2] == 217);
    static_assert(contract::kFailureHopons[0].slot == 19
                  && contract::kFailureHopons[1].slot == 83
                  && contract::kFailureHopons[2].slot == 147);
    static_assert(contract::kSuccessHopons[0].slot == 20
                  && contract::kSuccessHopons[1].slot == 84
                  && contract::kSuccessHopons[2].slot == 148);
    static_assert(contract::kDrillLaserChannels[0].slot == 231
                  && contract::kDrillLaserChannels[1].slot == 261
                  && contract::kDrillLaserChannels[2].slot == 291);
    static_assert(contract::kPlacementEngagementMonitors[0].slot == 235
                  && contract::kPlacementEngagementMonitors[1].slot == 265
                  && contract::kPlacementEngagementMonitors[2].slot == 295);
    static_assert(contract::kPublicEventSensor == 229);
    static_assert(contract::kDummyPlacement == 450);
    assert(contract::kSites.size() == 3);
}
