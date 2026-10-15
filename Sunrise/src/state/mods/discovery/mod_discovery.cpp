#include "mod_discovery.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <span>

#include "../../../core/filesystem/path.h"
#include "../manifest/mod_manifest_parser.h"
#include "../problem/mod_problem.h"
#include "mod_directory_listing.h"

namespace sunrise::state::mods::discovery {
namespace {

/** A manifest is small text; a larger file is refused rather than read. */
constexpr std::size_t kManifestFileLimit = 256 * 1024;
/** A leading dot marks a staging folder that an installer is still writing. */
constexpr wchar_t kStagingPrefix = L'.';
/** Lists every entry of one directory. */
constexpr std::wstring_view kWildcard = L"\\*";
/** Separates the mod folder from its manifest. */
constexpr std::wstring_view kSeparator = L"\\";

/** One listed folder name, kept until the list is sorted. */
struct FolderName final {
    std::array<wchar_t, kFolderCapacity> chars{};
    std::size_t length{};
    /** True when the real name did not fit and `chars` holds its start. */
    bool truncated{};

    [[nodiscard]] std::wstring_view view() const noexcept {
        return {chars.data(), length};
    }
};

/** @return True when `first` sorts before `second`, by ordinal character value. */
[[nodiscard]] bool name_before(const FolderName& first, const FolderName& second) noexcept {
    return first.view() < second.view();
}

/** How reading one manifest file ended. */
enum class ReadStatus : std::uint8_t {
    ok,
    missing,
    unreadable,
    tooLarge,
};

/** Boot reads one manifest at a time on one thread, so one buffer serves them all. */
std::array<char, kManifestFileLimit> g_manifestText{};
std::array<FolderName, kModCapacity> g_folders{};

/** @return True for a character the client's ASCII package path can carry. */
[[nodiscard]] bool folder_character(wchar_t value) noexcept {
    return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z')
           || (value >= L'0' && value <= L'9') || value == L'.' || value == L'-' || value == L'_';
}

/**
 * Copies a folder name for display. Characters outside the allowed set show as '?'.
 * @return True when every character is allowed and the whole name fits.
 */
[[nodiscard]] bool copy_folder(const FolderName& name, Text<kFolderCapacity>& output) noexcept {
    output = {};
    bool valid = !name.truncated && name.length != 0 && name.length < output.bytes.size();
    output.length = (std::min)(name.length, output.bytes.size() - 1);
    for (std::size_t index = 0; index < output.length; ++index) {
        const wchar_t value = name.chars[index];
        valid = valid && folder_character(value);
        output.bytes[index] = folder_character(value) ? static_cast<char>(value) : '?';
    }
    return valid;
}

/**
 * Reads one whole manifest into the shared buffer.
 * @param path Manifest file path.
 * @param text Receives the file text.
 * @return How the read ended.
 */
[[nodiscard]] ReadStatus read_manifest(const core::path::Buffer& path,
                                       std::string_view& text) noexcept {
    text = {};
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? ReadStatus::missing
                   : ReadStatus::unreadable;
    }
    LARGE_INTEGER size{};
    ReadStatus status = ReadStatus::unreadable;
    if (GetFileSizeEx(file, &size) != FALSE) {
        if (size.QuadPart > static_cast<LONGLONG>(g_manifestText.size())) {
            status = ReadStatus::tooLarge;
        } else {
            DWORD read = 0;
            const auto requested = static_cast<DWORD>(size.QuadPart);
            if (ReadFile(file, g_manifestText.data(), requested, &read, nullptr) != FALSE
                && read == requested) {
                text = {g_manifestText.data(), read};
                status = ReadStatus::ok;
            }
        }
    }
    CloseHandle(file);
    return status;
}

/**
 * Reads one folder's manifest into its record.
 * @param modsDirectory Absolute mods directory.
 * @param name Listed folder name.
 * @param record Cleared record receiving the outcome.
 */
void read_record(std::wstring_view modsDirectory, const FolderName& name, Record& record) noexcept {
    record = {};
    if (!copy_folder(name, record.folder)) {
        problem::refuse(record,
                        Reason::folderPath,
                        "The folder name may only use letters, digits, '.', '-' and '_', and at "
                        "most %zu characters.",
                        kFolderCapacity - 1);
        return;
    }
    core::path::Buffer path;
    if (!core::path::assign(path, modsDirectory) || !core::path::append(path, kSeparator)
        || !core::path::append(path, name.view()) || !core::path::append(path, kSeparator)
        || !core::path::append(path, kManifestFileName)) {
        problem::refuse(record, Reason::folderPath, "The folder path is too long.");
        return;
    }
    std::string_view text;
    switch (read_manifest(path, text)) {
    case ReadStatus::ok:
        break;
    case ReadStatus::missing:
        problem::refuse(record, Reason::manifestMissing, "No mod.json in this folder.");
        return;
    case ReadStatus::tooLarge:
        problem::refuse(record,
                        Reason::manifestUnreadable,
                        "mod.json is larger than %zu bytes.",
                        kManifestFileLimit);
        return;
    case ReadStatus::unreadable:
        problem::refuse(record, Reason::manifestUnreadable, "mod.json could not be read.");
        return;
    }
    std::uint32_t version = 0;
    if (!manifest::read_version(text, version)) {
        problem::refuse(record, Reason::manifestInvalid, "mod.json is not one JSON object.");
        return;
    }
    if (version > kManifestVersion) {
        problem::refuse(record,
                        Reason::manifestVersion,
                        "Needs a newer Sunrise: manifest version %u, this build reads up to %u.",
                        version,
                        kManifestVersion);
        return;
    }
    manifest::Error error{};
    if (!manifest::parse(text, record.manifest, error)) {
        problem::refuse(record, Reason::manifestInvalid, "mod.json: %s", error.bytes.data());
        return;
    }
    record.manifestRead = true;
    record.status = Status::loaded;
}

} // namespace

