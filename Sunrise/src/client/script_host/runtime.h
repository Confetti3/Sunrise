#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../state/activity/forced/activity_forced_destination.h"
#include "../../state/activity/incidents/runtime.h"
#include "../../state/activity/runtime.h"

namespace sunrise::client::script_host {
namespace detail {

inline constexpr wchar_t kDefaultPipePath[] = L"\\\\.\\pipe\\sunrise-script-host-v1";
inline constexpr std::size_t kPipePathCapacity = 256;
inline constexpr std::size_t kInboundCapacity = 8192;
inline constexpr std::size_t kLineCapacity = 4096;
inline constexpr DWORD kReconnectDelayMilliseconds = 750;
inline constexpr DWORD kPollMilliseconds = 100;

inline SRWLOCK g_lifecycleLock{SRWLOCK_INIT};
inline std::atomic_bool g_running{false};
inline std::atomic_bool g_connected{false};
inline HANDLE g_stopEvent{};
inline HANDLE g_thread{};

[[nodiscard]] inline const char* phase_name(state::activity::WorldPhase phase) noexcept {
    switch (phase) {
    case state::activity::WorldPhase::idle:
        return "idle";
    case state::activity::WorldPhase::transitioning:
        return "transitioning";
    case state::activity::WorldPhase::arrived:
        return "arrived";
    }
    return "unknown";
}

inline void report(core::log::Level level, std::string_view message) noexcept {
    core::log::write(core::log::Channel::client, level, message);
}

[[nodiscard]] inline bool disabled() noexcept {
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"SUNRISE_SCRIPT_HOST_DISABLED", value.data(), static_cast<DWORD>(value.size()));
    return length != 0 && length < value.size() && value[0] == L'1';
}

inline void pipe_path(std::array<wchar_t, kPipePathCapacity>& output) noexcept {
    output.fill(L'\0');
    std::array<wchar_t, kPipePathCapacity> configured{};
    const DWORD length = GetEnvironmentVariableW(
        L"SUNRISE_SCRIPT_HOST_PIPE", configured.data(), static_cast<DWORD>(configured.size()));
    if (length == 0 || length >= configured.size()) {
        (void)wcscpy_s(output.data(), output.size(), kDefaultPipePath);
        return;
    }

    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    const std::wstring_view value(configured.data(), length);
    if (value.starts_with(prefix)) {
        (void)wcsncpy_s(output.data(), output.size(), value.data(), value.size());
        return;
    }

    const int written = _snwprintf_s(output.data(),
                                     output.size(),
                                     _TRUNCATE,
                                     L"\\\\.\\pipe\\%.*s",
                                     static_cast<int>(value.size()),
                                     value.data());
    if (written <= 0) {
        (void)wcscpy_s(output.data(), output.size(), kDefaultPipePath);
    }
}

[[nodiscard]] inline bool write_all(HANDLE pipe, const char* data, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const DWORD requested = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (WriteFile(pipe, data + offset, requested, &written, nullptr) == FALSE || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

[[nodiscard]] inline bool write_line(HANDLE pipe, std::string_view line) noexcept {
    return write_all(pipe, line.data(), line.size()) && write_all(pipe, "\n", 1);
}

[[nodiscard]] inline bool write_hello(HANDLE pipe) noexcept {
    constexpr std::string_view hello =
        R"({"protocol":1,"type":"bridge.hello","bridge":"sunrise-native","build":"Sunrise 0.2.1 / Destiny 2 build 86657","capabilities":["host.ping","world.phase.observe","activity.incident.observe","activity.override.configure"],"limitations":["no activity-session snapshot","no incident encoder","no objective adapter","no actor lifecycle adapter","no AI policy"]})";
    return write_line(pipe, hello);
}

[[nodiscard]] inline bool write_world_phase(HANDLE pipe,
                                            state::activity::WorldPhase phase) noexcept {
    std::array<char, 192> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      R"({"protocol":1,"type":"world.phase","phase":"%s","transitionAgeMs":%llu})",
                      phase_name(phase),
                      static_cast<unsigned long long>(state::activity::world_transition_age()));
    return written > 0 && static_cast<std::size_t>(written) < line.size()
           && write_line(pipe, std::string_view(line.data(), static_cast<std::size_t>(written)));
}

template <std::size_t Capacity, typename... Arguments>
[[nodiscard]] inline bool append_format(std::array<char, Capacity>& output,
                                        std::size_t& length,
                                        const char* format,
                                        Arguments... arguments) noexcept {
    if (length >= output.size()) {
        return false;
    }
    const std::size_t remaining = output.size() - length;
    const int written = std::snprintf(output.data() + length, remaining, format, arguments...);
    if (written <= 0 || static_cast<std::size_t>(written) >= remaining) {
        return false;
    }
    length += static_cast<std::size_t>(written);
    return true;
}

[[nodiscard]] inline bool
write_activity_incident(HANDLE pipe,
                        const state::activity::incidents::Observation& observation) noexcept {
    std::array<char, 1024> line{};
    std::size_t length = 0;
    if (!append_format(
            line,
            length,
            R"({"protocol":1,"type":"activity.incident","sequence":%llu,"observedAtTickMs":%llu,"sessionId":"%llu","accountHandle":"0x%llX","primaryTarget":%u,"extraTargetCount":%u,"extraTargets":[)",
            static_cast<unsigned long long>(observation.sequence),
            static_cast<unsigned long long>(observation.observedAtTickMs),
            static_cast<unsigned long long>(observation.sessionId),
            static_cast<unsigned long long>(observation.accountHandle),
            observation.primaryTarget,
            observation.extraTargetCount)) {
        return false;
    }
    for (std::uint32_t index = 0; index < observation.extraTargetCount; ++index) {
        if (!append_format(
                line, length, index == 0 ? "%u" : ",%u", observation.extraTargets[index])) {
            return false;
        }
    }
    if (!append_format(
            line,
            length,
            R"(],"payloadLength":%u,"bodyLength":%u,"bodyFingerprint":"0x%016llX","hasCompressedSelector":%s,"hasPayload":%s,"droppedBefore":%llu})",
            observation.payloadLength,
            observation.bodyLength,
            static_cast<unsigned long long>(observation.bodyFingerprint),
            observation.hasCompressedSelector ? "true" : "false",
            observation.hasPayload ? "true" : "false",
            static_cast<unsigned long long>(observation.droppedBefore))) {
        return false;
    }
    return write_line(pipe, std::string_view(line.data(), length));
}

inline void report_stale_incidents(std::size_t discarded) noexcept {
    if (discarded == 0) {
        return;
    }
    std::array<char, 192> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=script_host stage=incident_relay result=drop "
                      "reason=reconnect count=%zu total=%llu",
                      discarded,
                      static_cast<unsigned long long>(state::activity::incidents::dropped_count()));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        report(core::log::Level::info,
               std::string_view(line.data(), static_cast<std::size_t>(written)));
    }
}

