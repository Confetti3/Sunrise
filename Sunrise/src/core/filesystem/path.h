#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace sunrise::core::path {

/** Characters reserved for Windows extended paths. */
inline constexpr std::size_t kExtendedPathCapacity = 32768;
/** Win32 skips the MAX_PATH limit for a drive path with this prefix. */
inline constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
/** The same for a UNC path; it replaces the two leading backslashes. */
inline constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";
/** Device namespace prefix; it needs no conversion. */
inline constexpr std::wstring_view kDevicePrefix = L"\\\\.\\";

/** Fixed-size mutable Windows path. */
struct Buffer {
    std::array<wchar_t, kExtendedPathCapacity> chars{};
    std::size_t length{};
};

/** Replaces one path buffer with caller text. */
[[nodiscard]] bool assign(Buffer& path, std::wstring_view value) noexcept;

/** Resolves the directory containing one loaded module. */
[[nodiscard]] bool module_directory(void* module, Buffer& output) noexcept;

/** Resolves and creates the one Sunrise-owned generated-artifact directory. */
[[nodiscard]] bool artifact_directory(void* module, Buffer& output) noexcept;

/**
 * Resolves one Sunrise-owned file beside this DLL, creating any directory it needs.
 * The path is `<directory holding steam_api64.dll>\Sunrise\<relative>`.
 * The module is found from this function's own address, so no caller threads a handle down.
 * @param relative File name, optionally with one leading subdirectory such as `exports\x.txt`.
 * @param output Receives the full path.
 * @return True when the path fits and every directory in it exists or was created.
 */
[[nodiscard]] bool artifact_file(std::wstring_view relative, Buffer& output) noexcept;

/** Appends one suffix without exceeding fixed path storage. */
[[nodiscard]] bool append(Buffer& path, std::wstring_view suffix) noexcept;

/**
 * Gives the full path in extended form, so Win32 file calls accept it past MAX_PATH.
 * @param path Null-terminated path; a `\\?\` or `\\.\` path is returned unchanged.
 * @return The prefixed full path, or empty when it cannot be resolved.
 */
[[nodiscard]] std::wstring extended(const wchar_t* path) noexcept;

} // namespace sunrise::core::path
