#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include "../../core/logging/log.h"
#include "../activity/defaults/definition.h"
#include "../runtime/runtime.h"
#include "sqlite/account_snapshot.h"

namespace sunrise::state::persistence {
namespace detail {

/** Active module path used only between a successful initialize and shutdown. */
inline void* g_module{};
/** True only after the wrapped State runtime reached a complete initialized image. */
inline bool g_active{};
/** False when startup found data written in a format this build must preserve untouched. */
inline bool g_writable{};

/** Emits one compact persistence lifecycle event through the already-open State log. */
inline void report(std::string_view stage,
                   std::string_view result,
                   std::string_view reason) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=account_persistence backend=sqlite stage=%.*s "
                                      "result=%.*s reason=%.*s",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      static_cast<int>(result.size()),
                                      result.data(),
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), length});
    }
}

} // namespace detail

/**
 * Restores a valid SQLite snapshot before State initializes, with authored settings as fallback.
 * @param module Loaded Sunrise module, or null to retain ephemeral test behavior.
 * @param initialAccount Checked account parsed from settings.json.
 * @param activityDefaults Immutable activity policy from settings.json.
 * @return True when State initializes from either the snapshot or authored fallback.
 */
[[nodiscard]] inline bool
initialize(void* module,
           const AccountState& initialAccount,
           const activity::defaults::ActivityDefaults& activityDefaults) noexcept {
    if (detail::g_active) {
        return true;
    }
    AccountState restored{};
    const sqlite::detail::LoadResult load = sqlite::detail::load_account(module, restored);
    if (load == sqlite::detail::LoadResult::loaded) {
        if (::sunrise::state::initialize(module, restored, activityDefaults)) {
            detail::g_module = module;
            detail::g_active = true;
            detail::g_writable = module != nullptr;
            detail::report("load", "ok", "restored");
            return true;
        }
        // A structurally valid snapshot can still reference data absent from a different installed
        // build. Unwind that attempt and retain the human-authored recovery path.
        ::sunrise::state::shutdown();
        detail::report("load", "fallback", "runtime_rejected");
    } else if (load == sqlite::detail::LoadResult::schemaNewer) {
        detail::report("load", "fallback", "schema_newer");
    } else if (load == sqlite::detail::LoadResult::formatNewer) {
        detail::report("load", "fallback", "format_newer");
    } else if (load == sqlite::detail::LoadResult::invalid) {
        detail::report("load", "fallback", "invalid_database");
    }
    if (!::sunrise::state::initialize(module, initialAccount, activityDefaults)) {
        return false;
    }
    detail::g_module = module;
    detail::g_active = true;
    detail::g_writable = module != nullptr
                         && load != sqlite::detail::LoadResult::schemaNewer
                         && load != sqlite::detail::LoadResult::formatNewer;
    if (module != nullptr && load == sqlite::detail::LoadResult::missing) {
        detail::report("load", "ok", "first_run");
    }
    return true;
}

/** Writes the current account snapshot without stopping State. */
[[nodiscard]] inline bool flush() noexcept {
    if (!detail::g_active || detail::g_module == nullptr || !detail::g_writable) {
        return true;
    }
    return sqlite::detail::save_account(detail::g_module, ::sunrise::state::account_snapshot());
}

/** Writes and reports one durable snapshot without stopping State. */
[[nodiscard]] inline bool checkpoint(std::string_view reason) noexcept {
    const bool report =
        detail::g_active && detail::g_module != nullptr && detail::g_writable;
    const bool saved = flush();
    if (report) {
        detail::report("save", saved ? "ok" : "fail", reason);
    }
    return saved;
}

/** Persists one final account image, then securely shuts down the State runtime. */
inline void shutdown() noexcept {
    if (detail::g_active) {
        if (detail::g_module != nullptr) {
            (void)checkpoint("clean_shutdown");
        }
        detail::g_active = false;
        detail::g_writable = false;
        detail::g_module = nullptr;
    }
    ::sunrise::state::shutdown();
}

} // namespace sunrise::state::persistence