[[nodiscard]] inline bool extract_json_string(std::string_view line,
                                              std::string_view key,
                                              std::array<char, 96>& output,
                                              std::size_t& outputLength) noexcept {
    output.fill('\0');
    outputLength = 0;
    std::array<char, 64> token{};
    const int tokenLength = std::snprintf(
        token.data(), token.size(), "\"%.*s\"", static_cast<int>(key.size()), key.data());
    if (tokenLength <= 0 || static_cast<std::size_t>(tokenLength) >= token.size()) {
        return false;
    }
    const std::size_t keyPosition =
        line.find(std::string_view(token.data(), static_cast<std::size_t>(tokenLength)));
    if (keyPosition == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = line.find(':', keyPosition + static_cast<std::size_t>(tokenLength));
    const std::size_t firstQuote =
        colon == std::string_view::npos ? std::string_view::npos : line.find('"', colon + 1);
    const std::size_t lastQuote = firstQuote == std::string_view::npos
                                      ? std::string_view::npos
                                      : line.find('"', firstQuote + 1);
    if (firstQuote == std::string_view::npos || lastQuote == std::string_view::npos) {
        return false;
    }
    const std::size_t length = lastQuote - firstQuote - 1;
    if (length == 0 || length >= output.size()) {
        return false;
    }
    std::memcpy(output.data(), line.data() + firstQuote + 1, length);
    outputLength = length;
    return true;
}

[[nodiscard]] inline bool extract_json_value(std::string_view line,
                                             std::string_view key,
                                             std::string_view& output) noexcept {
    output = {};
    std::array<char, 64> token{};
    const int tokenLength = std::snprintf(
        token.data(), token.size(), "\"%.*s\"", static_cast<int>(key.size()), key.data());
    if (tokenLength <= 0 || static_cast<std::size_t>(tokenLength) >= token.size()) {
        return false;
    }
    const std::size_t keyPosition =
        line.find(std::string_view(token.data(), static_cast<std::size_t>(tokenLength)));
    const std::size_t colon = keyPosition == std::string_view::npos
                                  ? std::string_view::npos
                                  : line.find(':', keyPosition + tokenLength);
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t first = colon + 1;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
        ++first;
    }
    if (first == line.size()) {
        return false;
    }
    if (line[first] == '"') {
        const std::size_t last = line.find('"', first + 1);
        if (last == std::string_view::npos || last == first + 1) {
            return false;
        }
        output = line.substr(first + 1, last - first - 1);
        return true;
    }
    std::size_t last = first;
    while (last < line.size() && line[last] != ',' && line[last] != '}' && line[last] != ' '
           && line[last] != '\t' && line[last] != '\r' && line[last] != '\n') {
        ++last;
    }
    if (last == first) {
        return false;
    }
    output = line.substr(first, last - first);
    return true;
}

