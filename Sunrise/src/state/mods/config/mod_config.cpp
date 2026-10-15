/** Saved mod config values. The UI thread writes them and the server thread reads them. */

#include "mod_config.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <span>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../core/json/reader.h"
#include "../../../core/logging/log.h"
#include "../manifest/mod_manifest_parser.h"
#include "../mods_runtime.h"

namespace sunrise::state::mods::config {
namespace {

/** The values file, directly in the Sunrise folder. */
constexpr std::wstring_view kFileName = L"\\mod_config.json";
/** A refused file is moved to this name, so a save cannot overwrite it. */
constexpr std::wstring_view kBadSuffix = L".bad";
/** File format version. A file of another version is read as a bad file. */
constexpr unsigned kFileVersion = 1;
/** Root keys of the file. */
constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kModsKey = "mods";
/** Longest saved value text. A float in shortest form needs 15 bytes. */
constexpr std::size_t kValueCapacity = 32;
/** Saved values kept: every key of a full mod list. */
constexpr std::size_t kSavedLimit = kModCapacity * kConfigLimit;
/** 512 KiB holds a full table even when every value belongs to a different mod. */
constexpr std::size_t kFileCapacity = 512 * 1024;
/** Boolean values as JSON writes them. */
constexpr std::string_view kTrueText = "true";
constexpr std::string_view kFalseText = "false";
/** Appended to a whole float, so the file keeps it a float. */
constexpr std::string_view kPointZero = ".0";
/** A float in shortest form already reads as a float when it holds one of these. */
constexpr std::string_view kFloatMarks = ".e";

/** One saved value, kept as JSON text so an unloaded mod's value is written back unchanged. */
struct Saved final {
    Id modId{};
    Text<kConfigKeyCapacity> key{};
    Text<kValueCapacity> value{};
};

/** A bound value keeps the exact typed bits used by the published package overlay. */
struct Applied final {
    Id modId{};
    Text<kConfigKeyCapacity> key{};
    ConfigValue value{};
};

/** Saved values sorted by mod id, then key, and the file they are saved to. */
struct Storage final {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<Saved, kSavedLimit> values{};
    std::size_t count{};
    std::array<Applied, kSavedLimit> applied{};
    std::size_t appliedCount{};
    core::path::Buffer path{};
    /** False when saving must not touch the file. */
    bool ready{};
    /** Holds the file text while it is read or written. */
    std::array<char, kFileCapacity> file{};
};

Storage g_storage;

/** @param reason Key naming what went wrong. */
void report(const char* reason) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=mods stage=config result=fail reason=%s",
                      reason);
}

/**
 * Copies text into bounded storage and keeps a null after it.
 * @return False when the text does not fit.
 */
template <std::size_t Capacity>
[[nodiscard]] bool assign(Text<Capacity>& output, std::string_view text) noexcept {
    if (text.size() >= Capacity) {
        return false;
    }
    output = {};
    std::copy(text.begin(), text.end(), output.bytes.begin());
    output.length = text.size();
    return true;
}

/** @return True when `saved` sorts before the pair `modId`, `key`. */
[[nodiscard]] bool
before(const Saved& saved, std::string_view modId, std::string_view key) noexcept {
    const int order = saved.modId.view().compare(modId);
    return order < 0 || (order == 0 && saved.key.view() < key);
}

/** @return True when `saved` holds the pair `modId`, `key`. */
[[nodiscard]] bool same(const Saved& saved, std::string_view modId, std::string_view key) noexcept {
    return saved.modId.view() == modId && saved.key.view() == key;
}

/** @return The saved values. The caller holds the lock. */
[[nodiscard]] std::span<Saved> saved_values(Storage& storage) noexcept {
    return std::span(storage.values).first(storage.count);
}

