#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "account_snapshot.h"

namespace sunrise::state::persistence::sqlite {

/** Small lifecycle wrapper used by tests and future production integration. */
class StateStore final {
public:
    using Status = OpenResult;

    StateStore() noexcept = default;
    ~StateStore() noexcept;

    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    /** Opens exactly the caller-provided path, probing existing files read-only first. */
    [[nodiscard]] OpenResult open(std::wstring_view path) noexcept;
    [[nodiscard]] OpenResult open(const std::wstring& path) noexcept;
    [[nodiscard]] OpenResult open(const std::filesystem::path& path) noexcept;
    /** Inspects user_version without changing journal or pragma state. */
    [[nodiscard]] OpenResult inspect_schema() noexcept;
    /** Enables supported pragmas and creates the latest schema for a new database. */
    [[nodiscard]] OpenResult initialize_schema() noexcept;
    /** Loads a complete account after initialize_schema succeeded. */
    [[nodiscard]] LoadResult load(AccountState& output) noexcept;
    /** Atomically replaces a complete account after initialize_schema succeeded. */
    [[nodiscard]] bool save(const AccountState& account) noexcept;
    /** Closes the store without attempting an implicit save. */
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_writable() const noexcept;
    [[nodiscard]] OpenResult status() const noexcept;

private:
    [[nodiscard]] OpenResult open_unlocked(std::wstring_view path) noexcept;
    [[nodiscard]] bool close_unlocked() noexcept;
    void fail_closed_unlocked() noexcept;

    Database database_;
    mutable std::mutex mutex_;
    OpenResult status_{OpenResult::failed};
    bool schemaInitialized_{};
    bool writesDisabled_{};
};

} // namespace sunrise::state::persistence::sqlite
