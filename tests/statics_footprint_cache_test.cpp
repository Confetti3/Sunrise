#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "client/content/statics/statics_footprint_cache.h"

namespace cache = sunrise::client::content::statics::cache;
namespace statics = sunrise::client::content::statics;

namespace {

bool check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}

cache::Key key() {
    cache::Key result{};
    for (std::size_t index = 0; index < result.contentFingerprint.size(); ++index) {
        result.contentFingerprint[index] = static_cast<std::byte>(index + 1U);
    }
    result.scenarioTag = 0x81234567U;
    result.mapFamily = "edz";
    result.packageIds[0] = 0x123U;
    result.packageIds[1] = 0x456U;
    result.packageCount = 2;
    return result;
}

std::array<statics::Footprint, 2> rows() {
    return {{{0x80001001U, {-4.0F, -3.0F, -2.0F}, {1.0F, 2.0F, 3.0F}, 12, 2},
             {0x80001002U, {10.0F, 11.0F, 12.0F}, {13.0F, 14.0F, 15.0F}, 4, 1}}};
}

} // namespace

int main() {
    std::array<wchar_t, MAX_PATH> temporaryRoot{};
    const DWORD rootLength =
        GetTempPathW(static_cast<DWORD>(temporaryRoot.size()), temporaryRoot.data());
    if (!check(rootLength != 0 && rootLength < temporaryRoot.size(), "temp path unavailable")) {
        return 1;
    }
    std::array<wchar_t, MAX_PATH> directory{};
    std::swprintf(directory.data(),
                  directory.size(),
                  L"%ssunrise-statics-cache-%lu",
                  temporaryRoot.data(),
                  GetCurrentProcessId());
    (void)CreateDirectoryW(directory.data(), nullptr);
    const std::wstring path = std::wstring(directory.data()) + L"\\statics-footprints.bin";

    const cache::Key expected = key();
    const auto expectedRows = rows();
    statics::Progress expectedProgress{};
    expectedProgress.collections = 4;
    expectedProgress.published = 2;
    expectedProgress.rejected = 1;
    expectedProgress.truncated = 1;
    std::string diagnostic;
    bool valid = check(cache::store_atomic(path, expected, expectedRows, expectedProgress, diagnostic),
                       "valid cache write failed");

    std::vector<statics::Footprint> loadedRows;
    statics::Progress loadedProgress{};
    cache::LoadResult loaded = cache::load(path, expected, loadedRows, loadedProgress);
    valid = check(loaded.state == cache::LoadState::ready, "valid cache was rejected") && valid;
    valid = check(loadedRows.size() == expectedRows.size(), "row count did not round trip") && valid;
    valid = check(loadedRows.size() == 2 && loadedRows[1].tag == expectedRows[1].tag
                      && loadedRows[0].minimum == expectedRows[0].minimum
                      && loadedProgress.collections == expectedProgress.collections
                      && loadedProgress.rejected == expectedProgress.rejected,
                  "cache payload did not round trip")
            && valid;

    cache::Key stale = expected;
    stale.contentFingerprint[0] = std::byte{0xFF};
    loaded = cache::load(path, stale, loadedRows, loadedProgress);
    valid = check(loaded.state == cache::LoadState::rejected,
                  "stale content fingerprint was accepted")
            && valid;

    auto invalidRows = expectedRows;
    std::swap(invalidRows[0], invalidRows[1]);
    valid = check(!cache::store_atomic(path, expected, invalidRows, expectedProgress, diagnostic),
                  "unsorted rows were cached")
            && valid;
    loaded = cache::load(path, expected, loadedRows, loadedProgress);
    valid = check(loaded.state == cache::LoadState::ready,
                  "failed replacement damaged the last valid cache")
            && valid;

    const HANDLE corrupt = CreateFileW(path.c_str(),
                                       GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    if (corrupt != INVALID_HANDLE_VALUE) {
        const std::array<std::byte, 4> shortFile{};
        DWORD written = 0;
        (void)WriteFile(corrupt,
                        shortFile.data(),
                        static_cast<DWORD>(shortFile.size()),
                        &written,
                        nullptr);
        (void)CloseHandle(corrupt);
    }
    loaded = cache::load(path, expected, loadedRows, loadedProgress);
    valid = check(loaded.state == cache::LoadState::rejected, "truncated cache was accepted")
            && valid;

    (void)DeleteFileW(path.c_str());
    (void)RemoveDirectoryW(directory.data());
    return valid ? 0 : 1;
}