/** @return Where the pair is, or where it would go. The caller holds the lock. */
[[nodiscard]] std::size_t
find_index(const Storage& storage, std::string_view modId, std::string_view key) noexcept {
    const auto values = std::span(storage.values).first(storage.count);
    const auto found = std::partition_point(values.begin(), values.end(), [&](const Saved& saved) {
        return before(saved, modId, key);
    });
    return static_cast<std::size_t>(found - values.begin());
}

/** @return True when index holds the pair. The caller holds the lock. */
[[nodiscard]] bool found_at(const Storage& storage,
                            std::size_t index,
                            std::string_view modId,
                            std::string_view key) noexcept {
    return index < storage.count && same(storage.values[index], modId, key);
}

/**
 * Stores one value text, replacing a saved one for the same key. Caller holds the lock
 * exclusively.
 * @return False when the table is full or a text does not fit.
 */
[[nodiscard]] bool
put(std::string_view modId, std::string_view key, std::string_view text) noexcept {
    const std::size_t index = find_index(g_storage, modId, key);
    if (found_at(g_storage, index, modId, key)) {
        return assign(g_storage.values[index].value, text);
    }
    Saved saved{};
    if (g_storage.count == g_storage.values.size() || !assign(saved.modId, modId)
        || !assign(saved.key, key) || !assign(saved.value, text)) {
        return false;
    }
    const auto first = g_storage.values.begin() + static_cast<std::ptrdiff_t>(index);
    const auto last = g_storage.values.begin() + static_cast<std::ptrdiff_t>(g_storage.count);
    std::copy_backward(first, last, last + 1);
    *first = saved;
    ++g_storage.count;
    return true;
}

/** Removes the value at `index`. The caller holds the lock exclusively. */
void erase(std::size_t index) noexcept {
    const auto first = g_storage.values.begin() + static_cast<std::ptrdiff_t>(index);
    const auto last = g_storage.values.begin() + static_cast<std::ptrdiff_t>(g_storage.count);
    std::copy(first + 1, last, first);
    --g_storage.count;
    g_storage.values[g_storage.count] = Saved{};
}

/** Empties the table. The caller holds the lock exclusively. */
void clear_values(Storage& storage) noexcept {
    storage.values.fill(Saved{});
    storage.count = 0;
    storage.appliedCount = 0;
}

/** Reads `mod_config.json` into the table. Any break in the format refuses the whole file. */
class FileParser final : private core::json::Reader {
public:
    explicit FileParser(Storage& storage, std::string_view json) noexcept
        : Reader(json), storage_(storage), json_(json) {}

    /** @return True when the whole file was read. The caller holds the lock exclusively. */
    [[nodiscard]] bool root() noexcept {
        bool versionRead = false;
        const bool read = object([&](std::string_view key) noexcept {
            if (key == kVersionKey) {
                std::uint64_t fileVersion = 0;
                versionRead = unsigned_integer(fileVersion) && fileVersion == kFileVersion;
                return versionRead;
            }
            return key == kModsKey
                   && object([&](std::string_view rawId) noexcept { return mod_values(rawId); });
        });
        return read && versionRead && at_end();
    }

private:
    /**
     * Reads one object and hands each key to `member`, after its colon.
     * @return False when the object breaks the format or `member` refuses a key.
     */
    template <typename Member> [[nodiscard]] bool object(Member&& member) noexcept {
        if (!consume('{')) {
            return false;
        }
        if (consume('}')) {
            return true;
        }
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':') || !member(key)) {
                return false;
            }
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    /** Reads the values of one mod. Its id must be a valid mod id. */
    [[nodiscard]] bool mod_values(std::string_view rawId) noexcept {
        Id modId{};
        if (!core::json::decode_string(rawId, modId.bytes, modId.length)
            || !manifest::valid_id(modId.view())) {
            return false;
        }
        return object([&](std::string_view rawKey) noexcept { return saved_value(modId, rawKey); });
    }

    /**
     * Reads one value as JSON text. Only a number or a boolean is kept.
     * @param modId Mod that owns the value.
     * @param rawKey Key as the file spells it.
     * @return False on a bad key, a value of another kind, or a full table.
     */
    [[nodiscard]] bool saved_value(const Id& modId, std::string_view rawKey) noexcept {
        Text<kConfigKeyCapacity> key{};
        if (!core::json::decode_string(rawKey, key.bytes, key.length)
            || !manifest::valid_config_key(key.view())
            || storage_.count == storage_.values.size()) {
            return false;
        }
        whitespace();
        const std::size_t start = position();
        bool flag = false;
        if (!boolean(flag) && !number()) {
            return false;
        }
        Saved& saved = storage_.values[storage_.count];
        saved = Saved{};
        const bool stored = assign(saved.modId, modId.view()) && assign(saved.key, key.view())
                            && assign(saved.value, json_.substr(start, position() - start));
        storage_.count += stored ? 1U : 0U;
        return stored;
    }

    Storage& storage_;
    std::string_view json_;
};