[[nodiscard]] inline bool parse_unsigned(std::string_view text,
                                         std::uint64_t maximum,
                                         std::uint64_t& output) noexcept {
    output = 0;
    unsigned base = 10;
    std::size_t index = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2;
    }
    if (index == text.size()) {
        return false;
    }
    for (; index < text.size(); ++index) {
        const char character = text[index];
        unsigned digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned>(character - '0');
        } else if (base == 16 && character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned>(character - 'a') + 10U;
        } else if (base == 16 && character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned>(character - 'A') + 10U;
        } else {
            return false;
        }
        if (digit >= base || output > (maximum - digit) / base) {
            return false;
        }
        output = output * base + digit;
    }
    return true;
}

[[nodiscard]] inline bool extract_json_unsigned(std::string_view line,
                                                std::string_view key,
                                                std::uint64_t maximum,
                                                std::uint64_t& output) noexcept {
    std::string_view value;
    return extract_json_value(line, key, value) && parse_unsigned(value, maximum, output);
}

[[nodiscard]] inline bool extract_json_boolean(std::string_view line,
                                               std::string_view key,
                                               bool& output) noexcept {
    std::string_view value;
    if (!extract_json_value(line, key, value)) {
        return false;
    }
    if (value == "true") {
        output = true;
        return true;
    }
    if (value == "false") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool write_command_result(HANDLE pipe,
                                               std::string_view requestId,
                                               std::string_view capability,
                                               std::string_view status,
                                               std::string_view reason) noexcept {
    std::array<char, 512> response{};
    const int written = std::snprintf(
        response.data(),
        response.size(),
        R"({"protocol":1,"type":"command.result","requestId":"%.*s","capability":"%.*s","status":"%.*s","reason":"%.*s"})",
        static_cast<int>(requestId.size()),
        requestId.data(),
        static_cast<int>(capability.size()),
        capability.data(),
        static_cast<int>(status.size()),
        status.data(),
        static_cast<int>(reason.size()),
        reason.data());
    return written > 0 && static_cast<std::size_t>(written) < response.size()
           && write_line(pipe, std::string_view(response.data(), static_cast<std::size_t>(written)));
}

[[nodiscard]] inline bool valid_package_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > state::activity::destination::kPackageNameCapacity) {
        return false;
    }
    for (const char character : name) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
              || character == '_')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool write_activity_override_result(HANDLE pipe,
                                                         std::string_view line) noexcept {
    constexpr std::string_view capabilityName = "activity.override.configure";
    std::array<char, 96> requestId{};
    std::array<char, 96> capability{};
    std::size_t requestLength = 0;
    std::size_t capabilityLength = 0;
    if (!extract_json_string(line, "requestId", requestId, requestLength)
        || !extract_json_string(line, "capability", capability, capabilityLength)
        || std::string_view(capability.data(), capabilityLength) != capabilityName) {
        return write_command_result(pipe,
                                    "unknown",
                                    capabilityName,
                                    "error",
                                    "malformed activity override request");
    }
    const std::string_view request(requestId.data(), requestLength);
    bool enabled = false;
    if (!extract_json_boolean(line, "enabled", enabled)) {
        return write_command_result(
            pipe, request, capabilityName, "error", "enabled must be a boolean");
    }
    if (!enabled) {
        state::activity::forced::clear();
        report(core::log::Level::info,
               "ev=script_host stage=activity_override result=cleared");
        return write_command_result(pipe, request, capabilityName, "ok", "override cleared");
    }

    std::array<char, 96> package{};
    std::size_t packageLength = 0;
    std::uint64_t bubble = 0;
    std::uint64_t sliceSet = 0;
    if (!extract_json_string(line, "packageName", package, packageLength)
        || !valid_package_name(std::string_view(package.data(), packageLength))
        || !extract_json_unsigned(line, "bubble", state::activity::forced::kMaximumBubble, bubble)
        || !extract_json_unsigned(
            line, "sliceSet", state::activity::forced::kMaximumSliceSet, sliceSet)) {
        return write_command_result(pipe,
                                    request,
                                    capabilityName,
                                    "error",
                                    "packageName, bubble, or sliceSet is invalid");
    }

    state::activity::forced::ForcedDestination value{};
    std::copy_n(package.begin(), packageLength, value.packageName.begin());
    value.packageNameLength = static_cast<std::uint8_t>(packageLength);
    value.bubble = static_cast<std::uint8_t>(bubble);
    value.hasBubble = true;
    value.sliceSet = static_cast<std::uint16_t>(sliceSet);
    value.hasSliceSet = true;
    value.enabled = true;
    if (line.find(R"("spawnSetHash")") != std::string_view::npos) {
        std::uint64_t spawnSet = 0;
        if (!extract_json_unsigned(
                line, "spawnSetHash", (std::numeric_limits<std::uint32_t>::max)(), spawnSet)) {
            return write_command_result(
                pipe, request, capabilityName, "error", "spawnSetHash is invalid");
        }
        value.spawnSetHash = static_cast<std::uint32_t>(spawnSet);
        value.hasSpawnSetHash = true;
    }
    if (!state::activity::forced::publish(value)) {
        return write_command_result(
            pipe, request, capabilityName, "error", "override failed state validation");
    }
    report(core::log::Level::info,
           "ev=script_host stage=activity_override result=published");
    return write_command_result(pipe, request, capabilityName, "ok", "override published");
}

