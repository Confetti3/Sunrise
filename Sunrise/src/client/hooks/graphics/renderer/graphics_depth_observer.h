#pragma once

#include <Windows.h>

#include <cstdint>
#include <d3d11.h>

#include "../../../inspection/inspection_scene.h"

namespace sunrise::client::hooks::graphics::renderer::depth_observer {

inline constexpr std::uint32_t kProofFrameCount = 300;
inline constexpr std::uint32_t kRequiredQualifyingFrames = 285;
inline constexpr std::uint32_t kRequiredMatrixHashes = 30;
inline constexpr std::uint32_t kIdentityCapacity = 4;
inline constexpr bool kCaptureReleaseEnabled = true;

enum class Convention : std::uint8_t {
    unknown,
    standard,
    reversed,
};

enum class Failure : std::uint8_t {
    none,
    hooksUnavailable,
    contextVtableMismatch,
    noExactView,
    noCandidate,
    unsupportedDescriptor,
    viewportMismatch,
    insufficientFrames,
    insufficientFrequency,
    insufficientMatrices,
    conventionUnknown,
    conventionInconsistent,
    ambiguous,
    observationsDropped,
    observerOnly,
};

/** Copied descriptor-class key. It owns no D3D object. */
struct Descriptor final {
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t sampleCount{};
    std::uint32_t sampleQuality{};
    bool singleSliceMipZero{};
};

/** Pure proof inputs shared by runtime selection and external verification. */
struct ProofEvidence final {
    Descriptor descriptor{};
    std::uint32_t observedFrames{};
    std::uint32_t qualifyingFrames{};
    std::uint32_t distinctMatrixHashes{};
    std::uint32_t eligibleClassCount{};
    float clearDepth{};
    D3D11_COMPARISON_FUNC comparison{D3D11_COMPARISON_NEVER};
    bool selectedDevice{};
    bool viewportMatches{};
    bool clearConsistent{};
    bool comparisonConsistent{};
};

/**
 * Reports whether a typeless/typed D3D format belongs to a supported depth family.
 * @param format D3D resource or view format.
 * @return True for supported depth-stencil families.
 */
[[nodiscard]] bool supported_depth_format(DXGI_FORMAT format) noexcept;
/**
 * Compares a copied descriptor with an exact native-view viewport.
 * @param descriptor Copied depth resource descriptor class.
 * @param viewport Exact render viewport observed for the frame.
 * @return True when dimensions, sampling, and slice constraints match.
 */
[[nodiscard]] bool descriptor_matches_viewport(const Descriptor& descriptor,
                                               const inspection::RenderViewport& viewport) noexcept;
/**
 * Classifies depth direction from a clear value and comparison function.
 * @param clearDepth Observed clear value.
 * @param comparison Observed depth comparison function.
 * @return Standard, reversed, or unknown convention.
 */
[[nodiscard]] Convention classify_convention(float clearDepth,
                                             D3D11_COMPARISON_FUNC comparison) noexcept;
/**
 * Applies the complete depth-selection proof gate.
 * @param evidence Copied evidence for one descriptor class.
 * @return True only when all frequency, identity, viewport, and convention proofs pass.
 */
[[nodiscard]] bool proof_eligible(const ProofEvidence& evidence) noexcept;

/** Coherent copied report. No field is a retained engine or D3D pointer. */
struct Status final {
    Descriptor descriptor{};
    std::uint64_t renderTargetBindings{};
    std::uint64_t depthStateChanges{};
    std::uint64_t depthClears{};
    std::uint64_t presentFrames{};
    std::uint64_t lastObservationTick{};
    std::uint64_t lastEngineFrame{};
    std::uint64_t lastViewPublication{};
    std::uint64_t droppedObservations{};
    std::uint64_t candidateSequence{};
    std::uint64_t captureSequence{};
    std::uint32_t observedFrames{};
    std::uint32_t qualifyingFrames{};
    std::uint32_t distinctMatrixHashes{};
    std::uint32_t currentDistinctMatrixHashes{};
    std::uint32_t descriptorClassCount{};
    std::uint32_t eligibleClassCount{};
    std::uint32_t resourceIdentityCount{};
    std::uint32_t selectedThreadId{};
    float clearDepth{};
    D3D11_COMPARISON_FUNC comparison{D3D11_COMPARISON_NEVER};
    Convention convention{Convention::unknown};
    Failure failure{Failure::hooksUnavailable};
    bool hooksResolved{};
    bool selectedDevice{};
    bool exactView{};
    bool viewportMatches{};
    bool clearConsistent{};
    bool comparisonConsistent{};
    bool ambiguous{};
    bool eligible{};
    bool captureEnabled{};
    bool captureRequested{};
};

/** One mailbox transfer. The caller owns one reference in @p view. */
struct CapturedDepth final {
    ID3D11DepthStencilView* view{};
    Descriptor descriptor{};
    Convention convention{Convention::unknown};
    std::uint64_t sequence{};
};

/**
 * Excludes Sunrise-owned context calls made after the Present cutoff. The context
 * vtable hooks also see calls made by this DLL, so proof/capture must fence them out.
 */
class InternalCallScope final {
public:
    InternalCallScope() noexcept;
    ~InternalCallScope() noexcept;