/**
 * Sorts the table read from the file by mod id, then key.
 * @return False when one key of one mod appears twice.
 */
[[nodiscard]] bool sort_values(Storage& storage) noexcept {
    const std::span<Saved> values = saved_values(storage);
    std::sort(values.begin(), values.end(), [](const Saved& first, const Saved& second) {
        return before(first, second.modId.view(), second.key.view());
    });
    return std::adjacent_find(values.begin(),
                              values.end(),
                              [](const Saved& first, const Saved& second) {
                                  return same(first, second.modId.view(), second.key.view());
                              })
           == values.end();
}

/**
 * Moves a refused file to its `.bad` name, so the next save cannot overwrite it.
 * @param reason Key naming why the file was refused.
 * @return False when the file stays in place. The caller holds the lock exclusively.
 */
[[nodiscard]] bool set_aside(const Storage& storage, const char* reason) noexcept {
    core::path::Buffer bad = storage.path;
    const bool moved =
        core::path::append(bad, kBadSuffix)
        && MoveFileExW(storage.path.chars.data(), bad.chars.data(), MOVEFILE_REPLACE_EXISTING)
               != FALSE;
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=mods stage=config result=fail reason=%s kept=%s",
                      reason,
                      moved ? "bad" : "original");
    return moved;
}

/**
 * Reads the file into the table. A missing or refused file leaves it empty.
 * @return False when a refused file could not be moved aside. Caller holds the lock exclusively.
 */
[[nodiscard]] bool read_file(Storage& storage) noexcept {
    const HANDLE file = CreateFileW(storage.path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // No file is the normal state until a first value is saved.
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             "ev=mods stage=config result=none");
            return true;
        }
        return set_aside(storage, "unreadable");
    }
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool complete =
        GetFileSizeEx(file, &size) != FALSE
        && size.QuadPart < static_cast<LONGLONG>(storage.file.size())
        && ReadFile(file, storage.file.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
               != FALSE
        && static_cast<LONGLONG>(read) == size.QuadPart;
    (void)CloseHandle(file);
    FileParser parser(
        storage,
        std::string_view(storage.file.data(), complete ? static_cast<std::size_t>(read) : 0));
    if (!complete || !parser.root() || !sort_values(storage)) {
        clear_values(storage);
        return set_aside(storage, "bad_file");
    }
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=mods stage=config result=ok values=%zu",
                      storage.count);
    return true;
}

/**
 * Appends formatted text to the file buffer.
 * @param used Bytes already written, advanced by the appended text.
 * @param format printf-style format.
 * @return False when the text does not fit.
 */
[[nodiscard]] bool append(std::size_t& used, const char* format, ...) noexcept {
    std::va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(
        g_storage.file.data() + used, g_storage.file.size() - used, format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<std::size_t>(written) >= g_storage.file.size() - used) {
        return false;
    }
    used += static_cast<std::size_t>(written);
    return true;
}