[[nodiscard]] inline bool write_unsupported(HANDLE pipe, std::string_view line) noexcept {
    std::array<char, 96> requestId{};
    std::array<char, 96> capability{};
    std::size_t requestLength = 0;
    std::size_t capabilityLength = 0;
    if (!extract_json_string(line, "requestId", requestId, requestLength)
        || !extract_json_string(line, "capability", capability, capabilityLength)) {
        return write_line(
            pipe,
            R"({"protocol":1,"type":"command.result","requestId":"unknown","status":"error","reason":"malformed command.request"})");
    }

    std::array<char, 384> response{};
    const int written = std::snprintf(
        response.data(),
        response.size(),
        R"({"protocol":1,"type":"command.result","requestId":"%.*s","capability":"%.*s","status":"unsupported","reason":"native adapter not implemented for this verified build"})",
        static_cast<int>(requestLength),
        requestId.data(),
        static_cast<int>(capabilityLength),
        capability.data());
    return written > 0 && static_cast<std::size_t>(written) < response.size()
           && write_line(pipe,
                         std::string_view(response.data(), static_cast<std::size_t>(written)));
}

[[nodiscard]] inline bool process_line(HANDLE pipe, std::string_view line) noexcept {
    if (line.find(R"("type":"host.ping")") != std::string_view::npos) {
        return write_line(pipe, R"({"protocol":1,"type":"bridge.pong"})");
    }
    if (line.find(R"("type":"host.capabilities")") != std::string_view::npos) {
        return write_hello(pipe);
    }
    if (line.find(R"("type":"command.request")") != std::string_view::npos) {
        if (line.find(R"("capability":"activity.override.configure")")
            != std::string_view::npos) {
            return write_activity_override_result(pipe, line);
        }
        return write_unsupported(pipe, line);
    }
    return true;
}

