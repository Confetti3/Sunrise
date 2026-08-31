#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sunrise::middleware::bap::activity_message::glimmer_extraction {

inline constexpr std::uint64_t kBuildId = 86657;
inline constexpr std::uint32_t kBubble = 51;
inline constexpr std::uint32_t kSlice = 408;
inline constexpr std::uint32_t kResourceSet = 0x80B2FC6A;
inline constexpr std::uint32_t kResource = 0x80BE91F7;
inline constexpr std::uint32_t kGroup = 0x1F6C5054;
inline constexpr std::uint8_t kSpawnerType = 1;
inline constexpr std::uint8_t kSequenceType = 5;
inline constexpr std::uint8_t kCommandType = 58;
inline constexpr std::uint16_t kIntroSequence = 13;
inline constexpr std::uint16_t kActiveSequence = 14;

[[nodiscard]] constexpr std::uint32_t package_name_hash(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : value) hash = (hash * 16777619U) ^ byte;
    return hash;
}

// Exact normal branch configuration. These labels establish package identity only; they do not
// decode the predicted 98-bit runtime body or authorize a lifecycle signal.
inline constexpr std::uint32_t kNormalFailureChild = 0x80C016AE;
inline constexpr std::uint32_t kNormalSuccessChild = 0x80C016B3;
inline constexpr std::uint32_t kNormalFailureVariant = 0x815AE58A;
inline constexpr std::uint32_t kNormalSuccessVariant = 0x815AE58F;
inline constexpr std::uint32_t kNormalFailureLabel = package_name_hash("failure");
inline constexpr std::uint32_t kNormalSuccessLabel = package_name_hash("success");

// Exact build-87221 generic-codec result. Kind 2 owns one bit, so the four-record
// Sense descriptor owns 32+32+32+1 = 97 bits and occupies 13 packed bytes. Some
// containing captures were predicted as 98 bits; the extra bit's position and source
// are unresolved, so no aligned field split is exposed here.
inline constexpr std::uint16_t kCrossBuildHopOnDescriptorBits = 97;
inline constexpr std::uint8_t kCrossBuildHopOnPackedBytes = 13;
inline constexpr std::uint16_t kCrossBuildHopOnContainingBitsMax = 98;

struct Identity final { std::uint16_t slot; std::uint32_t hash; };

struct Placement final {
    std::uint64_t worldId;
    float x;
    float y;
    float z;
};

struct Site final {
    Identity objective;
    Identity dropship;
    Identity pilot;
    Identity glimmerDevice;
    Identity shipNearRule;
    Identity dropshipRule;
    Identity enterCommand;
    Identity exitCommand;
    Identity defenders;
    Identity normalChest;
    Placement placement;
};

inline constexpr std::array<Site, 3> kSites{{
    {{18, 0x7A3FD531}, {28, 0x507CE4BF}, {29, 0x76169C3D}, {78, 0xCD867332},
      {321, 0xD5F5F67F}, {324, 0xC1B4953A}, {394, 0x80D8AC75},
      {397, 0xC8F8CD69}, {207, 0x2F8A65F9}, {67, 0x6AF8AF2F},
      {0x41EAB9523B5BCA1DULL, 410.1298218F, 69.0198364F, 126.7949677F}},
    {{82, 0x4865E3E2}, {92, 0x0A6056B8}, {93, 0x49BF5F34}, {142, 0x80DBBD25},
      {421, 0x406D92D0}, {427, 0xCD03E5E9}, {331, 0x3676E0C0},
      {343, 0x4B3ADD2A}, {208, 0x308A676A}, {131, 0x05325AB8},
      {0x16A6225A59CB0B5DULL, 429.8782349F, 49.3713989F, 124.6949768F}},
    {{146, 0x236469F7}, {156, 0x435BDB75}, {157, 0x406F3FE3}, {206, 0x200E588C},
      {442, 0x97EC5E85}, {443, 0xDC1530C4}, {355, 0xD06AEB8B},
      {356, 0xE904662B}, {209, 0x318A6897}, {195, 0x2AD95805},
      {0xC1E6863A655ED91CULL, 360.5499268F, 79.5714035F, 126.6949768F}},
}};

inline constexpr std::array<std::uint16_t, 3> kDefenderAnchors{210, 211, 212};
inline constexpr std::array<std::uint16_t, 3> kNormalBosses{213, 215, 217};
inline constexpr std::array<Identity, 3> kFailureHopons{{
    {19, 0xE15EB3F7}, {83, 0x55AA9F56}, {147, 0x1BA1A6AD},
}};
inline constexpr std::array<Identity, 3> kSuccessHopons{{
    {20, 0xD960E6E4}, {84, 0xEDCFE539}, {148, 0xF62D784E},
}};
inline constexpr std::array<Identity, 3> kDrillLaserChannels{{
    {231, 0x7C3FF939}, {261, 0xA3F26EDE}, {291, 0x186F3193},
}};
inline constexpr std::array<Identity, 3> kPlacementEngagementMonitors{{
    {235, 0x64AC796C}, {265, 0x725152FF}, {295, 0x44C8394A},
}};
inline constexpr std::uint16_t kFleeObjective = 224;
inline constexpr std::uint16_t kEngagementSensor = 225;
inline constexpr std::uint16_t kDirectiveSensor = 226;
inline constexpr std::uint16_t kPublicEventSensor = 229;
inline constexpr std::uint16_t kDummyPlacement = 450;

static_assert(kNormalFailureLabel == 0xE73265DD);
static_assert(kNormalSuccessLabel == 0xD8120976);
static_assert(kSites.size() == 3);
static_assert(kSites[0].enterCommand.slot != kSites[1].enterCommand.slot
              && kSites[1].enterCommand.slot != kSites[2].enterCommand.slot);

} // namespace sunrise::middleware::bap::activity_message::glimmer_extraction