/**
 * Formats every saved value, grouped by mod, into the file buffer.
 * @param used Receives the byte count.
 * @return False when the text does not fit. The caller holds the lock.
 */
[[nodiscard]] bool format_file(std::size_t& used) noexcept {
    used = 0;
    bool fits = append(
        used, "{\n  \"%s\": %u,\n  \"%s\": {", kVersionKey.data(), kFileVersion, kModsKey.data());
    for (std::size_t index = 0; fits && index < g_storage.count; ++index) {
        const Saved& saved = g_storage.values[index];
        const bool firstOfMod =
            index == 0 || g_storage.values[index - 1].modId.view() != saved.modId.view();
        const bool lastOfMod = index + 1 == g_storage.count
                               || g_storage.values[index + 1].modId.view() != saved.modId.view();
        if (firstOfMod) {
            fits =
                append(used, "%s\n    \"%s\": {", index == 0 ? "" : ",", saved.modId.bytes.data());
        }
        fits = fits
               && append(used,
                         "%s\n      \"%s\": %s",
                         firstOfMod ? "" : ",",
                         saved.key.bytes.data(),
                         saved.value.bytes.data());
        if (lastOfMod) {
            fits = fits && append(used, "\n    }");
        }
    }
    return fits && append(used, "%s}\n}\n", g_storage.count == 0 ? "" : "\n  ");
}

/**
 * Publishes a flushed sibling so a failed save leaves the prior file intact.
 * @return True when every byte reached the file. The caller holds the lock exclusively.
 */
[[nodiscard]] bool save() noexcept {
    std::size_t used = 0;
    if (!format_file(used)) {
        return false;
    }
    core::path::Buffer temporary;
    if (!core::path::temporary_sibling({g_storage.path.chars.data(), g_storage.path.length},
                                       temporary)) {
        return false;
    }
    const HANDLE file = CreateFileW(temporary.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(used);
    bool complete =
        WriteFile(file, g_storage.file.data(), size, &written, nullptr) != FALSE && written == size;
    complete = complete && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    complete = complete
               && core::path::publish_sibling(temporary.chars.data(), g_storage.path.chars.data());
    if (!complete) {
        static_cast<void>(DeleteFileW(temporary.chars.data()));
    }
    return complete;
}

/**
 * Writes one value as JSON text. A float keeps a point, so a later int declaration of the same
 * key does not read it.
 * @return False for a float that is not finite.
 */
[[nodiscard]] bool format_value(const ConfigValue& value, Text<kValueCapacity>& output) noexcept {
    output = {};
    char* const first = output.bytes.data();
    // One byte stays free, so the text is always null-terminated.
    char* const last = first + output.bytes.size() - 1;
    std::to_chars_result result{last, std::errc::value_too_large};
    if (value.type == ConfigType::boolean) {
        const std::string_view text = value.boolean ? kTrueText : kFalseText;
        result = {std::copy(text.begin(), text.end(), first), std::errc{}};
    } else if (value.type == ConfigType::integer) {
        result = std::to_chars(first, last, value.integer);
    } else if (value.type == ConfigType::floating && std::isfinite(value.floating)) {
        result = std::to_chars(first, last, value.floating);
    }
    if (result.ec != std::errc{}) {
        return false;
    }
    output.length = static_cast<std::size_t>(result.ptr - first);
    if (value.type != ConfigType::floating
        || output.view().find_first_of(kFloatMarks) != std::string_view::npos) {
        return true;
    }
    if (output.length + kPointZero.size() >= output.bytes.size()) {
        return false;
    }
    std::copy(kPointZero.begin(), kPointZero.end(), first + output.length);
    output.length += kPointZero.size();
    return true;
}

/** Carries one declaration lookup through the mod list visit. */
struct Lookup final {
    std::string_view modId;
    std::string_view key;
    ConfigEntry entry{};
    bool found{};
    ConfigValue* value{};
};

/**
 * Finds the key among the loaded mods, which come first in the view.
 * @return Always true; the lookup records whether the key was found.
 */
[[nodiscard]] bool find_declaration(void* context, const View& view) noexcept {
    auto& lookup = *static_cast<Lookup*>(context);
    const auto loaded = std::span(view.records, view.loadedCount);
    const auto mod = std::find_if(loaded.begin(), loaded.end(), [&](const Record& record) {
        return record.manifest.id.view() == lookup.modId;
    });
    if (mod == loaded.end()) {
        return true;
    }
    const auto entries = std::span(mod->manifest.config).first(mod->manifest.configCount);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const ConfigEntry& entry) {
        return entry.key.view() == lookup.key;
    });
    lookup.found = found != entries.end();
    if (lookup.found) {
        lookup.entry = *found;
        if (lookup.value != nullptr) {
            (void)current(lookup.modId, *found, *lookup.value);
        }
    }
    return true;
}