[[nodiscard]] inline bool receive_available(HANDLE pipe,
                                            std::array<char, kInboundCapacity>& inbound,
                                            std::size_t& inboundSize) noexcept {
    DWORD available = 0;
    if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
        return false;
    }
    if (available == 0) {
        return true;
    }
    if (inboundSize == inbound.size()) {
        inboundSize = 0;
    }

    const std::size_t capacity = inbound.size() - inboundSize;
    const DWORD requested =
        static_cast<DWORD>((std::min)(capacity, static_cast<std::size_t>(available)));
    DWORD read = 0;
    if (requested == 0
        || ReadFile(pipe, inbound.data() + inboundSize, requested, &read, nullptr) == FALSE) {
        return false;
    }
    inboundSize += read;

    std::size_t consumed = 0;
    while (consumed < inboundSize) {
        const char* const start = inbound.data() + consumed;
        const void* const delimiter = std::memchr(start, '\n', inboundSize - consumed);
        if (delimiter == nullptr) {
            break;
        }
        const auto* const newline = static_cast<const char*>(delimiter);
        std::size_t length = static_cast<std::size_t>(newline - start);
        if (length != 0 && start[length - 1] == '\r') {
            --length;
        }
        if (length != 0 && length < kLineCapacity
            && !process_line(pipe, std::string_view(start, length))) {
            return false;
        }
        consumed += static_cast<std::size_t>(newline - start) + 1;
    }

    if (consumed != 0) {
        std::memmove(inbound.data(), inbound.data() + consumed, inboundSize - consumed);
        inboundSize -= consumed;
    }
    return true;
}

[[nodiscard]] inline HANDLE stop_event() noexcept {
    AcquireSRWLockShared(&g_lifecycleLock);
    HANDLE const event = g_stopEvent;
    ReleaseSRWLockShared(&g_lifecycleLock);
    return event;
}

