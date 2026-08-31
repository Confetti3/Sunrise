#include "retail_log_enqueue_observer.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../diagnostics/module_range.h"
#include "../squad_reference_probe/squad_reference_probe.h"
#include "../../targets/game.h"

namespace sunrise::client::hooks::retail_log {
namespace {

using Enqueue = void(__fastcall*)(std::int32_t, const char*) noexcept;
using SetCategoryVerbosity = void(__fastcall*)(std::int32_t, std::uint32_t) noexcept;

/** The game copies exactly this many bytes out of the caller's text buffer. */
constexpr std::size_t kNativeTextSize = 320;
/** Site id the game uses for an unregistered line. */
constexpr std::int32_t kUnregisteredSite = -1;
/** Line storage holds the cleaned text plus its fixed key prefix. */
constexpr std::size_t kEventCapacity = kNativeTextSize + 64;
/** A late config load resets the thresholds, so set them again on this period. A count will not
 *  do: a closed category emits fewer lines, so it advances slower and stays closed. */
constexpr std::uint64_t kReassertIntervalMs = 2'000;
/** How many categories the game's own verbosity table holds. */
constexpr std::uint32_t kCategoryCount = 26;
/** 0 is the game's loosest category threshold. A higher value logs less. */
constexpr std::uint32_t kMostVerbose = 0;
constexpr std::string_view kSobjectFailure =
    "networking:simulation:entity: failed to create 'sobject' entity";
constexpr std::size_t kCallerCodeBytes = 32;
constexpr std::size_t kFunctionCodeBytes = 128;
constexpr std::size_t kLowerCodeBytes = 160;
constexpr std::size_t kWideFunctionBytes = 1024;
constexpr std::size_t kWideChunkBytes = 48;
constexpr std::size_t kWideCalleeLimit = 8;
constexpr std::uint32_t kBackgroundFailureLimit = 2;
constexpr std::uint32_t kRecentFailureLimit = 16;
constexpr std::uint64_t kRecentBuildWindowMs = 250;

thread_local bool g_inObserver{};
/** Tick at which the next re-assert is due. Zero makes the first call assert. */
volatile LONG64 g_nextAssertTick{};
volatile LONG g_sobjectFailureCount{};
volatile LONG g_recentSobjectFailureCount{};
volatile LONG g_wideCaptureState{};
volatile LONG64 g_wideCaptureGeneration{1};
volatile LONG64 g_codeCaptureId{};

/**
 * Copies the native text into fixed storage as one printable line.
 * @param text Borrowed native buffer.
 * @param output Receives the cleaned characters.
 * @return Number of characters written.
 */
[[nodiscard]] std::size_t sanitize(const char* text, std::array<char, kNativeTextSize>& output) {
    std::size_t length = 0;
    __try {
        for (; length < kNativeTextSize - 1 && text[length] != '\0'; ++length) {
            const char value = text[length];
            // One line, one event: the native text carries its own line breaks.
            output[length] = value >= ' ' && value != '\x7F' ? value : ' ';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    while (length != 0 && output[length - 1] == ' ') {
        --length;
    }
    return length;
}

[[nodiscard]] bool executable_address(std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof region)
        != sizeof region) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = region.Protect & 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
           || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

struct WideFunctionCapture final {
    std::uintptr_t start{};
    std::uintptr_t end{};
    std::size_t copied{};
    std::array<std::byte, kWideFunctionBytes> bytes{};
};

[[nodiscard]] bool read_wide_function(const client::diagnostics::ModuleRange& image,
                                      std::uintptr_t start,
                                      std::uintptr_t end,
                                      WideFunctionCapture& output) noexcept {
    output = {};
    if (start == 0 || start >= end || end > image.end
        || !client::diagnostics::contains(image, start) || !executable_address(start)) return false;
    const std::size_t available = static_cast<std::size_t>(end - start);
    const std::size_t readSize = available < output.bytes.size() ? available : output.bytes.size();
    SIZE_T copied = 0;
    if (readSize == 0
        || ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(start),
                             output.bytes.data(), readSize, &copied) == FALSE
        || copied == 0 || copied > output.bytes.size()) return false;
    output.start = start;
    output.end = end;
    output.copied = static_cast<std::size_t>(copied);
    return true;
}

void report_wide_function(std::uint64_t capture,
                          std::string_view role,
                          std::uintptr_t parentRva,
                          std::size_t callOffset,
                          const client::diagnostics::ModuleRange& image,
                          const WideFunctionCapture& function) noexcept {
    constexpr char kHex[] = "0123456789ABCDEF";
    for (std::size_t offset = 0; offset < function.copied; offset += kWideChunkBytes) {
        const std::size_t remaining = function.copied - offset;
        const std::size_t count = remaining < kWideChunkBytes ? remaining : kWideChunkBytes;
        std::array<char, kWideChunkBytes * 2 + 1> hex{};
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint8_t value = std::to_integer<std::uint8_t>(
                function.bytes[offset + index]);
            hex[index * 2] = kHex[value >> 4];
            hex[index * 2 + 1] = kHex[value & 0x0F];
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(), line.size(),
            "ev=sobject_code_capture capture=%llu role=%.*s function_rva=0x%llX "
            "function_end_rva=0x%llX function_size=%llu captured=%zu parent_rva=0x%llX "
            "call_offset=0x%zX offset=%zu bytes=%zu hex=%s",
            static_cast<unsigned long long>(capture),
            static_cast<int>(role.size()), role.data(),
            static_cast<unsigned long long>(function.start - image.base),
            static_cast<unsigned long long>(function.end - image.base),
            static_cast<unsigned long long>(function.end - function.start),
            function.copied,
            static_cast<unsigned long long>(parentRva),
            callOffset, offset, count, hex.data());
        if (written > 0) {
            const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                           ? static_cast<std::size_t>(written)
                                           : line.size() - 1;
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), length});
        }
    }
}