/**
 * Copies one declaration out of the mod list. The config lock is never held inside the visit.
 * @return False when the mod is not loaded or does not declare the key.
 */
[[nodiscard]] bool
declared(std::string_view modId, std::string_view key, ConfigEntry& output) noexcept {
    Lookup lookup{modId, key};
    if (!mods::visit(&find_declaration, &lookup) || !lookup.found) {
        return false;
    }
    output = lookup.entry;
    return true;
}

/**
 * Reads the saved value of one declaration, or its default.
 * @return True when a saved value applies. The caller holds the lock.
 */
[[nodiscard]] bool
stored_value(std::string_view modId, const ConfigEntry& entry, ConfigValue& output) noexcept {
    output = entry.defaultValue;
    const std::size_t index = find_index(g_storage, modId, entry.key.view());
    // A value whose type no longer matches the declaration is ignored.
    return found_at(g_storage, index, modId, entry.key.view())
           && manifest::read_config_value(g_storage.values[index].value.view(), output);
}

} // namespace

/** Loads the saved values at boot. A refused file is moved aside, never overwritten. */
bool load(std::wstring_view artifactDirectory) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_values(g_storage);
    g_storage.ready = false;
    const bool built = core::path::assign(g_storage.path, artifactDirectory)
                       && core::path::append(g_storage.path, kFileName);
    if (built) {
        core::path::remove_stale_siblings(g_storage.path.chars.data());
        g_storage.ready = read_file(g_storage);
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    if (!built) {
        report("path");
    }
    return built;
}

