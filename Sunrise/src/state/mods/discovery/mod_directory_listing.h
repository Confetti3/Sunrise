#pragma once

#include <Windows.h>

#include <string_view>

#include "../../../core/filesystem/path.h"

namespace sunrise::state::mods::directory_listing {

/** Failed scans are empty only when the directory is missing or has no entries. */
[[nodiscard]] inline bool empty(std::wstring_view directory, DWORD error) noexcept {
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return false;
    }
    core::path::Buffer path;
    if (!core::path::assign(path, directory)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.chars.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD missing = GetLastError();
        return missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND;
    }
    return error == ERROR_FILE_NOT_FOUND && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Every acquired search closes even when row allocation throws. */
class Cursor final {
public:
    explicit Cursor(HANDLE value) noexcept : value_(value) {}
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    /** Unfinished enumeration still owns its search handle. */
    ~Cursor() noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            (void)FindClose(value_);
        }
    }

    /** A closed search is complete only after the normal end-of-directory result. */
    [[nodiscard]] bool finish(DWORD error) noexcept {
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        const bool closed = FindClose(value) != FALSE;
        return closed && error == ERROR_NO_MORE_FILES;
    }

private:
    HANDLE value_;
};

} // namespace sunrise::state::mods::directory_listing
