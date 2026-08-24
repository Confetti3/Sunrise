#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../inspection/inspection_scene.h"

namespace sunrise::client::hooks::graphics::renderer::native_debug {

enum class ObserverFailure : std::uint8_t {
    none,
    snapshotSignature,
    boundarySignature,
    callSiteSignature,
    frameCounterSignature,
    callRelationship,
    detourAttach,
    detourDetach,
};

/** Copied passive evidence from the native per-view debug-render boundary. */
struct ObserverStatus final {
    std::array<float, 16> matrix{};
    std::array<float, 4> normalizedViewport{};
    inspection::RenderViewport viewport{};
    std::uint64_t calls{};
    std::uint64_t acceptedSamples{};
    std::uint64_t rejectedSamples{};
    std::uint64_t concurrentSamples{};
    std::uint64_t sequence{};
    std::uint64_t lastSeenTick{};
    std::uint64_t engineFrame{};
    std::uint64_t matrixHash{};
    std::uint64_t pairedFrames{};
    std::uint64_t equalMatrixFrames{};
    std::uint64_t differentMatrixFrames{};
    std::uint32_t threadId{};
    std::uint32_t observedThreadCount{};
    std::uint32_t viewShapeCount{};
    std::uint32_t viewIndex{};
    std::uint32_t viewCount{};
    std::uint32_t passFlags{};
    std::uint16_t framebufferWidth{};
    std::uint16_t framebufferHeight{};
    ObserverFailure failure{ObserverFailure::none};
    bool signaturesValid{};
    bool installed{};
    bool observed{};
    bool fullViewport{};
};

/**
 * Attaches the read-only 86657 per-view observer. A signature miss is local and fail-closed.
 * @return True when every observer target was proven and the replacement attached.
 */
[[nodiscard]] bool install_observer() noexcept;

/**
 * Detaches the observer only after its replacement and copied publication are idle.
 * @return True when admission stopped, calls drained, and the replacement detached.
 */
[[nodiscard]] bool uninstall_observer() noexcept;

/** @return One coherent copied observer snapshot with no retained native pointer. */
[[nodiscard]] ObserverStatus observer_status() noexcept;

/**
 * Returns the stable observer-failure label used by logs and diagnostics.
 * @param failure Observer failure to describe.
 * @return Static null-terminated label.
 */
[[nodiscard]] const char* observer_failure_name(ObserverFailure failure) noexcept;

/**
 * Validates an observed matrix and converts its normalized viewport to pixels.
 * @param matrix Copied native view-projection matrix.
 * @param normalizedViewport Native left, top, right, and bottom coordinates.
 * @param framebufferWidth Current framebuffer width in pixels.
 * @param framebufferHeight Current framebuffer height in pixels.
 * @param viewport Destination for the converted pixel viewport.
 * @return True when all values are finite, ordered, and within the framebuffer.
 */
[[nodiscard]] bool make_observed_view(const std::array<float, 16>& matrix,
                                      const std::array<float, 4>& normalizedViewport,
                                      std::uint16_t framebufferWidth,
                                      std::uint16_t framebufferHeight,
                                      inspection::RenderViewport& viewport) noexcept;

/**
 * Converts Destiny's row-vector render matrix into Sunrise's canonical
 * column-vector matrix. The native debug path evaluates position * matrix,
 * while the CPU overlay and HLSL shaders evaluate matrix * position.
 * @param nativeMatrix Copied row-vector matrix from the native view.
 * @return Equivalent column-vector matrix used by Sunrise rendering.
 */
[[nodiscard]] std::array<float, 16>
canonical_view_projection(const std::array<float, 16>& nativeMatrix) noexcept;

/**
 * Publishes copied view lanes through the native-boundary seqlock.
 * @param view Pointer-free render-view snapshot to publish.
 */
void publish_view(const inspection::RenderViewSnapshot& view) noexcept;
/** @return Latest coherent pointer-free render-view snapshot. */
[[nodiscard]] inspection::RenderViewSnapshot view() noexcept;

/**
 * Rejects missing, future-dated, or stale native-view publications.
 * @param candidate Copied native-view publication to validate.
 * @param currentTick Current monotonic GetTickCount64 value.
 * @param maximumAge Maximum accepted publication age in milliseconds.
 * @return True only for a complete exact view observed within @p maximumAge.
 */
[[nodiscard]] bool view_current(const inspection::RenderViewSnapshot& candidate,
                                std::uint64_t currentTick,
                                std::uint64_t maximumAge = 100U) noexcept;

/** Clears passive observations and any installed bridge state on close/unload. */
void reset() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::native_debug