void capture_sobject_failure(std::int32_t siteId,
                             std::string_view text,
                             const void* callerAddress) noexcept;

/**
 * Writes one captured line.
 * @param siteId Registered site id.
 * @param text Borrowed native buffer.
 */
void capture_line(std::int32_t siteId,
                  const char* text,
                  const void* callerAddress) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) {
        return;
    }
    std::array<char, kNativeTextSize> sanitized{};
    const std::size_t textLength = sanitize(text, sanitized);
    capture_sobject_failure(
        siteId, std::string_view(sanitized.data(), textLength), callerAddress);
    std::array<char, kEventCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=retail site=%d text=%.*s",
                                      siteId,
                                      static_cast<int>(textLength),
                                      sanitized.data());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
}

void capture_sobject_failure(std::int32_t siteId,
                             std::string_view text,
                             const void* callerAddress) noexcept {
    if (callerAddress == nullptr || text != kSobjectFailure) return;
    const auto request = squad_reference_probe::runtime_snapshot();
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t age = request.lastBuildTick != 0 && now >= request.lastBuildTick
                                  ? now - request.lastBuildTick
                                  : ~std::uint64_t{};
    const bool recentBuild = age <= kRecentBuildWindowMs;
    volatile LONG* const counter = recentBuild ? &g_recentSobjectFailureCount
                                               : &g_sobjectFailureCount;
    const std::uint32_t limit = recentBuild ? kRecentFailureLimit : kBackgroundFailureLimit;
    const LONG ticket = InterlockedIncrement(counter) - 1;
    if (ticket < 0 || static_cast<std::uint32_t>(ticket) >= limit) return;
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(callerAddress);
    client::diagnostics::ModuleRange image{};
    const bool imageValid = client::diagnostics::module_range(GetModuleHandleW(nullptr), image);
    const bool callerInImage = imageValid && client::diagnostics::contains(image, caller);
    const bool callerExecutable = callerInImage && executable_address(caller);
    const std::uintptr_t callerRva = callerExecutable ? caller - image.base : 0;
    DWORD64 functionImageBase = 0;
    const PRUNTIME_FUNCTION function = callerExecutable
                                           ? RtlLookupFunctionEntry(
                                                 caller, &functionImageBase, nullptr)
                                           : nullptr;
    const bool functionInImage = function != nullptr && functionImageBase == image.base;
    const std::uintptr_t functionStart = functionInImage
                                             ? image.base + function->BeginAddress
                                             : 0;
    const std::uintptr_t functionEnd = functionInImage
                                           ? image.base + function->EndAddress
                                           : 0;
    const bool functionExecutable = functionInImage && functionStart < functionEnd
                                    && client::diagnostics::contains(image, functionStart)
                                    && functionEnd <= image.end
                                    && executable_address(functionStart);
    const std::uintptr_t functionRva = functionExecutable ? functionStart - image.base : 0;
    const std::uintptr_t functionEndRva = functionExecutable ? functionEnd - image.base : 0;

    std::array<std::byte, kCallerCodeBytes> code{};
    SIZE_T copied = 0;
    if (!callerExecutable || caller < image.base + 16 || caller + 16 > image.end
        || ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(caller - 16),
                             code.data(), code.size(), &copied) == FALSE) {
        copied = 0;
    }
    std::array<std::byte, kFunctionCodeBytes> functionCode{};
    SIZE_T functionCopied = 0;
    const std::size_t functionAvailable = functionExecutable
                                              ? static_cast<std::size_t>(
                                                    functionEnd - functionStart)
                                              : 0;
    const std::size_t functionReadSize = functionAvailable < functionCode.size()
                                             ? functionAvailable
                                             : functionCode.size();
    if (functionReadSize == 0
        || ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(functionStart),
                             functionCode.data(), functionReadSize,
                             &functionCopied) == FALSE) {
        functionCopied = 0;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::array<char, kCallerCodeBytes * 2 + 1> codeHex{};
    for (std::size_t index = 0; index < static_cast<std::size_t>(copied); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(code[index]);
        codeHex[index * 2] = kHex[value >> 4];
        codeHex[index * 2 + 1] = kHex[value & 0x0F];
    }
    std::array<char, kFunctionCodeBytes * 2 + 1> functionHex{};
    for (std::size_t index = 0; index < static_cast<std::size_t>(functionCopied); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(functionCode[index]);
        functionHex[index * 2] = kHex[value >> 4];
        functionHex[index * 2 + 1] = kHex[value & 0x0F];
    }

    std::uintptr_t lowerStart = 0;
    std::uintptr_t lowerEnd = 0;
    std::size_t lowerCallOffset = 0;
    for (std::size_t index = 0; index + 13 <= static_cast<std::size_t>(functionCopied); ++index) {
        if (functionCode[index] != std::byte{0xE8}
            || functionCode[index + 5] != std::byte{0x8B}
            || functionCode[index + 6] != std::byte{0x08}
            || functionCode[index + 7] != std::byte{0x41}
            || functionCode[index + 8] != std::byte{0x89}
            || functionCode[index + 9] != std::byte{0x0E}
            || functionCode[index + 10] != std::byte{0x83}
            || functionCode[index + 11] != std::byte{0xF9}
            || functionCode[index + 12] != std::byte{0xFF}) continue;
        std::int32_t displacement = 0;
        std::memcpy(&displacement, functionCode.data() + index + 1, sizeof displacement);
        const std::uintptr_t returnAddress = functionStart + index + 5;
        const std::uintptr_t candidate = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(returnAddress) + displacement);
        if (!client::diagnostics::contains(image, candidate) || !executable_address(candidate)) {
            continue;
        }
        DWORD64 lowerImageBase = 0;
        const PRUNTIME_FUNCTION lower = RtlLookupFunctionEntry(
            candidate, &lowerImageBase, nullptr);
        if (lower == nullptr || lowerImageBase != image.base
            || image.base + lower->BeginAddress != candidate) continue;
        lowerStart = candidate;
        lowerEnd = image.base + lower->EndAddress;
        lowerCallOffset = index;
        break;
    }
    std::array<std::byte, kLowerCodeBytes> lowerCode{};
    SIZE_T lowerCopied = 0;
    if (lowerStart != 0 && lowerStart < lowerEnd && lowerEnd <= image.end) {
        const std::size_t available = static_cast<std::size_t>(lowerEnd - lowerStart);
        const std::size_t readSize = available < lowerCode.size() ? available : lowerCode.size();
        if (readSize == 0
            || ReadProcessMemory(GetCurrentProcess(),
                                 reinterpret_cast<const void*>(lowerStart),
                                 lowerCode.data(), readSize, &lowerCopied) == FALSE) {
            lowerCopied = 0;
        }
    }
    std::array<char, kLowerCodeBytes * 2 + 1> lowerHex{};
    for (std::size_t index = 0; index < static_cast<std::size_t>(lowerCopied); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(lowerCode[index]);
        lowerHex[index * 2] = kHex[value >> 4];
        lowerHex[index * 2 + 1] = kHex[value & 0x0F];
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(),
        "ev=sobject_create_probe stage=retail_failure site=%d caller_site_rva=0x%llX "
        "caller_bytes=%zu caller_code=%s function_rva=0x%llX function_end_rva=0x%llX "
        "function_bytes=%zu function_code=%s recent_build=%u build_age_ms=%llu "
        "build_instance=0x%llX build_result=0x%llX produced=%d members=%d requested=%d,%d",
        siteId,
        static_cast<unsigned long long>(callerRva),
        static_cast<std::size_t>(copied), codeHex.data(),
        static_cast<unsigned long long>(functionRva),
        static_cast<unsigned long long>(functionEndRva),
        static_cast<std::size_t>(functionCopied), functionHex.data(),
        recentBuild ? 1U : 0U,
        static_cast<unsigned long long>(age),
        static_cast<unsigned long long>(request.lastBuildInstance),
        static_cast<unsigned long long>(request.lastBuildResult),
        request.lastBuildProduced, request.lastBuildMemberCount,
        request.lastBuildFirst, request.lastBuildSecond);
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
    if (lowerCopied != 0) {
        std::array<char, core::log::kLineCapacity> lowerLine{};
        const int lowerWritten = std::snprintf(
            lowerLine.data(), lowerLine.size(),
            "ev=sobject_create_probe stage=lower_function parent_rva=0x%llX "
            "lower_rva=0x%llX lower_end_rva=0x%llX lower_bytes=%zu lower_code=%s "
            "recent_build=%u build_age_ms=%llu build_instance=0x%llX",
            static_cast<unsigned long long>(functionRva),
            static_cast<unsigned long long>(lowerStart - image.base),
            static_cast<unsigned long long>(lowerEnd - image.base),
            static_cast<std::size_t>(lowerCopied), lowerHex.data(),
            recentBuild ? 1U : 0U,
            static_cast<unsigned long long>(age),
            static_cast<unsigned long long>(request.lastBuildInstance));
        if (lowerWritten > 0) {
            const std::size_t length = static_cast<std::size_t>(lowerWritten) < lowerLine.size()
                                           ? static_cast<std::size_t>(lowerWritten)
                                           : lowerLine.size() - 1;
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {lowerLine.data(), length});
        }
    }

    if (functionExecutable
        && InterlockedCompareExchange(&g_wideCaptureState, 1, 0) == 0) {
        const std::uint64_t capture = static_cast<std::uint64_t>(
            InterlockedIncrement64(&g_codeCaptureId));
        WideFunctionCapture wrapperWide{};
        if (read_wide_function(image, functionStart, functionEnd, wrapperWide)) {
            report_wide_function(capture, "wrapper", 0, 0, image, wrapperWide);
        }
        WideFunctionCapture lowerWide{};
        if (read_wide_function(image, lowerStart, lowerEnd, lowerWide)) {
            report_wide_function(capture,
                                 "lower",
                                 functionRva,
                                 lowerCallOffset,
                                 image,
                                 lowerWide);
            std::array<std::uintptr_t, kWideCalleeLimit> callees{};
            std::size_t calleeCount = 0;
            for (std::size_t index = 0;
                 index + 5 <= lowerWide.copied && calleeCount < callees.size();
                 ++index) {
                if (lowerWide.bytes[index] != std::byte{0xE8}) continue;
                std::int32_t displacement = 0;
                std::memcpy(&displacement,
                            lowerWide.bytes.data() + index + 1,
                            sizeof displacement);
                const std::uintptr_t returnAddress = lowerWide.start + index + 5;
                const std::uintptr_t candidate = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(returnAddress) + displacement);
                if (candidate == functionStart || candidate == lowerStart
                    || !client::diagnostics::contains(image, candidate)
                    || !executable_address(candidate)) continue;
                DWORD64 calleeImageBase = 0;
                const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(
                    candidate, &calleeImageBase, nullptr);
                if (entry == nullptr || calleeImageBase != image.base
                    || image.base + entry->BeginAddress != candidate) continue;
                bool duplicate = false;
                for (std::size_t seen = 0; seen < calleeCount; ++seen) {
                    if (callees[seen] == candidate) duplicate = true;
                }
                if (duplicate) continue;
                const std::uintptr_t calleeEnd = image.base + entry->EndAddress;
                WideFunctionCapture callee{};
                if (!read_wide_function(image, candidate, calleeEnd, callee)) continue;
                callees[calleeCount++] = candidate;
                report_wide_function(capture,
                                     "callee_candidate",
                                     lowerWide.start - image.base,
                                     index,
                                     image,
                                     callee);
            }
        }
        InterlockedExchange(&g_wideCaptureState, 2);
    }
}

