#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../account/account_state.h"
#include "database.h"

namespace sunrise::state::persistence::sqlite::detail {

/** Result of decoding the independently versioned settings payload. */
enum class SettingsDecodeResult {
    decoded,
    incompatible,
    invalid,
};

/** Fixed-capacity little-endian writer for the non-relational settings leaf. */
class SettingsWriter final {
public:
    [[nodiscard]] bool put_u8(std::uint8_t value) noexcept {
        if (size_ >= bytes_.size()) {
            return false;
        }
        bytes_[size_++] = static_cast<std::byte>(value);
        return true;
    }

    [[nodiscard]] bool put_bool(bool value) noexcept {
        return put_u8(value ? 1U : 0U);
    }

    [[nodiscard]] bool put_u16(std::uint16_t value) noexcept {
        return put_u8(static_cast<std::uint8_t>(value & 0xFFU))
               && put_u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }

    [[nodiscard]] bool put_u32(std::uint32_t value) noexcept {
        return put_u8(static_cast<std::uint8_t>(value & 0xFFU))
               && put_u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU))
               && put_u8(static_cast<std::uint8_t>((value >> 16U) & 0xFFU))
               && put_u8(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    }

    [[nodiscard]] bool put_i8(std::int8_t value) noexcept {
        return put_u8(std::bit_cast<std::uint8_t>(value));
    }

    [[nodiscard]] bool put_i32(std::int32_t value) noexcept {
        return put_u32(std::bit_cast<std::uint32_t>(value));
    }

    [[nodiscard]] bool put_float(float value) noexcept {
        return put_u32(std::bit_cast<std::uint32_t>(value));
    }

    [[nodiscard]] bool put_optional_u16(const std::optional<std::uint16_t>& value) noexcept {
        return put_bool(value.has_value()) && put_u16(value.value_or(0));
    }

    /** @return Complete encoded byte prefix. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {bytes_.data(), size_};
    }

private:
    std::array<std::byte, kSettingsPayloadCapacity> bytes_{};
    std::size_t size_{};
};

/** Bounds-checked little-endian reader paired with SettingsWriter. */
class SettingsReader final {
public:
    explicit SettingsReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool get_u8(std::uint8_t& output) noexcept {
        if (offset_ >= bytes_.size()) {
            return false;
        }
        output = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool get_bool(bool& output) noexcept {
        std::uint8_t value = 0;
        if (!get_u8(value) || value > 1) {
            return false;
        }
        output = value != 0;
        return true;
    }

    [[nodiscard]] bool get_u16(std::uint16_t& output) noexcept {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!get_u8(low) || !get_u8(high)) {
            return false;
        }
        output = static_cast<std::uint16_t>(low)
                 | static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U);
        return true;
    }

    [[nodiscard]] bool get_u32(std::uint32_t& output) noexcept {
        std::uint8_t byte0 = 0;
        std::uint8_t byte1 = 0;
        std::uint8_t byte2 = 0;
        std::uint8_t byte3 = 0;
        if (!get_u8(byte0) || !get_u8(byte1) || !get_u8(byte2) || !get_u8(byte3)) {
            return false;
        }
        output = static_cast<std::uint32_t>(byte0)
                 | (static_cast<std::uint32_t>(byte1) << 8U)
                 | (static_cast<std::uint32_t>(byte2) << 16U)
                 | (static_cast<std::uint32_t>(byte3) << 24U);
        return true;
    }

    [[nodiscard]] bool get_i8(std::int8_t& output) noexcept {
        std::uint8_t value = 0;
        if (!get_u8(value)) {
            return false;
        }
        output = std::bit_cast<std::int8_t>(value);
        return true;
    }

    [[nodiscard]] bool get_i32(std::int32_t& output) noexcept {
        std::uint32_t value = 0;
        if (!get_u32(value)) {
            return false;
        }
        output = std::bit_cast<std::int32_t>(value);
        return true;
    }

    [[nodiscard]] bool get_float(float& output) noexcept {
        std::uint32_t value = 0;
        if (!get_u32(value)) {
            return false;
        }
        output = std::bit_cast<float>(value);
        return std::isfinite(output);
    }

    [[nodiscard]] bool get_optional_u16(std::optional<std::uint16_t>& output) noexcept {
        bool present = false;
        std::uint16_t value = 0;
        if (!get_bool(present) || !get_u16(value)) {
            return false;
        }
        output = present ? std::optional<std::uint16_t>(value) : std::nullopt;
        return true;
    }

