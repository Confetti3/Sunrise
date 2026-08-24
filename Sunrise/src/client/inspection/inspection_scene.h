#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "world_inspection_model.h"

namespace sunrise::client::inspection {

struct SceneVertex final {
    std::array<float, 3> position{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SceneLine final {
    SceneVertex first{};
    SceneVertex second{};
    NodeId owner{};
    std::uint32_t pickToken{};
    bool selected{};
    bool hovered{};
    bool alwaysVisible{};
};

enum class HelperGlyph : std::uint8_t {
    unknown,
    spawn,
    runtimeEntity,
    spawnRule,
    squad,
    trigger,
    spatial,
    objective,
    device,
    interactable,
    action,
    target,
    condition,
    competitive,
    audio,
    physics,
    geometry,
};

enum class HelperMarkerShape : std::uint8_t {
    circle,
    square,
};

[[nodiscard]] HelperMarkerShape helper_marker_shape(HelperGlyph glyph) noexcept;
[[nodiscard]] std::array<float, 4> helper_role_color(HelperGlyph glyph) noexcept;
/** Fixed cyan used for spatial volumes independently of their semantic role. */
[[nodiscard]] std::array<float, 4> helper_volume_color() noexcept;

/** Constant-screen helper instance. The renderer expands it at its exact depth. */
struct SceneGlyph final {
    std::array<float, 3> position{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    NodeId owner{};
    std::uint32_t pickToken{};
    HelperGlyph glyph{HelperGlyph::unknown};
    HelperMarkerShape shape{HelperMarkerShape::square};
    float sizePixels{19.0F};
    bool selected{};
    bool hovered{};
    bool alwaysVisible{};
};

inline constexpr std::size_t kMaximumSceneLines = 12288;

enum class OverlayDetail : std::uint8_t {
    selectedOnly = 0,
    selectedNearby = 1,
    all = 2,
    adaptive = 3,
    cameraNearby = 4,
};

enum class PresentationCullReason : std::uint8_t {
    none,
    category,
    hidden,
    detail,
    distance,
    offscreen,
    representation,
    density,
    budget,
};

struct OverlayPolicy final {
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showLabels{};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showAuthoredOrientation{true};
    OverlayDetail detail{OverlayDetail::adaptive};
    std::uint32_t maximumVisibleNodes{320};
    float nearbyRadius{100.0F};
    float glyphSizePixels{19.0F};
    float lineWidthPixels{2.4F};
    float baseOpacity{0.75F};
    float focusContextOpacity{0.50F};
};

struct OverlayStats final {
    std::size_t lines{};
    std::size_t nodesWithLines{};
    std::size_t shownNodes{};
    std::size_t categoryFilteredNodes{};
    std::size_t hiddenNodes{};
    std::size_t detailFilteredNodes{};
    std::size_t viewFilteredNodes{};
    std::size_t representationFilteredNodes{};
    std::size_t densityFilteredNodes{};
    std::size_t distanceFilteredNodes{};
    std::size_t focusContextNodes{};
    std::size_t omittedNodes{};
    std::size_t partialNodes{};
    bool truncated{};
};

enum class DepthCapability : std::uint8_t {
    unavailable,
    supported,
    invalidDimensions,
    singleSample,
    unsupportedSamples,
    unsupportedFormat,
    captureFailed,
};

[[nodiscard]] const char* depth_capability_name(DepthCapability capability) noexcept;

struct SceneFrame final {
    std::vector<SceneLine> lines;
    std::vector<SceneGlyph> glyphs;
    struct PresentationEntry final {
        NodeId node{};
        HelperGlyph glyph{HelperGlyph::unknown};
        HelperMarkerShape shape{HelperMarkerShape::square};
        std::array<float, 3> center{};
        std::array<float, 4> color{};
        float screenX{};
        float screenY{};
        float depth{};
        bool hasBounds{};
        bool selected{};
        bool hovered{};
        bool focusContext{};
        std::uint8_t priority{};
        PresentationCullReason cullingReason{PresentationCullReason::none};
    };
    std::vector<PresentationEntry> presentation;
    struct PickEntry final {
        std::uint32_t token{};
        NodeId node{};
    };
    std::vector<PickEntry> picks;
    OverlayStats stats;
    float lineWidthPixels{2.4F};
    std::uint64_t sequence{};
    std::uint64_t sourceFrame{};
    std::uint32_t graphGeneration{};
};

using SceneFramePtr = std::shared_ptr<const SceneFrame>;

enum class HelperBackend : std::uint8_t {
    none,
    sunriseDepth,
};

enum class HelperFailureReason : std::uint8_t {
    none,
    depthUnavailable,
    exactViewUnavailable,
    drawFailed,
};

enum class PickingReadiness : std::uint8_t {
    unavailable,
    waitingForView,
    waitingForReadback,
    ready,
};

/** Exact captured-frame outcome for the one geometry backend selected that frame. */
struct HelperRenderStatus final {
    std::size_t submittedLines{};
    std::size_t omittedLines{};
    std::size_t submittedGlyphs{};
    std::size_t omittedGlyphs{};
    DepthCapability capability{DepthCapability::unavailable};
    HelperBackend backend{HelperBackend::none};
    HelperFailureReason failureReason{HelperFailureReason::none};
    PickingReadiness picking{PickingReadiness::unavailable};
    std::uint64_t capturedFrame{};
    std::uint64_t batchSequence{};
    std::uint64_t engineFrame{};
    std::uint64_t viewPublication{};
    std::uint64_t depthSequence{};
    std::uint64_t depthCaptureMicros{};
    std::uint32_t graphGeneration{};
    bool depthAvailable{};
    bool drawn{};
};

using DepthRenderStatus = HelperRenderStatus;

struct RenderViewport final {
    float x{};
    float y{};
    float width{};
    float height{};
    float minimumDepth{};
    float maximumDepth{1.0F};
};

/** Native main-view values copied at the proven debug-render boundary. */
struct RenderViewSnapshot final {
    std::array<float, 16> viewProjection{};
    RenderViewport viewport{};
    std::uint64_t engineFrame{};
    std::uint64_t publication{};
    std::uint64_t observedAtTick{};
    bool exactNative{};
    bool valid{};
};

struct PickRequest final {
    std::uint64_t sequence{};
    std::uint64_t capturedFrame{};
    std::uint64_t engineFrame{};
    std::uint64_t viewPublication{};
    std::uint64_t depthSequence{};
    std::uint32_t graphGeneration{};
    std::uint32_t x{};
    std::uint32_t y{};
};

struct PickResult final {
    std::uint64_t requestSequence{};
    std::uint64_t capturedFrame{};
    std::uint64_t engineFrame{};
    std::uint64_t viewPublication{};
    std::uint64_t depthSequence{};
    std::uint32_t graphGeneration{};
    NodeId node{};
    bool ready{};
};

struct PickCoordinate final {
    std::uint32_t x{};
    std::uint32_t y{};
    bool valid{};
};

/** Lock-free renderer-neutral GPU-picking mailboxes. */
void publish_pick_request(const PickRequest& request) noexcept;
[[nodiscard]] PickRequest pick_request() noexcept;
void publish_pick_result(const PickResult& result) noexcept;
[[nodiscard]] PickResult pick_result() noexcept;
void clear_pick_state() noexcept;

[[nodiscard]] bool overlay_category_enabled(NodeKind kind, const OverlayPolicy& policy) noexcept;
[[nodiscard]] const char* helper_backend_name(HelperBackend backend) noexcept;
[[nodiscard]] const char* helper_failure_name(HelperFailureReason reason) noexcept;
[[nodiscard]] const char* picking_readiness_name(PickingReadiness readiness) noexcept;
[[nodiscard]] const char* overlay_detail_name(OverlayDetail detail) noexcept;
[[nodiscard]] const char* helper_glyph_name(HelperGlyph glyph) noexcept;
[[nodiscard]] bool helper_geometry_current(const HelperRenderStatus& status,
                                           std::uint64_t capturedFrame) noexcept;
[[nodiscard]] bool pick_result_current(const PickResult& result,
                                       std::uint64_t requestSequence,
                                       std::uint64_t capturedFrame,
                                       std::uint64_t engineFrame,
                                       std::uint64_t viewPublication,
                                       std::uint64_t depthSequence,
                                       std::uint32_t graphGeneration) noexcept;
/** Accepts the bounded asynchronous result produced after a specific request. */
[[nodiscard]] bool pick_result_fresh(const PickResult& result,
                                     const PickRequest& request,
                                     std::uint64_t currentCapturedFrame,
                                     std::uint64_t currentEngineFrame,
                                     std::uint64_t currentViewPublication,
                                     std::uint64_t currentDepthSequence,
                                     std::uint32_t currentGraphGeneration) noexcept;
void assign_pick_tokens(SceneFrame& frame);
[[nodiscard]] NodeId resolve_pick_token(const SceneFrame& frame, std::uint32_t token) noexcept;
[[nodiscard]] PickCoordinate map_pick_coordinate(float cursorX,
                                                 float cursorY,
                                                 float imageMinimumX,
                                                 float imageMinimumY,
                                                 float imageMaximumX,
                                                 float imageMaximumY,
                                                 std::uint32_t sourceWidth,
                                                 std::uint32_t sourceHeight) noexcept;
[[nodiscard]] PickCoordinate map_pick_coordinate_to_viewport(float cursorX,
                                                             float cursorY,
                                                             float imageMinimumX,
                                                             float imageMinimumY,
                                                             float imageMaximumX,
                                                             float imageMaximumY,
                                                             const RenderViewport& viewport,
                                                             std::uint32_t sourceWidth,
                                                             std::uint32_t sourceHeight) noexcept;

} // namespace sunrise::client::inspection
