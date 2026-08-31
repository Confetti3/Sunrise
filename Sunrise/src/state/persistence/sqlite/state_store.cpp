#include "state_store.h"

namespace sunrise::state::persistence::sqlite {

StateStore::~StateStore() noexcept {
    close();
}

OpenResult StateStore::open(std::wstring_view path) noexcept {
    std::scoped_lock lock(mutex_);
    return open_unlocked(path);
}

OpenResult StateStore::open(const std::wstring& path) noexcept {
    return open(std::wstring_view(path));
}

OpenResult StateStore::open(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring widePath = path.wstring();
        return open(std::wstring_view(widePath));
    } catch (...) {
        return OpenResult::failed;
    }
}

OpenResult StateStore::open_unlocked(std::wstring_view path) noexcept {
    if (!close_unlocked()) {
        return OpenResult::failed;
    }
    status_ = database_.open(path);
    schemaInitialized_ = false;
    writesDisabled_ = status_ == OpenResult::newer;
    if (!database_.healthy()) {
        fail_closed_unlocked();
    }
    return status_;
}

OpenResult StateStore::inspect_schema() noexcept {
    std::scoped_lock lock(mutex_);
    if (!database_.is_open()) {
        status_ = OpenResult::failed;
        schemaInitialized_ = false;
        return status_;
    }
    status_ = database_.inspect_schema();
    if (status_ != OpenResult::opened || database_.schema_version() != kSchemaVersion) {
        schemaInitialized_ = false;
    }
    if (!database_.healthy()) {
        fail_closed_unlocked();
    }
    return status_;
}

OpenResult StateStore::initialize_schema() noexcept {
    std::scoped_lock lock(mutex_);
    if (!database_.is_open()) {
        status_ = OpenResult::failed;
        schemaInitialized_ = false;
        return status_;
    }
    status_ = database_.initialize_schema();
    schemaInitialized_ = status_ == OpenResult::opened
                         && database_.schema_version() == kSchemaVersion;
    if (!database_.healthy()) {
        fail_closed_unlocked();
    }
    return status_;
}

LoadResult StateStore::load(AccountState& output) noexcept {
    std::scoped_lock lock(mutex_);
    if (!schemaInitialized_) {
        output = {};
        return status_ == OpenResult::newer ? LoadResult::schemaNewer : LoadResult::disabled;
    }
    const LoadResult result = load_account(database_, output);
    if (result == LoadResult::formatNewer || result == LoadResult::schemaNewer) {
        writesDisabled_ = true;
    }
    if (!database_.healthy()) {
        fail_closed_unlocked();
    }
    return result;
}

bool StateStore::save(const AccountState& account) noexcept {
    std::scoped_lock lock(mutex_);
    if (!schemaInitialized_ || writesDisabled_) {
        return false;
    }
    const bool result = save_account(database_, account);
    if (!database_.healthy()) {
        fail_closed_unlocked();
    }
    return result;
}

void StateStore::close() noexcept {
    std::scoped_lock lock(mutex_);
    (void)close_unlocked();
}

bool StateStore::close_unlocked() noexcept {
    if (database_.close()) {
        status_ = OpenResult::failed;
        schemaInitialized_ = false;
        writesDisabled_ = false;
        return true;
    }
    if (!database_.healthy() && database_.close_unhealthy()) {
        status_ = OpenResult::failed;
        schemaInitialized_ = false;
        writesDisabled_ = false;
        return true;
    }
    // A healthy connection that returned SQLITE_BUSY remains open and all state remains truthful;
    // the caller may finalize its outstanding statement and call close again.
    return false;
}

void StateStore::fail_closed_unlocked() noexcept {
    status_ = OpenResult::failed;
    schemaInitialized_ = false;
    writesDisabled_ = true;
    if (!database_.healthy()) {
        (void)database_.close_unhealthy();
    }
}

bool StateStore::is_open() const noexcept {
    std::scoped_lock lock(mutex_);
    return database_.is_open();
}

bool StateStore::is_writable() const noexcept {
    std::scoped_lock lock(mutex_);
    return database_.is_writable() && !writesDisabled_;
}

OpenResult StateStore::status() const noexcept {
    std::scoped_lock lock(mutex_);
    return status_;
}

} // namespace sunrise::state::persistence::sqlite