    /** @return True only when every payload byte was consumed. */
    [[nodiscard]] bool complete() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

/** Encodes every account-setting scalar without copying object padding or optional internals. */
[[nodiscard]] inline bool
encode_settings(const account::settings::AccountSettings& value, SettingsWriter& writer) noexcept {
    const auto& controls = value.controls;
    const auto& audio = value.audio;
    const auto& display = value.display;
    const auto& interfaceSettings = value.interface;
    const auto& social = value.social;
    if (!writer.put_u32(kSettingsPayloadVersion) || !writer.put_i8(controls.buttonLayout)
        || !writer.put_i8(controls.movementMode)
        || !writer.put_i8(controls.controllerLookSensitivity)
        || !writer.put_bool(controls.controllerInvertVertical)
        || !writer.put_bool(controls.controllerAutoLookCentering)
        || !writer.put_bool(controls.controllerVibration)
        || !writer.put_bool(controls.controllerSwapShoulders)
        || !writer.put_bool(controls.controllerInvertHorizontal)
        || !writer.put_i32(controls.mouseLookSensitivity)
        || !writer.put_bool(controls.mouseInvertVertical)
        || !writer.put_bool(controls.mouseInvertHorizontal)
        || !writer.put_bool(controls.unidentifiedToggle)
        || !writer.put_bool(controls.mouseAimSmoothing)
        || !writer.put_float(controls.adsSensitivityModifier)
        || !writer.put_i8(controls.doublePressDelay)
        || !writer.put_i8(audio.voiceOutputMode) || !writer.put_i8(audio.teamVoiceChannel)
        || !writer.put_i8(audio.reservedMode) || !writer.put_i8(audio.migrationVersion)
        || !writer.put_i8(audio.chatVolume) || !writer.put_bool(audio.muteWhenUnfocused)
        || !writer.put_i8(audio.soundEffectsVolume) || !writer.put_i8(audio.dialogueVolume)
        || !writer.put_i8(audio.musicVolume) || !writer.put_i8(display.brightness)
        || !writer.put_bool(display.showFps) || !writer.put_i8(display.hdrMode)
        || !writer.put_u8(display.verticalSyncInterval)
        || !writer.put_i32(display.fieldOfView)
        || !writer.put_float(display.calibrationPrimary)
        || !writer.put_float(display.calibrationAlpha)
        || !writer.put_i8(interfaceSettings.subtitlesMode)
        || !writer.put_i8(interfaceSettings.colorblindMode)
        || !writer.put_i8(interfaceSettings.helmetMode)
        || !writer.put_i8(interfaceSettings.hudOpacity)
        || !writer.put_bool(interfaceSettings.displayHints)
        || !writer.put_i8(interfaceSettings.backgroundOpacity)
        || !writer.put_i8(interfaceSettings.reticleLocation)
        || !writer.put_i8(interfaceSettings.reticleColor)
        || !writer.put_i8(interfaceSettings.textSize)
        || !writer.put_i8(interfaceSettings.textColor)
        || !writer.put_i8(interfaceSettings.textBackgroundStyle)
        || !writer.put_i8(interfaceSettings.textBackgroundOpacity)
        || !writer.put_i8(interfaceSettings.reservedTextMode)
        || !writer.put_i8(interfaceSettings.subtitleOptionsEntry)
        || !writer.put_bool(social.preferGoodConnection)
        || !writer.put_i8(social.textChatMode) || !writer.put_bool(social.showRealNames)
        || !writer.put_bool(social.clanInviteNotifications)
        || !writer.put_bool(social.profanityFilter)
        || !writer.put_bool(social.voiceChatEnabled)
        || !writer.put_i8(social.whisperChatMode)
        || !writer.put_i8(social.teamChatJoinMode)
        || !writer.put_i8(social.localChatJoinMode)
        || !writer.put_i8(social.clanChatJoinMode)
        || !writer.put_i8(social.chatAutoHideMode)
        || !writer.put_u8(static_cast<std::uint8_t>(value.keyBindingSource))
        || !writer.put_bool(value.keyBindings.configured)) {
        return false;
    }
    for (const account::settings::bindings::Binding& binding : value.keyBindings.values) {
        if (!writer.put_optional_u16(binding.primary)
            || !writer.put_optional_u16(binding.secondary)) {
            return false;
        }
    }
    return writer.put_bool(value.configured);
}

/** Decodes settings only after the whole versioned payload is structurally complete. */
[[nodiscard]] inline SettingsDecodeResult
decode_settings(std::span<const std::byte> bytes,
                 account::settings::AccountSettings& output) noexcept {
    SettingsReader reader(bytes);
    account::settings::AccountSettings value{};
    std::uint32_t version = 0;
    std::uint8_t keyBindingSource = 0;
    auto& controls = value.controls;
    auto& audio = value.audio;
    auto& display = value.display;
    auto& interfaceSettings = value.interface;
    auto& social = value.social;
    if (!reader.get_u32(version)) {
        return SettingsDecodeResult::invalid;
    }
    if (version > kSettingsPayloadVersion) {
        return SettingsDecodeResult::incompatible;
    }
    if (version != kSettingsPayloadVersion || !reader.get_i8(controls.buttonLayout)
        || !reader.get_i8(controls.movementMode)
        || !reader.get_i8(controls.controllerLookSensitivity)
        || !reader.get_bool(controls.controllerInvertVertical)
        || !reader.get_bool(controls.controllerAutoLookCentering)
        || !reader.get_bool(controls.controllerVibration)
        || !reader.get_bool(controls.controllerSwapShoulders)
        || !reader.get_bool(controls.controllerInvertHorizontal)
        || !reader.get_i32(controls.mouseLookSensitivity)
        || !reader.get_bool(controls.mouseInvertVertical)
        || !reader.get_bool(controls.mouseInvertHorizontal)
        || !reader.get_bool(controls.unidentifiedToggle)
        || !reader.get_bool(controls.mouseAimSmoothing)
        || !reader.get_float(controls.adsSensitivityModifier)
        || !reader.get_i8(controls.doublePressDelay)
        || !reader.get_i8(audio.voiceOutputMode) || !reader.get_i8(audio.teamVoiceChannel)
        || !reader.get_i8(audio.reservedMode) || !reader.get_i8(audio.migrationVersion)
        || !reader.get_i8(audio.chatVolume) || !reader.get_bool(audio.muteWhenUnfocused)
        || !reader.get_i8(audio.soundEffectsVolume) || !reader.get_i8(audio.dialogueVolume)
        || !reader.get_i8(audio.musicVolume) || !reader.get_i8(display.brightness)
        || !reader.get_bool(display.showFps) || !reader.get_i8(display.hdrMode)
        || !reader.get_u8(display.verticalSyncInterval)
        || !reader.get_i32(display.fieldOfView)
        || !reader.get_float(display.calibrationPrimary)
        || !reader.get_float(display.calibrationAlpha)
        || !reader.get_i8(interfaceSettings.subtitlesMode)
        || !reader.get_i8(interfaceSettings.colorblindMode)
        || !reader.get_i8(interfaceSettings.helmetMode)
        || !reader.get_i8(interfaceSettings.hudOpacity)
        || !reader.get_bool(interfaceSettings.displayHints)
        || !reader.get_i8(interfaceSettings.backgroundOpacity)
        || !reader.get_i8(interfaceSettings.reticleLocation)
        || !reader.get_i8(interfaceSettings.reticleColor)
        || !reader.get_i8(interfaceSettings.textSize)
        || !reader.get_i8(interfaceSettings.textColor)
        || !reader.get_i8(interfaceSettings.textBackgroundStyle)
        || !reader.get_i8(interfaceSettings.textBackgroundOpacity)
        || !reader.get_i8(interfaceSettings.reservedTextMode)
        || !reader.get_i8(interfaceSettings.subtitleOptionsEntry)
        || !reader.get_bool(social.preferGoodConnection)
        || !reader.get_i8(social.textChatMode) || !reader.get_bool(social.showRealNames)
        || !reader.get_bool(social.clanInviteNotifications)
        || !reader.get_bool(social.profanityFilter)
        || !reader.get_bool(social.voiceChatEnabled)
        || !reader.get_i8(social.whisperChatMode)
        || !reader.get_i8(social.teamChatJoinMode)
        || !reader.get_i8(social.localChatJoinMode)
        || !reader.get_i8(social.clanChatJoinMode)
        || !reader.get_i8(social.chatAutoHideMode) || !reader.get_u8(keyBindingSource)
        || keyBindingSource
               > static_cast<std::uint8_t>(account::settings::KeyBindingSource::computer)
        || !reader.get_bool(value.keyBindings.configured)) {
        return SettingsDecodeResult::invalid;
    }
    value.keyBindingSource = static_cast<account::settings::KeyBindingSource>(keyBindingSource);
    for (account::settings::bindings::Binding& binding : value.keyBindings.values) {
        if (!reader.get_optional_u16(binding.primary)
            || !reader.get_optional_u16(binding.secondary)) {
            return SettingsDecodeResult::invalid;
        }
    }
    if (!reader.get_bool(value.configured) || !reader.complete()) {
        return SettingsDecodeResult::invalid;
    }
    output = value;
    return SettingsDecodeResult::decoded;
}

} // namespace sunrise::state::persistence::sqlite::detail