inline DWORD WINAPI worker(void*) noexcept {
    const HANDLE stop = stop_event();
    if (stop == nullptr) {
        g_running.store(false, std::memory_order_release);
        return 0;
    }

    std::array<wchar_t, kPipePathCapacity> path{};
    pipe_path(path);
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        if (WaitNamedPipeW(path.data(), kPollMilliseconds) == FALSE) {
            if (WaitForSingleObject(stop, kReconnectDelayMilliseconds) != WAIT_TIMEOUT) {
                break;
            }
            continue;
        }

        HANDLE const pipe = CreateFileW(path.data(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            continue;
        }

        g_connected.store(true, std::memory_order_release);
        report(core::log::Level::info, "ev=script_host stage=connect result=ok");
        report_stale_incidents(state::activity::incidents::discard_pending());
        bool alive = write_hello(pipe);
        state::activity::WorldPhase lastPhase = state::activity::world_phase();
        alive = alive && write_world_phase(pipe, lastPhase);
        std::array<char, kInboundCapacity> inbound{};
        std::size_t inboundSize = 0;

        while (alive && WaitForSingleObject(stop, kPollMilliseconds) == WAIT_TIMEOUT) {
            const state::activity::WorldPhase phase = state::activity::world_phase();
            if (phase != lastPhase) {
                alive = write_world_phase(pipe, phase);
                lastPhase = phase;
            }
            state::activity::incidents::Observation incident{};
            while (alive && state::activity::incidents::try_pop(incident)) {
                alive = write_activity_incident(pipe, incident);
            }
            if (alive) {
                alive = receive_available(pipe, inbound, inboundSize);
            }
        }

        g_connected.store(false, std::memory_order_release);
        (void)CloseHandle(pipe);
        report(core::log::Level::info, "ev=script_host stage=disconnect result=ok");
    }

    g_connected.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
    return 0;
}

} // namespace detail

/** Starts the optional out-of-process script-host bridge. Failure never demotes Sunrise boot. */
[[nodiscard]] inline bool start() noexcept {
    if (detail::disabled()) {
        detail::report(core::log::Level::info,
                       "ev=script_host stage=start result=disabled reason=environment");
        return true;
    }

    AcquireSRWLockExclusive(&detail::g_lifecycleLock);
    if (detail::g_thread != nullptr) {
        ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
        return true;
    }

    detail::g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (detail::g_stopEvent == nullptr) {
        ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
        detail::report(core::log::Level::warn,
                       "ev=script_host stage=start result=fail reason=stop_event");
        return false;
    }

    detail::g_running.store(true, std::memory_order_release);
    detail::g_thread = CreateThread(nullptr, 0, detail::worker, nullptr, 0, nullptr);
    if (detail::g_thread == nullptr) {
        detail::g_running.store(false, std::memory_order_release);
        (void)CloseHandle(detail::g_stopEvent);
        detail::g_stopEvent = nullptr;
        ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
        detail::report(core::log::Level::warn,
                       "ev=script_host stage=start result=fail reason=worker_thread");
        return false;
    }

    ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
    detail::report(core::log::Level::info, "ev=script_host stage=start result=ok");
    return true;
}

/** Stops the worker before client State and hooks begin teardown. */
inline void stop() noexcept {
    AcquireSRWLockExclusive(&detail::g_lifecycleLock);
    HANDLE const thread = detail::g_thread;
    HANDLE const stop = detail::g_stopEvent;
    if (thread == nullptr) {
        ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
        return;
    }
    if (stop != nullptr) {
        (void)SetEvent(stop);
    }
    ReleaseSRWLockExclusive(&detail::g_lifecycleLock);

    (void)WaitForSingleObject(thread, INFINITE);

    AcquireSRWLockExclusive(&detail::g_lifecycleLock);
    (void)CloseHandle(thread);
    if (stop != nullptr) {
        (void)CloseHandle(stop);
    }
    detail::g_thread = nullptr;
    detail::g_stopEvent = nullptr;
    detail::g_connected.store(false, std::memory_order_release);
    detail::g_running.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&detail::g_lifecycleLock);
    detail::report(core::log::Level::info, "ev=script_host stage=stop result=ok");
}

/** @return True only while the C# pipe server is connected. */
[[nodiscard]] inline bool connected() noexcept {
    return detail::g_connected.load(std::memory_order_acquire);
}

} // namespace sunrise::client::script_host
