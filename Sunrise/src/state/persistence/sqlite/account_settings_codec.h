#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../account/account_state.h"
#include "database.h"

namespace sunrise::state::persistence::sqlite {

/** Outcome of decoding the independently versioned settings payload. */
enum class SettingsDecodeResult {
    decoded,
    incompatible,
    invalid,
};

/** Fixed-capacity little-endian writer for AccountSettings. */
class SettingsWriter final {
public:
    [[nodiscard]] bool put_u8(std::uint8_t value) noexcept;
    [[nodiscard]] bool put_bool(bool value) noexcept;
    [[nodiscard]] bool put_u16(std::uint16_t value) noexcept;
    [[nodiscard]] bool put_u32(std::uint32_t value) noexcept;
    [[nodiscard]] bool put_i8(std::int8_t value) noexcept;
    [[nodiscard]] bool put_i32(std::int32_t value) noexcept;
    [[nodiscard]] bool put_float(float value) noexcept;
    [[nodiscard]] bool put_optional_u16(const std::optional<std::uint16_t>& value) noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    std::array<std::byte, kSettingsPayloadCapacity> bytes_{};
    std::size_t size_{};
};

/** Bounds-checked little-endian reader paired with SettingsWriter. */
class SettingsReader final {
public:
    explicit SettingsReader(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] bool get_u8(std::uint8_t& output) noexcept;
    [[nodiscard]] bool get_bool(bool& output) noexcept;
    [[nodiscard]] bool get_u16(std::uint16_t& output) noexcept;
    [[nodiscard]] bool get_u32(std::uint32_t& output) noexcept;
    [[nodiscard]] bool get_i8(std::int8_t& output) noexcept;
    [[nodiscard]] bool get_i32(std::int32_t& output) noexcept;
    [[nodiscard]] bool get_float(float& output) noexcept;
    [[nodiscard]] bool get_optional_u16(std::optional<std::uint16_t>& output) noexcept;
    [[nodiscard]] bool complete() const noexcept;

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

/** Encodes all setting fields, including optional binding presence, without object padding. */
[[nodiscard]] bool encode_settings(const account::settings::AccountSettings& value,
                                   SettingsWriter& writer) noexcept;
/** Decodes exactly one version-1 settings payload. */
[[nodiscard]] SettingsDecodeResult
 decode_settings(std::span<const std::byte> bytes,
                 account::settings::AccountSettings& output) noexcept;

namespace detail {
using ::sunrise::state::persistence::sqlite::SettingsDecodeResult;
using ::sunrise::state::persistence::sqlite::SettingsReader;
using ::sunrise::state::persistence::sqlite::SettingsWriter;
using ::sunrise::state::persistence::sqlite::decode_settings;
using ::sunrise::state::persistence::sqlite::encode_settings;
} // namespace detail

} // namespace sunrise::state::persistence::sqlite