    InternalCallScope(const InternalCallScope&) = delete;
    InternalCallScope& operator=(const InternalCallScope&) = delete;
};

/**
 * Selects renderer-owned identities after the production vtable is validated.
 * @param device Renderer-owned D3D device identity; not retained with added ownership.
 * @param immediateContext Renderer-owned immediate-context identity.
 * @param contextVtableMatches True only after the hooked production vtable was verified.
 */
void select_device(ID3D11Device* device,
                   ID3D11DeviceContext* immediateContext,
                   bool contextVtableMatches) noexcept;
/** Stops admission before renderer-owned D3D references are released. */
void clear_selection() noexcept;
/** Clears all copied evidence. Safe after hook removal and at unload. */
void reset() noexcept;
/**
 * Enables mailbox ownership only while visible depth helpers need it.
 * @param requested True to admit one-reference capture publications.
 */
void set_capture_requested(bool requested) noexcept;
/**
 * Transfers the newest captured reference without blocking the Present path.
 * @param captured Destination that receives ownership of one view reference.
 * @return True when a newer capture was available.
 */
[[nodiscard]] bool take_latest(CapturedDepth& captured) noexcept;
/**
 * Releases a previously transferred view reference and clears its metadata.
 * @param captured Capture previously returned by take_latest.
 */
void release_captured(CapturedDepth& captured) noexcept;

/**
 * Observes a render-target binding made through the validated immediate context.
 * @param context Calling D3D context identity.
 * @param depthView Bound depth-stencil view, or null.
 */
void observe_render_targets(ID3D11DeviceContext* context,
                            ID3D11DepthStencilView* depthView) noexcept;
/**
 * Observes a depth-state change made through the validated immediate context.
 * @param context Calling D3D context identity.
 * @param state Bound depth-stencil state, or null for defaults.
 */
void observe_depth_state(ID3D11DeviceContext* context, ID3D11DepthStencilState* state) noexcept;
/**
 * Observes a depth clear made through the validated immediate context.
 * @param context Calling D3D context identity.
 * @param depthView Cleared depth-stencil view.
 * @param clearFlags D3D clear-mask flags.
 * @param depth Depth clear value.
 */
void observe_depth_clear(ID3D11DeviceContext* context,
                         ID3D11DepthStencilView* depthView,
                         UINT clearFlags,
                         FLOAT depth) noexcept;

/**
 * Freezes the final qualifying binding for one exact native view.
 * @param view Pointer-free exact-view evidence published before Present.
 */
void sample_present(const inspection::RenderViewSnapshot& view) noexcept;
/** @return One coherent copied depth-observer report. */
[[nodiscard]] Status status() noexcept;

/**
 * Returns the stable depth-convention label used by diagnostics.
 * @param convention Convention to describe.
 * @return Static null-terminated label.
 */
[[nodiscard]] const char* convention_name(Convention convention) noexcept;
/**
 * Returns the stable depth-failure label used by diagnostics.
 * @param failure Failure to describe.
 * @return Static null-terminated label.
 */
[[nodiscard]] const char* failure_name(Failure failure) noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::depth_observer
