#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "mission_program.h"

namespace sunrise::server::gameplay::mission {

inline constexpr std::size_t kMissionFileByteLimit = 256 * 1024;

enum class MissionCompileStatus : std::uint8_t {
    success,
    missing,
    ioError,
    tooLarge,
    syntaxError,
    validationError,
    resourceLimit,
};

struct MissionCompileResult final {
    MissionCompileStatus status{MissionCompileStatus::validationError};
    MissionProgram program{};
    std::array<char, 256> error{};
};

[[nodiscard]] MissionCompileResult compile_mission_source(std::string_view source,
                                                          std::string_view destination) noexcept;
[[nodiscard]] MissionCompileResult compile_mission_for_destination(
    const std::filesystem::path& missionDirectory,
    std::string_view destination) noexcept;

} // namespace sunrise::server::gameplay::mission