/**
 * Mirrors the single funnel every retail log line passes through.
 * @param siteId Registered site id.
 * @param text Native buffer holding the already-formatted line.
 */
__declspec(noinline) void __fastcall enqueue_body(std::int32_t siteId, const char* text) noexcept {
    // The verbosity setter logs through this same funnel; without this it would recurse.
    const bool outer = !g_inObserver;
    const void* const callerAddress = outer ? _ReturnAddress() : nullptr;
    g_inObserver = true;
    const auto call = reinterpret_cast<Enqueue>(g_handle.original);
    if (call != nullptr) {
        call(siteId, text);
    }
    if (outer) {
        if (siteId != kUnregisteredSite && text != nullptr) {
            capture_line(siteId, text, callerAddress);
        }
        assert_verbosity();
        g_inObserver = false;
    }
}

} // namespace

/** @return The enqueue observer body itself, with internal linkage. */
void* enqueue_entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_body);
}

std::uint64_t rearm_sobject_capture() noexcept {
    LONG state = InterlockedCompareExchange(&g_wideCaptureState, 0, 0);
    for (;;) {
        if (state == 1 || state == 3) return 0;
        const LONG observed = InterlockedCompareExchange(&g_wideCaptureState, 3, state);
        if (observed == state) break;
        state = observed;
    }
    InterlockedExchange(&g_sobjectFailureCount, 0);
    InterlockedExchange(&g_recentSobjectFailureCount, 0);
    const std::uint64_t generation = static_cast<std::uint64_t>(
        InterlockedIncrement64(&g_wideCaptureGeneration));
    InterlockedExchange(&g_wideCaptureState, 0);
    return generation;
}

