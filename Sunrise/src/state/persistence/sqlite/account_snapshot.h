#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../account/account_state.h"
#include "account_settings_codec.h"
#include "database.h"

namespace sunrise::state::persistence::sqlite {

/** Outcome of attempting to restore one complete account snapshot. */
enum class LoadResult {
    disabled,
    missing,
    loaded,
    schemaNewer,
    formatNewer,
    invalid,
};

/** Loads one complete snapshot from an already-open v1 database. */
[[nodiscard]] LoadResult load_account(Database& database, AccountState& output) noexcept;
/** Replaces every account row in one BEGIN IMMEDIATE transaction. */
[[nodiscard]] bool save_account(Database& database, const AccountState& account) noexcept;

namespace detail {
using ::sunrise::state::persistence::sqlite::LoadResult;
using ::sunrise::state::persistence::sqlite::load_account;
using ::sunrise::state::persistence::sqlite::save_account;
} // namespace detail

} // namespace sunrise::state::persistence::sqlite
