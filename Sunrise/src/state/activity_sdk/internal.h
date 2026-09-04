#pragma once

#include <memory>

#include "runtime.h"

namespace sunrise::state::activity_sdk {

/** Loads the fixed module-relative pack against one catalog-authorized identity. */
[[nodiscard]] bool load(void* module,
                        const ExpectedIdentity& expected,
                        std::shared_ptr<Catalog>& output,
                        Status& result) noexcept;
/** Loads one explicit pack path against one independent expected identity. */
[[nodiscard]] bool load_path_expected(const wchar_t* path,
                                      const ExpectedIdentity& expected,
                                      std::shared_ptr<Catalog>& output,
                                      Status& result) noexcept;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned regression fixture in focused test binaries only. */
[[nodiscard]] bool
load_path(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept;
/** Loads a synthetic pack against one explicit payload pin in test binaries only. */
[[nodiscard]] bool load_path_for_test(const wchar_t* path,
                                      const std::array<std::byte, 32>& expectedPayloadSha256,
                                      std::shared_ptr<Catalog>& output,
                                      Status& result) noexcept;
#endif
/** Checks every row and cross-section relation before publication. */
[[nodiscard]] bool valid_catalog(const Catalog& value) noexcept;
/** Names the check the last refused catalog failed, with its row where the check is row-local. */
[[nodiscard]] const char* last_catalog_reason() noexcept;

} // namespace sunrise::state::activity_sdk