SobjectCaptureStatus sobject_capture_status() noexcept {
    for (std::uint32_t attempt = 0; attempt < 4; ++attempt) {
        const LONG stateBefore = InterlockedCompareExchange(&g_wideCaptureState, 0, 0);
        if (stateBefore == 3) {
            SobjectCaptureStatus busy{};
            busy.wideRearming = true;
            return busy;
        }
        const LONG64 generationBefore =
            InterlockedCompareExchange64(&g_wideCaptureGeneration, 0, 0);
        const LONG background = InterlockedCompareExchange(&g_sobjectFailureCount, 0, 0);
        const LONG recent = InterlockedCompareExchange(&g_recentSobjectFailureCount, 0, 0);
        const LONG stateAfter = InterlockedCompareExchange(&g_wideCaptureState, 0, 0);
        const LONG64 generationAfter =
            InterlockedCompareExchange64(&g_wideCaptureGeneration, 0, 0);
        if (stateBefore != stateAfter || generationBefore != generationAfter
            || stateAfter == 3) continue;
        SobjectCaptureStatus output{};
        output.generation = static_cast<std::uint64_t>(generationAfter);
        output.backgroundFailures = static_cast<std::uint32_t>(background);
        output.recentFailures = static_cast<std::uint32_t>(recent);
        output.wideArmed = stateAfter == 0;
        output.wideCapturing = stateAfter == 1;
        return output;
    }
    SobjectCaptureStatus busy{};
    busy.wideRearming = true;
    return busy;
}

