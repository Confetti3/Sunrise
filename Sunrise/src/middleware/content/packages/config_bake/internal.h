#pragma once

#include <Windows.h>

#include <array>
#include <span>
#include <utility>
#include <vector>

#include "../config_bake.h"
#include "../merge/internal.h"

namespace sunrise::middleware::content::packages::config_bake::internal {

namespace tables = merge::internal;
namespace layout = reader::layout;
using tables::align;
using tables::HeaderField;
using tables::kDiskAlignment;
using tables::kFooterMarker;
using tables::load;
using tables::MetadataField;
using tables::store;

/** Entry starts use sixteen-byte alignment. */
inline constexpr std::uint64_t kEntryAlignment = 16;
/** Startup may combine every mod's bindings, but one entry still admits only 256 sites. */
inline constexpr std::size_t kValueLimit =
    state::mods::kModCapacity * state::mods::kConfigBindingLimit;

/** Closes the owned Windows handle. */
class File final {
public:
    File() noexcept = default;
    explicit File(HANDLE handle) noexcept : handle_(handle) {}
    ~File() {
        (void)close();
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            (void)close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    /** @return True when the owned handle closed, or no handle was held. */
    [[nodiscard]] bool close() noexcept {
        return !valid() || CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE)) != FALSE;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct Input final {
    File file;
    tables::Package package;
    std::uint32_t size{};
};

/** Values are copied before validation; a replacement span lives as long as the public call. */
struct Entry final {
    std::uint32_t index{};
    std::uint32_t size{};
    std::uint64_t position{};
    std::vector<Value> values;
    /** Whole new entry bytes, or empty when the entry is read from its source. */
    std::span<const std::byte> replacement;
};

/** Prepared bytes and held sources are shared by validation, comparison and publication. */
struct Prepared final {
    /** Generations point into this object, so it never moves. */
    Prepared() = default;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;

    Input source;
    std::vector<Input> parents;
    std::array<Input*, tables::kPatchLimit + 1> generations{};
    std::vector<Entry> entries;
    std::array<std::byte, layout::kHeaderSize> header{};
    std::vector<std::byte> metadata;
    std::uint32_t newBlocks{};
    std::uint64_t metadataOffset{}, blocksOffset{}, footer{}, fileSize{};
};

#if defined(SUNRISE_CONFIG_BAKE_TEST)
/** @param plan Held sources exposed only to the owned file-lock fixture. */
void sources_held_for_test(const Prepared& plan) noexcept;
#endif

/** @param source Raw package. @param parents Raw ancestors. @param output Held sources. */
[[nodiscard]] Status
read_inputs(const wchar_t* source, std::span<const Parent> parents, Prepared& output);
/**
 * The one plaintext entry reader: every fragment comes from a held generation's own blocks.
 * @param plan Held sources.
 * @param index Entry index in the newest source.
 * @param bytes Receives the plaintext entry.
 * @return ok, blocks when a fragment is not plaintext in a held file, or identity.
 */
[[nodiscard]] Status
read_entry(const Prepared& plan, std::uint32_t index, std::vector<std::byte>& bytes);
/**
 * Builds the complete output plan.
 * @param source Raw package. @param parents Raw ancestors. @param values Sites and values.
 * @param replacements Whole entries to pack, sorted by unique index.
 * @param output Receives the plan.
 * @return The first read, identity, entry or capacity failure.
 */
[[nodiscard]] Status prepare(const wchar_t* source,
                             std::span<const Parent> parents,
                             std::span<const Value> values,
                             std::span<const Replacement> replacements,
                             Prepared& output);

/** One complete zero-padded plaintext block is borrowed only for this call. */
using BlockVisitor = bool (*)(void*, std::uint32_t, std::span<const std::byte>);
/** @param plan Held sources and placements. @param visitor Receives complete new blocks. */
[[nodiscard]] bool blocks(const Prepared& plan, BlockVisitor visitor, void* context);

} // namespace sunrise::middleware::content::packages::config_bake::internal
