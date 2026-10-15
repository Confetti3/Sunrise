#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::middleware::gameplay::group {

/** Registry index of every group-session parameter. The names are the binary's own strings. */
enum class Parameter : std::uint8_t {
    worldControllerGoalData = 0,
    activeJoinControls = 1,
    atomicCascadeJoinData = 2,
    activityHost = 3,
    jipGate = 4,
    activitySelection = 5,
    activitySelectionResponses = 6,
    currentActivity = 7,
    previousActivity = 8,
    userJoinControls = 9,
    desiredJoinControls = 10,
    language = 11,
    requestedRemoteJoinData = 12,
    remoteJoinData = 13,
    remoteJoinResult = 14,
    hostSelected = 15,
    matchmakingMessaging = 16,
    matchmakingAbortRequested = 17,
    sessionDisbandReason = 18,
    matchmakingProgress = 19,
    initialSliceSetStatus = 20,
    publicSessionReservations = 21,
    matchmakingData = 22,
    matchmakingPeerData = 23,
    networkQuality = 24,
};

/**
 * Names the parameters one mask selects, as a comma-separated list.
 * @param mask Parameter mask; only its low 25 bits are read.
 * @param output Destination buffer.
 * @param capacity Bytes available, including the terminator.
 * @return `output`, always terminated. An empty mask gives "none".
 */
const char* parameter_names(std::uint64_t mask, char* output, std::size_t capacity) noexcept;

} // namespace sunrise::middleware::gameplay::group
