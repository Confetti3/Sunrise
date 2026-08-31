#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::state::persistence::sqlite {

class Database;

/** SQLite application id containing the big-endian ASCII bytes "SUNR". */
inline constexpr std::int32_t kApplicationId = 0x53554E52;
/** PR88's account-only schema remains version one. Bootstrap state is version two. */
inline constexpr int kSchemaVersion = 2;

/** Creates a fresh account schema or migrates a PR88 v1 schema to v2 atomically. */
[[nodiscard]] bool initialize_state_schema(Database& database) noexcept;
/** Exact checked-in schema text compiled into the current production path. */
[[nodiscard]] std::string_view schema_v1_sql() noexcept;
/** Exact checked-in additive migration text compiled into the current production path. */
[[nodiscard]] std::string_view migration_v1_to_v2_sql() noexcept;

} // namespace sunrise::state::persistence::sqlite