/** Lists mod folders, sorted by name, and reads each `mod.json`. */
Result discover(std::wstring_view modsDirectory, std::span<Record> records) noexcept {
    Result result{};
    core::path::Buffer search;
    if (!core::path::assign(search, modsDirectory) || !core::path::append(search, kWildcard)) {
        return result;
    }
    WIN32_FIND_DATAW entry{};
    const HANDLE listing = FindFirstFileW(search.chars.data(), &entry);
    if (listing == INVALID_HANDLE_VALUE) {
        result.complete = directory_listing::empty(modsDirectory, GetLastError());
        return result;
    }
    directory_listing::Cursor cursor(listing);
    std::size_t folders = 0;
    const std::size_t capacity = (std::min)(records.size(), g_folders.size());
    do {
        const std::wstring_view name(entry.cFileName);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name.empty()
            || name.front() == kStagingPrefix) {
            continue;
        }
        FolderName candidate{};
        candidate.truncated = name.size() >= candidate.chars.size();
        candidate.length = (std::min)(name.size(), candidate.chars.size() - 1);
        std::copy_n(name.begin(), candidate.length, candidate.chars.begin());
        if (folders < capacity) {
            g_folders[folders++] = candidate;
            continue;
        }
        // Over capacity, keep the lowest names, so the listing order does not pick the mods.
        ++result.skipped;
        const auto kept = std::span(g_folders).first(capacity);
        const auto largest = std::max_element(kept.begin(), kept.end(), name_before);
        if (largest != kept.end() && name_before(candidate, *largest)) {
            *largest = candidate;
        }
    } while (FindNextFileW(listing, &entry) != FALSE);
    if (!cursor.finish(GetLastError())) {
        return result;
    }

    std::sort(
        g_folders.begin(), g_folders.begin() + static_cast<std::ptrdiff_t>(folders), name_before);
    for (std::size_t index = 0; index < folders; ++index) {
        read_record(modsDirectory, g_folders[index], records[index]);
    }
    result.count = folders;
    result.complete = true;
    return result;
}

} // namespace sunrise::state::mods::discovery