/** Records the value each bound key of a loaded mod uses at boot. */
void publish(std::span<const Record> records) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    g_storage.appliedCount = 0;
    for (const Record& record : records) {
        if (record.status != Status::loaded) {
            continue;
        }
        const Manifest& mod = record.manifest;
        const auto entries = std::span(mod.config).first(mod.configCount);
        for (const ConfigBinding& binding :
             std::span(mod.configBindings).first(mod.configBindingCount)) {
            const auto saved = std::span(g_storage.applied).first(g_storage.appliedCount);
            const auto existing =
                std::find_if(saved.begin(), saved.end(), [&](const Applied& value) {
                    return value.modId.view() == mod.id.view()
                           && value.key.view() == binding.key.view();
                });
            if (existing != saved.end() || g_storage.appliedCount == g_storage.applied.size()) {
                continue;
            }
            // The parser refuses a binding whose key is not declared.
            const ConfigEntry& entry =
                *std::find_if(entries.begin(), entries.end(), [&](const ConfigEntry& value) {
                    return value.key.view() == binding.key.view();
                });
            Applied& value = g_storage.applied[g_storage.appliedCount++];
            value.modId = mod.id;
            value.key = entry.key;
            (void)stored_value(mod.id.view(), entry, value.value);
        }
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Drops the saved values and the file path. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_values(g_storage);
    g_storage.path.chars[0] = L'\0';
    g_storage.path.length = 0;
    g_storage.ready = false;
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Declaration and current value share one published mod-list lifetime. */
bool read(std::string_view modId, std::string_view key, ConfigValue& output) noexcept {
    Lookup lookup{modId, key, {}, false, &output};
    return mods::visit(&find_declaration, &lookup) && lookup.found;
}

/** Stores and saves one value of the declared type. */
bool write(std::string_view modId, std::string_view key, const ConfigValue& value) noexcept {
    ConfigEntry entry{};
    Text<kValueCapacity> text{};
    if (!declared(modId, key, entry) || value.type != entry.defaultValue.type
        || !format_value(value, text)) {
        return false;
    }
    const char* failure = nullptr;
    AcquireSRWLockExclusive(&g_storage.lock);
    const std::size_t index = find_index(g_storage, modId, key);
    const bool existed = found_at(g_storage, index, modId, key);
    const Saved previous = existed ? g_storage.values[index] : Saved{};
    if (!g_storage.ready) {
        failure = "path";
    } else if (!put(modId, key, text.view())) {
        failure = "capacity";
    } else if (!save()) {
        if (existed) {
            g_storage.values[index] = previous;
        } else {
            erase(index);
        }
        failure = "write";
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    if (failure != nullptr) {
        report(failure);
    }
    return failure == nullptr;
}

/** Removes the saved value, so the key follows its default again. */
bool reset(std::string_view modId, std::string_view key) noexcept {
    ConfigEntry entry{};
    if (!declared(modId, key, entry)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    const std::size_t index = find_index(g_storage, modId, key);
    const bool removed = g_storage.ready && found_at(g_storage, index, modId, key);
    const Saved previous = removed ? g_storage.values[index] : Saved{};
    if (removed) {
        erase(index);
    }
    const bool saved = !removed || save();
    if (!saved) {
        static_cast<void>(put(previous.modId.view(), previous.key.view(), previous.value.view()));
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    if (!saved) {
        report("write");
    }
    return saved;
}

/** Value for a declaration the caller already holds. */
bool current(std::string_view modId, const ConfigEntry& entry, ConfigValue& output) noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const bool saved = stored_value(modId, entry, output);
    ReleaseSRWLockShared(&g_storage.lock);
    return saved;
}

/** Only keys used by the loaded package overlay have an applied value. */
bool applied(std::string_view modId, std::string_view key, ConfigValue& output) noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const auto values = std::span(g_storage.applied).first(g_storage.appliedCount);
    const auto found = std::find_if(values.begin(), values.end(), [&](const Applied& value) {
        return value.modId.view() == modId && value.key.view() == key;
    });
    const bool matched = found != values.end();
    if (matched) {
        output = found->value;
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return matched;
}

/** Compares only the member that `type` names. */
bool same_value(const ConfigValue& first, const ConfigValue& second) noexcept {
    if (first.type != second.type) {
        return false;
    }
    switch (first.type) {
    case ConfigType::integer:
        return first.integer == second.integer;
    case ConfigType::floating:
        return std::bit_cast<std::uint32_t>(first.floating)
               == std::bit_cast<std::uint32_t>(second.floating);
    case ConfigType::boolean:
        return first.boolean == second.boolean;
    }
    return false;
}

/** Both snapshots are read under one lock so concurrent saves cannot mix their values. */
bool restart_required(std::string_view modId, const ConfigEntry& entry) noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const auto values = std::span(g_storage.applied).first(g_storage.appliedCount);
    const auto found = std::find_if(values.begin(), values.end(), [&](const Applied& value) {
        return value.modId.view() == modId && value.key.view() == entry.key.view();
    });
    ConfigValue value{};
    (void)stored_value(modId, entry, value);
    const bool changed = found != values.end() && !same_value(found->value, value);
    ReleaseSRWLockShared(&g_storage.lock);
    return changed;
}

} // namespace sunrise::state::mods::config
