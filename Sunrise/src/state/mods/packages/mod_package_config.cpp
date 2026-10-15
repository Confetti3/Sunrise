#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/config_bake.h"
#include "../../../middleware/crypto/sha256.h"
#include "../../content_manifest/fingerprint/content_manifest_fingerprint.h"
#include "../../content_manifest/scanner/package/content_manifest_package_name.h"
#include "../config/mod_config.h"
#include "../problem/mod_problem.h"
#include "internal.h"

namespace sunrise::state::mods::packages::internal {
namespace {

namespace bake = middleware::content::packages::config_bake;
namespace layout = middleware::content::packages::reader::layout;
namespace fingerprint = content_manifest::fingerprint;
namespace name = content_manifest::scanner::package;
/** Separates rewrite keys from other Sunrise SHA-256 uses. The digit is the key version. */
constexpr std::string_view kConfigDomain = "SunriseModConfig2";
/** Separates the stem from the key in a config cache file name. */
constexpr std::wstring_view kKeyInfix = L".config.";
/** Files are hashed in fixed chunks, so memory does not grow with package size. */
constexpr std::size_t kHashChunkSize = 16 * 1024;

/** @return The absolute path of an overlay file. */
std::wstring absolute(const Work& work, const OverlayFile& file) {
    return join(work.paths.gameDirectory, widen({file.clientPath.data(), file.clientPathLength}));
}

/** Parent paths remain raw mod files even after served rows are redirected to cache files. */
std::vector<bake::Parent> parents(const Work& work,
                                  std::span<const OverlayFile> additional,
                                  std::uint16_t packageId,
                                  std::uint16_t generation) {
    std::vector<bake::Parent> output;
    const auto append = [&](std::span<const OverlayFile> files) {
        for (const OverlayFile& file : files) {
            if (file.row.packageId != packageId) {
                continue;
            }
            name::ParsedName parsed{};
            if (!name::parse_stem({file.row.name.data(), file.row.nameLength}, parsed)
                || parsed.patchIndex >= generation) {
                continue;
            }
            output.push_back({static_cast<std::uint16_t>(parsed.patchIndex), absolute(work, file)});
        }
    };
    append(work.rawParents);
    append(additional);
    std::sort(
        output.begin(), output.end(), [](const bake::Parent& first, const bake::Parent& second) {
            return first.patchId < second.patchId;
        });
    return output;
}

/** Returns the newest row for one package from a validated raw file set. */
const OverlayFile* newest(std::span<const OverlayFile> files,
                          std::uint16_t packageId,
                          std::uint16_t& generation) noexcept {
    const OverlayFile* output = nullptr;
    generation = 0;
    for (const OverlayFile& file : files) {
        if (file.row.packageId != packageId) {
            continue;
        }
        name::ParsedName parsed{};
        if (!name::parse_stem({file.row.name.data(), file.row.nameLength}, parsed)) {
            continue;
        }
        if (output == nullptr || parsed.patchIndex > generation) {
            output = &file;
            generation = static_cast<std::uint16_t>(parsed.patchIndex);
        }
    }
    return output;
}

/** Required earlier bindings cannot be displaced by a later mod's entry replacement. */
bool preserves_bindings(Work& work, std::size_t record, std::span<const Claim> claims) {
    for (const Claim& claim : claims) {
        for (const Bound& binding : work.bindings) {
            if (package_id(binding.site.tag) != claim.file.row.packageId
                || !claim.changed.test(binding.site.tag & layout::kTagEntryMask)) {
                continue;
            }
            problem::refuse(work.records[record],
                            Reason::assetOverride,
                            "Entry 0x%08X has a required config binding from %s.",
                            binding.site.tag,
                            work.records[binding.record].manifest.id.bytes.data());
            return false;
        }
    }
    return true;
}

/** Each group validates against the declaring mod's raw source before its files are committed. */
bool validate_owned(Work& work,
                    std::size_t record,
                    std::span<const OverlayFile> files,
                    std::span<const Claim> claims,
                    std::span<const Bound> bindings) {
    std::vector<std::uint16_t> checked;
    for (const Bound& binding : bindings) {
        const auto packageId = package_id(binding.site.tag);
        if (std::find(checked.begin(), checked.end(), packageId) != checked.end()) {
            continue;
        }
        checked.push_back(packageId);
        std::uint16_t generation = 0;
        const OverlayFile* source = newest(files, packageId, generation);
        if (source == nullptr) {
            const auto claim = std::find_if(claims.begin(), claims.end(), [&](const Claim& item) {
                return item.file.row.packageId == packageId;
            });
            if (claim != claims.end()) {
                source = &claim->file;
                name::ParsedName parsed{};
                if (!name::parse_stem({source->row.name.data(), source->row.nameLength}, parsed)) {
                    return false;
                }
                generation = static_cast<std::uint16_t>(parsed.patchIndex);
            }
        }
        if (source == nullptr) {
            return false;
        }
        std::vector<bake::Value> values;
        for (const Bound& row : bindings) {
            if (package_id(row.site.tag) == packageId) {
                values.push_back({row.site, row.value});
            }
        }
        const auto inherited = parents(work, files, packageId, generation);
        const auto status = bake::validate(absolute(work, *source).c_str(), inherited, values);
        if (status != bake::Status::ok) {
            problem::refuse(work.records[record],
                            Reason::packageInvalid,
                            "Package 0x%04X config binding refused: %s.",
                            packageId,
                            bake::status_text(status));
            return false;
        }
    }
    return true;
}

/** Unused ConfigValue lanes and struct padding never enter a cache identity. */
bool hash_value(fingerprint::Hasher& hasher, const Bound& binding, const Record& record) noexcept {
    const ConfigBinding& site = binding.site;
    const auto id = record.manifest.id.view();
    const auto key = site.key.view();
    std::uint32_t bits = 0;
    switch (binding.value.type) {
    case ConfigType::integer:
        bits = static_cast<std::uint32_t>(binding.value.integer);
        break;
    case ConfigType::floating:
        bits = std::bit_cast<std::uint32_t>(binding.value.floating);
        break;
    case ConfigType::boolean:
        bits = binding.value.boolean ? 1U : 0U;
        break;
    }
    const std::array fields{static_cast<std::uint32_t>(site.profile),
                            static_cast<std::uint32_t>(site.type),
                            site.tag,
                            site.classId,
                            site.size,
                            site.propertyOffset,
                            site.poolOffset,
                            site.bindingOffset,
                            site.offset,
                            static_cast<std::uint32_t>(binding.value.type),
                            bits};
    bool valid = fingerprint::update_u32(hasher, static_cast<std::uint32_t>(id.size()))
                 && hasher.update(std::as_bytes(std::span(id)))
                 && fingerprint::update_u32(hasher, static_cast<std::uint32_t>(key.size()))
                 && hasher.update(std::as_bytes(std::span(key)))
                 && hasher.update(site.sourceSha256);
    for (const auto field : fields) {
        valid = valid && fingerprint::update_u32(hasher, field);
    }
    return valid;
}

/** Each replacement enters the key as its index, size and SHA-256. */
bool hash_replacements(fingerprint::Hasher& hasher,
                       std::span<const bake::Replacement> replacements) noexcept {
    bool valid = fingerprint::update_u32(hasher, static_cast<std::uint32_t>(replacements.size()));
    for (const bake::Replacement& item : replacements) {
        middleware::crypto::sha256::Digest digest{};
        valid = valid && fingerprint::update_u32(hasher, item.index)
                && fingerprint::update_u64(hasher, item.bytes.size())
                && middleware::crypto::sha256::hash(item.bytes, digest) && hasher.update(digest);
    }
    return valid;
}

/** @return The cache file name: stem, `.config.`, then the SHA-256 of every input. */
std::wstring cache_leaf(const Work& work,
                        const OverlayFile& source,
                        const std::wstring& path,
                        std::span<const bake::Parent> inherited,
                        std::span<const bake::Replacement> replacements,
                        SourceInputs& inputs) {
    fingerprint::Hasher hasher;
    bool valid = hasher.update(std::as_bytes(std::span(kConfigDomain))) && inputs.hash(hasher, path)
                 && fingerprint::update_u32(hasher, static_cast<std::uint32_t>(inherited.size()));
    for (const auto& parent : inherited) {
        valid = valid && fingerprint::update_u16(hasher, parent.patchId)
                && inputs.hash(hasher, parent.path);
    }
    for (const Bound& binding : work.bindings) {
        if (package_id(binding.site.tag) == source.row.packageId) {
            valid = valid && hash_value(hasher, binding, work.records[binding.record]);
        }
    }
    valid = valid && hash_replacements(hasher, replacements);
    content_manifest::Fingerprint digest{};
    if (!valid || !hasher.finish(digest)) {
        return {};
    }
    return cache_leaf_name({source.row.name.data(), source.row.nameLength}, kKeyInfix, digest);
}

/** How one package rewrite ended. */
enum class Rewrite : std::uint8_t {
    ok,
    /** A package with required config bindings failed; its mod is refused. */
    required,
    /** A package with merged entries only failed. */
    replacements,
};

/**
 * Writes one package's merged entries and config values into a cache file and serves it.
 * @param status Receives why the rewrite failed.
 * @return How the rewrite ended.
 */
Rewrite rewrite_package(Work& work, std::uint16_t packageId, bake::Status& status) {
    std::vector<bake::Value> values;
    const Bound* required = nullptr;
    for (const Bound& binding : work.bindings) {
        if (package_id(binding.site.tag) == packageId) {
            required = required == nullptr ? &binding : required;
            values.push_back({binding.site, binding.value});
        }
    }
    std::vector<bake::Replacement> replacements;
    for (const table_merge::Replacement& item : work.replacements) {
        if (item.packageId == packageId) {
            replacements.push_back({item.entryIndex, item.bytes});
        }
    }
    std::uint16_t generation = 0;
    const OverlayFile* selected = newest(work.files, packageId, generation);
    status = bake::Status::writeFailed;
    if (selected != nullptr) {
        const auto index = static_cast<std::size_t>(selected - work.files.data());
        const auto inherited = parents(work, {}, packageId, generation);
        const auto source = absolute(work, *selected);
        SourceInputs inputs;
        const auto leaf = cache_leaf(work, *selected, source, inherited, replacements, inputs);
        if (!leaf.empty() && ensure_merge_directory(work)) {
            const auto output = join(work.paths.mergeDirectory, leaf);
            status =
                bake::matches(source.c_str(), inherited, values, replacements, output.c_str())
                    ? bake::Status::ok
                    : bake::write(source.c_str(), inherited, values, replacements, output.c_str());
            if (status == bake::Status::ok && client_path(work, output, work.files[index])) {
                work.cacheLeaves.push_back(leaf);
                return Rewrite::ok;
            }
            if (status == bake::Status::ok) {
                status = bake::Status::writeFailed;
            }
        }
    }
    core::log::writef(core::log::Channel::state,
                      required != nullptr ? core::log::Level::error : core::log::Level::warn,
                      "ev=mods stage=rewrite package=0x%04X result=fail reason=%s config=%s",
                      packageId,
                      bake::status_name(status),
                      required != nullptr ? "required" : "none");
    if (required != nullptr) {
        problem::refuse(work.records[required->record],
                        Reason::packageInvalid,
                        "Package 0x%04X config output failed: %s.",
                        packageId,
                        bake::status_text(status));
        work.failed = required->record;
        return Rewrite::required;
    }
    return Rewrite::replacements;
}

/**
 * Rewrites each package id with bindings or replacements, and stops at the first failure.
 * @param failed Receives the package that failed.
 * @param status Receives why it failed.
 * @return How the first failed rewrite ended, or ok.
 */
Rewrite rewrite_each(Work& work, std::uint16_t& failed, bake::Status& status) {
    std::vector<std::uint16_t> packageIds;
    packageIds.reserve(work.bindings.size() + work.replacements.size());
    for (const Bound& binding : work.bindings) {
        packageIds.push_back(package_id(binding.site.tag));
    }
    for (const table_merge::Replacement& item : work.replacements) {
        packageIds.push_back(item.packageId);
    }
    std::sort(packageIds.begin(), packageIds.end());
    packageIds.erase(std::unique(packageIds.begin(), packageIds.end()), packageIds.end());
    for (const std::uint16_t packageId : packageIds) {
        const Rewrite outcome = rewrite_package(work, packageId, status);
        if (outcome != Rewrite::ok) {
            failed = packageId;
            return outcome;
        }
    }
    return Rewrite::ok;
}

} // namespace

/** Closes the held input files. */
SourceInputs::~SourceInputs() {
    for (const HANDLE file : handles_) {
        (void)CloseHandle(file);
    }
}

/** No one can write the file while it is hashed. */
bool SourceInputs::hash(fingerprint::Hasher& hasher, const std::wstring& path) {
    // The key hashes the plain path, so only the open uses the extended form.
    const HANDLE file = CreateFileW(core::path::extended(path.c_str()).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    try {
        handles_.push_back(file);
    } catch (...) {
        (void)CloseHandle(file);
        throw;
    }
    LARGE_INTEGER size{};
    bool valid = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
                 && fingerprint::update_u64(hasher, static_cast<std::uint64_t>(size.QuadPart))
                 && fingerprint::update_u32(hasher, static_cast<std::uint32_t>(path.size()))
                 && hasher.update(std::as_bytes(std::span(path)));
    std::array<std::byte, kHashChunkSize> chunk{};
    std::uint64_t read = 0;
    while (valid && read < static_cast<std::uint64_t>(size.QuadPart)) {
        DWORD count = 0;
        valid =
            ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr) != FALSE
            && count != 0 && hasher.update(std::span(chunk).first(count));
        read += count;
    }
    valid = valid && read == static_cast<std::uint64_t>(size.QuadPart);
    return valid;
}

/** Only owned entry replacements may declare new config bindings. */
bool admit_bindings(Work& work,
                    std::size_t record,
                    std::span<const OverlayFile> files,
                    std::span<const Claim> claims,
                    std::vector<Bound>& output) {
    if (!preserves_bindings(work, record, claims)) {
        return false;
    }
    const Manifest& mod = work.records[record].manifest;
    for (const ConfigBinding& site : std::span(mod.configBindings).first(mod.configBindingCount)) {
        const auto packageId = package_id(site.tag);
        std::uint16_t generation = 0;
        const bool added = newest(files, packageId, generation) != nullptr;
        const bool replaced = std::any_of(claims.begin(), claims.end(), [&](const Claim& claim) {
            return claim.file.row.packageId == packageId
                   && claim.changed.test(site.tag & layout::kTagEntryMask);
        });
        if (!added && !replaced) {
            problem::refuse(work.records[record],
                            Reason::packageInvalid,
                            "Config entry 0x%08X must be supplied by this mod.",
                            site.tag);
            return false;
        }
        const auto entries = std::span(mod.config).first(mod.configCount);
        const auto entry =
            std::find_if(entries.begin(), entries.end(), [&](const ConfigEntry& value) {
                return value.key.view() == site.key.view();
            });
        Bound binding{record, site};
        if (entry == entries.end()) {
            problem::refuse(work.records[record],
                            Reason::packageInvalid,
                            "Config value for entry 0x%08X could not be prepared.",
                            site.tag);
            return false;
        }
        (void)config::current(mod.id.view(), *entry, binding.value);
        output.push_back(binding);
    }
    return validate_owned(work, record, files, claims, output);
}

/** Raw parent paths stay; only each final newest row is redirected. */
bool rewrite_packages(Work& work) {
    const std::vector<OverlayFile> files = work.files;
    const std::size_t leaves = work.cacheLeaves.size();
    std::uint16_t failed = 0;
    auto status = bake::Status::ok;
    const Rewrite outcome = rewrite_each(work, failed, status);
    if (outcome != Rewrite::replacements) {
        return outcome == Rewrite::ok;
    }
    // Merged rows span packages, so the files written so far are undone and only bindings stay.
    work.files = files;
    work.cacheLeaves.resize(leaves);
    drop_replacements(work, failed, bake::status_text(status));
    return rewrite_each(work, failed, status) == Rewrite::ok;
}

} // namespace sunrise::state::mods::packages::internal
