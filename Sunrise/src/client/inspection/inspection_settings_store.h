#pragma once

#include <cstdint>

namespace sunrise::client::inspection::settings {

inline constexpr float kDefaultLeftWidth = 300.0F;
inline constexpr float kMinimumLeftWidth = 220.0F;
inline constexpr float kMaximumLeftWidth = 520.0F;
inline constexpr float kDefaultRightWidth = 350.0F;
inline constexpr float kMinimumRightWidth = 260.0F;
inline constexpr float kMaximumRightWidth = 620.0F;
inline constexpr float kDefaultBottomHeight = 210.0F;
inline constexpr float kMinimumBottomHeight = 120.0F;
inline constexpr float kMaximumBottomHeight = 480.0F;
inline constexpr std::uint32_t kDefaultMaximumVisibleNodes = 320;
inline constexpr float kDefaultNearbyRadius = 100.0F;
inline constexpr float kDefaultGlyphSizePixels = 19.0F;
inline constexpr float kMinimumGlyphSizePixels = 6.0F;
inline constexpr float kMaximumGlyphSizePixels = 24.0F;
inline constexpr float kDefaultLineWidthPixels = 2.4F;
inline constexpr float kDefaultBaseOpacity = 0.75F;
inline constexpr float kDefaultFocusContextOpacity = 0.50F;

struct Settings final {
    float leftWidth{kDefaultLeftWidth};
    float rightWidth{kDefaultRightWidth};
    float bottomHeight{kDefaultBottomHeight};
    std::uint8_t overlayDetail{3};
    std::uint32_t maximumVisibleNodes{kDefaultMaximumVisibleNodes};
    float nearbyRadius{kDefaultNearbyRadius};
    float glyphSizePixels{kDefaultGlyphSizePixels};
    float lineWidthPixels{kDefaultLineWidthPixels};
    float baseOpacity{kDefaultBaseOpacity};
    float focusContextOpacity{kDefaultFocusContextOpacity};
    bool bottomCollapsed{};
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showAuthoredOrientation{true};
    bool showLabels{};
};

/** Loads schema-versioned inspector.json settings or creates defaults when absent. */
void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Settings get() noexcept;
[[nodiscard]] bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::inspection::settings
