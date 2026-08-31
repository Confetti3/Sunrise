#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "database.h"
#include "../../activity/defaults/definition.h"
#include "../../build_data/definition.h"
#include "../../entitlements/definition.h"
#include "../../investment/investment.h"
#include "../../unlocks/definition.h"

namespace sunrise::state::persistence::sqlite {

inline constexpr std::size_t kDatabaseUuidCapacity = 36;
inline constexpr std::size_t kSha256TextCapacity = 64;

/** Bounded schema-v2 store identity and migration journal metadata. */
struct StoreMetadata final {
    std::array<char, kDatabaseUuidCapacity> databaseUuid{};
    std::size_t databaseUuidLength{};
    std::uint32_t seedRevision{};
    std::array<char, kSha256TextCapacity> seedHash{};
    std::uint32_t contentBuild{};
    build_data::BuildIdentity buildIdentity{};
    std::uint32_t legacySourceVersion{};
    std::array<char, kSha256TextCapacity> legacySourceHash{};
    std::uint8_t accountOrigin{};
    std::uint8_t bootstrapOrigin{};
    std::uint8_t compactionState{};
};

/** All immutable/bootstrap domains moved by schema v2, in their existing runtime types. */
struct BootstrapSnapshot final {
    entitlements::Table entitlements;
    unlocks::Table unlocks;
    Family5State family5;
    activity::defaults::ActivityDefaults activityDefaults;
};

/** Loads the required singleton metadata row without changing it. */
[[nodiscard]] bool load_store_metadata(Database& database, StoreMetadata& output) noexcept;

/** Loads and strictly validates one canonical schema-v2 bootstrap snapshot. */
[[nodiscard]] bool load_bootstrap(Database& database, BootstrapSnapshot& output) noexcept;
/** Canonicalizes and atomically replaces every schema-v2 bootstrap table. */
[[nodiscard]] bool save_bootstrap(Database& database,
                                  const BootstrapSnapshot& snapshot) noexcept;

} // namespace sunrise::state::persistence::sqlite