std::uint64_t capture_sobject_function(std::uintptr_t rva) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) return 0;
    client::diagnostics::ModuleRange image{};
    if (!client::diagnostics::module_range(GetModuleHandleW(nullptr), image)
        || rva >= image.end - image.base) return 0;
    const std::uintptr_t address = image.base + rva;
    if (!client::diagnostics::contains(image, address) || !executable_address(address)) return 0;

    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    std::string_view role = "manual";
    DWORD64 functionImageBase = 0;
    const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
        address, &functionImageBase, nullptr);
    if (function != nullptr && functionImageBase == image.base) {
        start = image.base + function->BeginAddress;
        end = image.base + function->EndAddress;
    } else {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof region)
            != sizeof region) return 0;
        const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::uintptr_t maximum = (std::numeric_limits<std::uintptr_t>::max)();
        const std::uintptr_t regionSize = static_cast<std::uintptr_t>(region.RegionSize);
        const std::uintptr_t regionEnd = regionSize <= maximum - regionStart
                                             ? regionStart + regionSize
                                             : maximum;
        const std::uintptr_t requestedStart = address - image.base >= 64
                                                  ? address - 64
                                                  : image.base;
        const std::uintptr_t requestedEnd = image.end - address >= 192
                                                ? address + 192
                                                : image.end;
        start = (std::max)(requestedStart, (std::max)(regionStart, image.base));
        end = (std::min)(requestedEnd, (std::min)(regionEnd, image.end));
        role = "manual_raw";
    }
    WideFunctionCapture captured{};
    if (!read_wide_function(image, start, end, captured)) return 0;
    const std::uint64_t generation = static_cast<std::uint64_t>(
        InterlockedIncrement64(&g_codeCaptureId));
    report_wide_function(generation, role, rva, 0, image, captured);
    return generation;
}

/**
 * Opens every category in the game's own log table, once we know the block exists. Reaching the
 * enqueue funnel is the proof: without the block the native body returns early.
 */
void assert_verbosity() noexcept {
    // How much the game logs follows the client threshold, so debug is what opens its table.
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)) {
        return;
    }
    const auto now = static_cast<LONG64>(GetTickCount64());
    const LONG64 due = g_nextAssertTick;
    if (now < due) {
        return;
    }
    // One claim per period, so concurrent funnel threads do not all reopen the table.
    if (InterlockedCompareExchange64(
            &g_nextAssertTick, now + static_cast<LONG64>(kReassertIntervalMs), due)
        != due) {
        return;
    }
    const auto setter = reinterpret_cast<SetCategoryVerbosity>(
        targets::game::retail_log::get().setCategoryVerbosity);
    if (setter == nullptr) {
        return;
    }
    for (std::uint32_t category = 0; category < kCategoryCount; ++category) {
        setter(static_cast<std::int32_t>(category), kMostVerbose);
    }
}

} // namespace sunrise::client::hooks::retail_log
