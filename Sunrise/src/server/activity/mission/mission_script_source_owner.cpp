#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../state/mods/mods_runtime.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

/** UTF-8 uses at most four bytes for each Windows path character. */
constexpr std::size_t kUtf8PathCapacity = core::path::kExtendedPathCapacity * 4;
using core::path::kDevicePrefix;
using core::path::kExtendedPrefix;
using core::path::kExtendedUncPrefix;

/** @return Whether two Windows path fragments have the same ordinal spelling. */
[[nodiscard]] bool equal_path(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size()
           && CompareStringOrdinal(left.data(),
                                   static_cast<int>(left.size()),
                                   right.data(),
                                   static_cast<int>(right.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

/** @return Whether an absolute path starts with one namespace prefix. */
[[nodiscard]] bool starts_with(std::wstring_view path, std::wstring_view prefix) noexcept {
    return path.size() >= prefix.size() && equal_path(path.substr(0, prefix.size()), prefix);
}

/** @return Whether a component has no wildcard syntax or unextended Windows alias. */
[[nodiscard]] bool valid_component(std::wstring_view value, bool extended) noexcept {
    if (value.empty() || (!extended && (value.back() == L'.' || value.back() == L' '))) {
        return false;
    }
    for (const wchar_t character : value) {
        if (character < L' '
            || std::wstring_view(L":<>\"|?*").find(character) != std::wstring_view::npos) {
            return false;
        }
    }
    return true;
}

/**
 * Collapses directory segments without consulting the process working directory or filesystem.
 * @param input Absolute path with backslash separators.
 * @param output Receives one normalized drive or UNC path.
 * @return False for an invalid, relative or oversized path.
 */
[[nodiscard]] bool normalize_path(std::wstring_view input, core::path::Buffer& output) noexcept {
    output = {};
    bool unc = false;
    const bool extended = starts_with(input, kExtendedPrefix);
    if (starts_with(input, kExtendedUncPrefix)) {
        input.remove_prefix(kExtendedUncPrefix.size());
        unc = true;
    } else if (starts_with(input, kExtendedPrefix)) {
        input.remove_prefix(kExtendedPrefix.size());
    } else if (starts_with(input, kDevicePrefix)) {
        // Device namespace paths are not Lua search roots.
        return false;
    } else if (input.starts_with(L"\\\\")) {
        input.remove_prefix(2);
        unc = true;
    }
    std::size_t position = 0;
    if (unc) {
        const std::size_t serverEnd = input.find(L'\\');
        if (serverEnd == std::wstring_view::npos) {
            return false;
        }
        const std::size_t shareEnd = input.find(L'\\', serverEnd + 1);
        position = shareEnd == std::wstring_view::npos ? input.size() : shareEnd;
        if (!valid_component(input.substr(0, serverEnd), extended)
            || !valid_component(input.substr(serverEnd + 1, position - serverEnd - 1), extended)
            || !core::path::append(output, L"\\\\")
            || !core::path::append(output, input.substr(0, position))
            || !core::path::append(output, L"\\")) {
            return false;
        }
    } else {
        const bool drive =
            input.size() >= 3 && input[1] == L':' && input[2] == L'\\'
            && ((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z'));
        if (!drive || !core::path::append(output, input.substr(0, 3))) {
            return false;
        }
        position = 3;
    }
    const std::size_t rootLength = output.length;
    while (position < input.size()) {
        if (input[position] == L'\\') {
            ++position;
            continue;
        }
        const std::size_t separator = input.find(L'\\', position);
        const std::size_t end = separator == std::wstring_view::npos ? input.size() : separator;
        const std::wstring_view component = input.substr(position, end - position);
        position = end;
        // Extended paths bypass Win32 parsing, so these spellings must not be rewritten.
        if (extended && (component == L"." || component == L"..")) {
            return false;
        }
        if (component == L".") {
            continue;
        }
        if (component == L"..") {
            if (output.length == rootLength) {
                return false;
            }
            while (output.length > rootLength && output.chars[output.length - 1] != L'\\') {
                --output.length;
            }
            if (output.length > rootLength) {
                --output.length;
            }
            output.chars[output.length] = L'\0';
            continue;
        }
        if (!valid_component(component, extended)
            || (output.chars[output.length - 1] != L'\\' && !core::path::append(output, L"\\"))
            || !core::path::append(output, component)) {
            return false;
        }
    }
    return true;
}

/** Attribution storage belongs to the caller while the mod list is borrowed. */
struct SourceOwnerLookup final {
    std::wstring_view source;
    state::mods::Id owner{};
    bool valid{true};
};

/**
 * Compares one enabled search root with the loaded filename.
 * @param lookup Holds the normalized filename and any prior owner.
 * @param root Directory built with the loader's mod folder rules.
 * @param record Loaded mod that owns the directory.
 * @return False if the root is invalid or ownership is ambiguous.
 */
[[nodiscard]] bool match_root(SourceOwnerLookup& lookup,
                              std::wstring_view root,
                              const state::mods::Record& record) noexcept {
    core::path::Buffer normalized{};
    if (!normalize_path(root, normalized) || !core::path::append(normalized, L"\\")) {
        return false;
    }
    if (lookup.source.size() <= normalized.length
        || !equal_path(lookup.source.substr(0, normalized.length),
                       {normalized.chars.data(), normalized.length})) {
        return true;
    }
    const state::mods::Id& id = record.manifest.id;
    if (id.length == 0 || id.length >= id.bytes.size()
        || (lookup.owner.length != 0 && lookup.owner.view() != id.view())) {
        return false;
    }
    lookup.owner = id;
    return true;
}

/**
 * Resolves only roots published by the native Lua loader.
 * @param opaque Caller-owned attribution storage.
 * @param view Loaded mods in their published order.
 * @return False if attribution could not be completed safely.
 */
[[nodiscard]] bool find_owner(void* opaque, const state::mods::View& view) noexcept {
    auto& lookup = *static_cast<SourceOwnerLookup*>(opaque);
    core::path::Buffer root{};
    for (std::size_t index = 0; index < view.loadedCount; ++index) {
        const state::mods::Record& record = view.records[index];
        if (record.status != state::mods::Status::loaded
            || (!record.hasScripts && !record.hasSdk)) {
            continue;
        }
        root = {};
        if (!append_mod_directory(root, view, record) || !core::path::append(root, L"\\")) {
            lookup.valid = false;
            return false;
        }
        const std::span rootCharacters(root.chars.data(), root.length);
        if (starts_with({root.chars.data(), root.length}, kExtendedPrefix)
            && std::find(rootCharacters.begin(), rootCharacters.end(), L'/')
                   != rootCharacters.end()) {
            lookup.valid = false;
            return false;
        }
        std::replace(rootCharacters.begin(), rootCharacters.end(), L'/', L'\\');
        const std::size_t directoryLength = root.length;
        for (const std::wstring_view leaf :
             {state::mods::kSdkFolder, state::mods::kScriptsFolder}) {
            if ((leaf == state::mods::kSdkFolder && !record.hasSdk)
                || (leaf == state::mods::kScriptsFolder && !record.hasScripts)) {
                continue;
            }
            root.length = directoryLength;
            root.chars[root.length] = L'\0';
            if (!core::path::append(root, leaf)
                || !match_root(lookup, {root.chars.data(), root.length}, record)) {
                lookup.valid = false;
                return false;
            }
        }
    }
    return true;
}

} // namespace

/**
 * Attributes a loaded Lua filename to its mod without guessing from its module name.
 * @param loadedPath Absolute UTF-8 filename returned by the Lua file searcher.
 * @param modId Receives the manifest id, or an empty string for an unowned source.
 * @return False for malformed paths, ambiguous ownership or insufficient output storage.
 */
bool module_source_owner(std::string_view loadedPath, std::span<char> modId) noexcept {
    if (modId.empty()) {
        return false;
    }
    modId[0] = '\0';
    if (loadedPath.empty() || loadedPath.size() >= kUtf8PathCapacity
        || loadedPath.find('\0') != std::string_view::npos) {
        return false;
    }
    core::path::Buffer source{};
    core::path::Buffer normalized{};
    const int characters = MultiByteToWideChar(CP_UTF8,
                                               MB_ERR_INVALID_CHARS,
                                               loadedPath.data(),
                                               static_cast<int>(loadedPath.size()),
                                               source.chars.data(),
                                               static_cast<int>(source.chars.size() - 1));
    if (characters <= 0) {
        return false;
    }
    source.length = static_cast<std::size_t>(characters);
    if (starts_with({source.chars.data(), source.length}, kExtendedPrefix)
        && loadedPath.find('/') != std::string_view::npos) {
        return false;
    }
    const std::span sourceCharacters(source.chars.data(), source.length);
    std::replace(sourceCharacters.begin(), sourceCharacters.end(), L'/', L'\\');
    if (!normalize_path({source.chars.data(), source.length}, normalized)) {
        return false;
    }
    SourceOwnerLookup lookup{{normalized.chars.data(), normalized.length}};
    (void)state::mods::visit(&find_owner, &lookup);
    if (!lookup.valid || lookup.owner.length >= modId.size()) {
        return false;
    }
    std::copy_n(lookup.owner.bytes.begin(), lookup.owner.length, modId.begin());
    modId[lookup.owner.length] = '\0';
    return true;
}

} // namespace sunrise::server::activity::mission
