#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../hooking/detour.h"
#include "../../../patterns/image_scan.h"
#include "../../../patterns/signature_text.h"
#include "native_debug_renderer.h"

namespace sunrise::client::hooks::graphics::renderer::native_debug {
namespace {

constexpr std::string_view kSnapshotText =
    "48 8B 0B E8 ? ? ? ? 48 8B 13 48 8D 8B 30 9A 03 00 48 89 83 10 6F 03 00";
constexpr auto kSnapshot =
    patterns::signature<patterns::signature_length(kSnapshotText)>(kSnapshotText);
constexpr std::string_view kBoundaryText =
    "4C 8B DC 55 53 49 8D AB 58 FF FF FF 48 81 EC 98 01 00 00 48 8B 05 ? ? ? ? "
    "48 33 C4 48 89 45 F0 F3 0F 10 05 ? ? ? ? 49 89 73 10 49 8B F0 49 89 7B 20 "
    "48 8B F9";
constexpr auto kBoundary =
    patterns::signature<patterns::signature_length(kBoundaryText)>(kBoundaryText);
constexpr std::string_view kCallSiteText =
    "4D 8B C4 49 8B 8F 10 6F 03 00 49 8B D7 0F 29 44 24 40 E8 ? ? ? ? "
    "49 81 C6 D0 2B 00 00";
constexpr auto kCallSite =
    patterns::signature<patterns::signature_length(kCallSiteText)>(kCallSiteText);
constexpr std::string_view kFrameCounterGetterText =
    "8B 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC CC 4C 8B DC 48 83 EC 48";
constexpr auto kFrameCounterGetter =
    patterns::signature<patterns::signature_length(kFrameCounterGetterText)>(
        kFrameCounterGetterText);

constexpr std::size_t kViewportOffset = 0x500;
constexpr std::size_t kFramebufferWidthOffset = 0x626;
constexpr std::size_t kFramebufferHeightOffset = 0x628;
constexpr std::size_t kMatrixOffset = 0x800;
constexpr std::size_t kViewArrayOffset = 0x425C8;
constexpr std::size_t kViewCountOffset = 0x425D0;
constexpr std::size_t kViewStride = 0x2BD0;
constexpr std::uint32_t kMaximumViewCount = 256;
constexpr std::size_t kViewShapeCapacity = 32;
constexpr std::size_t kCallOpcodeOffset = 0x12;
constexpr std::size_t kCallOperandOffset = kCallOpcodeOffset + 1;
constexpr std::size_t kCallEndOffset = kCallOpcodeOffset + 5;
constexpr std::uint32_t kMaximumTransitionLogs = 64;

using Boundary = void(__fastcall*)(
    void*, void*, void*, const std::byte*, const void*, const void*, const float*, std::uint32_t);

struct ObserverMailbox final {
    std::atomic_uint64_t lockSequence{};
    std::array<std::atomic_uint32_t, 16> matrix{};
    std::array<std::atomic_uint32_t, 4> normalizedViewport{};
    std::array<std::atomic_uint32_t, 6> viewport{};
    std::atomic_uint64_t publication{};
    std::atomic_uint64_t lastSeenTick{};
    std::atomic_uint64_t engineFrame{};
    std::atomic_uint64_t matrixHash{};
    std::atomic_uint32_t threadId{};
    std::atomic_uint32_t viewIndex{};
    std::atomic_uint32_t viewCount{};
    std::atomic_uint32_t passFlags{};
    std::atomic_uint16_t framebufferWidth{};
    std::atomic_uint16_t framebufferHeight{};
    std::atomic_bool fullViewport{};
    std::atomic_bool observed{};
};

struct PairSample final {
    std::atomic_uint64_t matrixHash{};
    std::atomic_uint64_t engineFrame{(std::numeric_limits<std::uint64_t>::max)()};
    std::atomic_uint32_t viewCount{};
};

hooking::detour::Handle g_handle{};
std::atomic<Boundary> g_original{};
std::atomic_bool g_installPublishing{};
std::atomic_bool g_installed{};
std::atomic_bool g_stopping{};
std::atomic_bool g_signaturesValid{};
std::atomic<ObserverFailure> g_failure{ObserverFailure::none};
std::atomic_uint g_replacementInFlight{};
std::atomic_flag g_mailboxWriter = ATOMIC_FLAG_INIT;
ObserverMailbox g_mailbox{};
std::array<PairSample, 2> g_pairSamples{};
const std::uint64_t* g_frameCounter{};
std::atomic_uint64_t g_calls{};
std::atomic_uint64_t g_acceptedSamples{};
std::atomic_uint64_t g_rejectedSamples{};
std::atomic_uint64_t g_concurrentSamples{};
std::atomic_uint32_t g_firstThreadId{};
std::atomic_uint64_t g_threadMask{};
std::atomic_uint32_t g_viewShapeCount{};
std::array<std::atomic_uint64_t, kViewShapeCapacity> g_viewShapes{};
std::atomic_uint32_t g_transitionLogs{};
std::atomic_uint64_t g_lastComparedFrame{(std::numeric_limits<std::uint64_t>::max)()};
std::atomic_uint64_t g_pairedFrames{};
std::atomic_uint64_t g_equalMatrixFrames{};
std::atomic_uint64_t g_differentMatrixFrames{};
std::atomic_uint64_t g_primaryPublications{};

struct ReplacementScope final {
    ReplacementScope() noexcept {
        g_replacementInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ReplacementScope() {
        g_replacementInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;
};

[[nodiscard]] std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float lane(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}

template <typename T> [[nodiscard]] T read(const std::byte* address) noexcept {
    T value{};
    std::memcpy(&value, address, sizeof value);
    return value;
}

[[nodiscard]] bool replacement_idle() noexcept {
    return g_replacementInFlight.load(std::memory_order_acquire) == 0;
}

[[nodiscard]] Boundary original() noexcept {
    Boundary next = g_original.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = g_original.load(std::memory_order_acquire);
    }
    return next;
}

void set_failure(ObserverFailure failure, const char* stage) noexcept {
    g_failure.store(failure, std::memory_order_release);
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=native_render_observer stage=%s result=fail reason=%s",
                                      stage,
                                      observer_failure_name(failure));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void clear_observations() noexcept {
    g_calls.store(0, std::memory_order_release);
    g_acceptedSamples.store(0, std::memory_order_release);
    g_rejectedSamples.store(0, std::memory_order_release);
    g_concurrentSamples.store(0, std::memory_order_release);
    g_firstThreadId.store(0, std::memory_order_release);
    g_threadMask.store(0, std::memory_order_release);
    g_viewShapeCount.store(0, std::memory_order_release);
    for (std::atomic_uint64_t& shape : g_viewShapes) {
        shape.store(0, std::memory_order_release);
    }
    g_transitionLogs.store(0, std::memory_order_release);
    g_lastComparedFrame.store((std::numeric_limits<std::uint64_t>::max)(),
                              std::memory_order_release);
    g_pairedFrames.store(0, std::memory_order_release);
    g_equalMatrixFrames.store(0, std::memory_order_release);
    g_differentMatrixFrames.store(0, std::memory_order_release);
    g_primaryPublications.store(0, std::memory_order_release);
    for (PairSample& sample : g_pairSamples) {
        sample.matrixHash.store(0, std::memory_order_release);
        sample.engineFrame.store((std::numeric_limits<std::uint64_t>::max)(),
                                 std::memory_order_release);
        sample.viewCount.store(0, std::memory_order_release);
    }
    g_mailbox.lockSequence.fetch_add(1, std::memory_order_acq_rel);
    for (std::atomic_uint32_t& value : g_mailbox.matrix) {
        value.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint32_t& value : g_mailbox.normalizedViewport) {
        value.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint32_t& value : g_mailbox.viewport) {
        value.store(0, std::memory_order_relaxed);
    }
    g_mailbox.publication.store(0, std::memory_order_relaxed);
    g_mailbox.lastSeenTick.store(0, std::memory_order_relaxed);
    g_mailbox.engineFrame.store(0, std::memory_order_relaxed);
    g_mailbox.matrixHash.store(0, std::memory_order_relaxed);
    g_mailbox.threadId.store(0, std::memory_order_relaxed);
    g_mailbox.viewIndex.store(0, std::memory_order_relaxed);
    g_mailbox.viewCount.store(0, std::memory_order_relaxed);
    g_mailbox.passFlags.store(0, std::memory_order_relaxed);
    g_mailbox.framebufferWidth.store(0, std::memory_order_relaxed);
    g_mailbox.framebufferHeight.store(0, std::memory_order_relaxed);
    g_mailbox.fullViewport.store(false, std::memory_order_relaxed);
    g_mailbox.observed.store(false, std::memory_order_relaxed);
    g_mailbox.lockSequence.fetch_add(1, std::memory_order_release);
    g_mailboxWriter.clear(std::memory_order_release);
}

[[nodiscard]] std::uint64_t shape_hash(const std::array<float, 4>& normalizedViewport,
                                       std::uint16_t width,
                                       std::uint16_t height,
                                       std::uint32_t passFlags,
                                       std::uint32_t viewIndex,
                                       std::uint32_t viewCount) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    for (float value : normalizedViewport) {
        append(bits(value));
    }
    append(static_cast<std::uint32_t>(width) | (static_cast<std::uint32_t>(height) << 16U));
    append(passFlags);
    append(viewIndex);
    append(viewCount);
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::uint64_t matrix_hash(const std::array<float, 16>& matrix) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (float value : matrix) {
        const std::uint32_t valueBits = bits(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (valueBits >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool identity_matrix(const std::array<float, 16>& matrix) noexcept {
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        const float expected = index == 0 || index == 5 || index == 10 || index == 15 ? 1.0F : 0.0F;
        if (std::fabs(matrix[index] - expected) > 1.0e-6F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool full_viewport(const std::array<float, 4>& viewport) noexcept {
    return std::fabs(viewport[0]) <= 0.0001F && std::fabs(viewport[1]) <= 0.0001F
           && std::fabs(viewport[2] - 1.0F) <= 0.0001F && std::fabs(viewport[3] - 1.0F) <= 0.0001F;
}

void compare_primary_pair(std::uint32_t viewIndex,
                          std::uint32_t viewCount,
                          std::uint64_t engineFrame,
                          std::uint64_t matrixHash) noexcept {
    if (viewIndex >= g_pairSamples.size()) {
        return;
    }
    PairSample& own = g_pairSamples[viewIndex];
    own.matrixHash.store(matrixHash, std::memory_order_relaxed);
    own.viewCount.store(viewCount, std::memory_order_relaxed);
    own.engineFrame.store(engineFrame, std::memory_order_release);
    const PairSample& other = g_pairSamples[1U - viewIndex];
    if (other.engineFrame.load(std::memory_order_acquire) != engineFrame
        || other.viewCount.load(std::memory_order_relaxed) != viewCount) {
        return;
    }
    std::uint64_t compared = g_lastComparedFrame.load(std::memory_order_acquire);
    while (compared != engineFrame) {
        if (g_lastComparedFrame.compare_exchange_weak(
                compared, engineFrame, std::memory_order_acq_rel, std::memory_order_acquire)) {
            g_pairedFrames.fetch_add(1, std::memory_order_acq_rel);
            if (other.matrixHash.load(std::memory_order_relaxed) == matrixHash) {
                g_equalMatrixFrames.fetch_add(1, std::memory_order_acq_rel);
            } else {
                g_differentMatrixFrames.fetch_add(1, std::memory_order_acq_rel);
            }
            return;
        }
    }
}

[[nodiscard]] bool first_view_shape(std::uint64_t shape) noexcept {
    for (std::atomic_uint64_t& entry : g_viewShapes) {
        std::uint64_t existing = entry.load(std::memory_order_acquire);
        if (existing == shape) {
            return false;
        }
        if (existing == 0
            && entry.compare_exchange_strong(
                existing, shape, std::memory_order_acq_rel, std::memory_order_acquire)) {
            g_viewShapeCount.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        if (existing == shape) {
            return false;
        }
    }
    return false;
}

void report_transition(std::uint32_t threadId,
                       std::uint32_t passFlags,
                       std::uint16_t width,
                       std::uint16_t height,
                       std::uint32_t viewIndex,
                       std::uint32_t viewCount,
                       std::uint64_t engineFrame,
                       std::uint64_t matrixHash,
                       const std::array<float, 4>& normalizedViewport,
                       const inspection::RenderViewport& viewport) noexcept {
    if (g_transitionLogs.fetch_add(1, std::memory_order_acq_rel) >= kMaximumTransitionLogs) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=native_render_observer stage=view result=observed thread=%u flags=0x%X "
                      "view=%u/%u frame=%llu matrix=0x%016llX framebuffer=%ux%u "
                      "normalized=%.4f,%.4f,%.4f,%.4f "
                      "viewport=%.1f,%.1f,%.1f,%.1f",
                      threadId,
                      passFlags,
                      viewIndex,
                      viewCount,
                      static_cast<unsigned long long>(engineFrame),
                      static_cast<unsigned long long>(matrixHash),
                      static_cast<unsigned>(width),
                      static_cast<unsigned>(height),
                      static_cast<double>(normalizedViewport[0]),
                      static_cast<double>(normalizedViewport[1]),
                      static_cast<double>(normalizedViewport[2]),
                      static_cast<double>(normalizedViewport[3]),
                      static_cast<double>(viewport.x),
                      static_cast<double>(viewport.y),
                      static_cast<double>(viewport.width),
                      static_cast<double>(viewport.height));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void publish_candidate(const std::array<float, 16>& matrix,
                       const std::array<float, 4>& normalizedViewport,
                       const inspection::RenderViewport& viewport,
                       std::uint16_t width,
                       std::uint16_t height,
                       std::uint32_t viewIndex,
                       std::uint32_t viewCount,
                       std::uint32_t passFlags,
                       std::uint64_t engineFrame) noexcept {
    const std::uint32_t threadId = GetCurrentThreadId();
    std::uint32_t firstThread = 0;
    (void)g_firstThreadId.compare_exchange_strong(
        firstThread, threadId, std::memory_order_acq_rel, std::memory_order_acquire);
    const std::uint32_t threadBit = (threadId ^ (threadId >> 6U)) & 63U;
    g_threadMask.fetch_or(1ULL << threadBit, std::memory_order_acq_rel);
    const std::uint64_t shape =
        shape_hash(normalizedViewport, width, height, passFlags, viewIndex, viewCount);
    const std::uint64_t matrixHash = matrix_hash(matrix);
    compare_primary_pair(viewIndex, viewCount, engineFrame, matrixHash);
    const std::uint64_t observedAtTick = GetTickCount64();
    if (viewIndex == 0 && (passFlags & 0x1U) != 0) {
        if (full_viewport(normalizedViewport) && !identity_matrix(matrix)) {
            inspection::RenderViewSnapshot exactView{};
            // The retail debug renderer broadcasts x/y/z/w and multiplies those
            // lanes by consecutive native rows (position * matrix). Normalize
            // once so every Sunrise consumer can keep using matrix * position.
            exactView.viewProjection = canonical_view_projection(matrix);
            exactView.viewport = viewport;
            exactView.engineFrame = engineFrame;
            exactView.publication =
                g_primaryPublications.fetch_add(1, std::memory_order_acq_rel) + 1;
            exactView.observedAtTick = observedAtTick;
            exactView.exactNative = true;
            exactView.valid = true;
            publish_view(exactView);
        } else {
            publish_view({});
        }
    }
    if (first_view_shape(shape)) {
        report_transition(threadId,
                          passFlags,
                          width,
                          height,
                          viewIndex,
                          viewCount,
                          engineFrame,
                          matrixHash,
                          normalizedViewport,
                          viewport);
    }
    g_acceptedSamples.fetch_add(1, std::memory_order_acq_rel);
    if (g_mailboxWriter.test_and_set(std::memory_order_acquire)) {
        g_concurrentSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    g_mailbox.lockSequence.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        g_mailbox.matrix[index].store(bits(matrix[index]), std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < normalizedViewport.size(); ++index) {
        g_mailbox.normalizedViewport[index].store(bits(normalizedViewport[index]),
                                                  std::memory_order_relaxed);
    }
    const std::array<float, 6> pixelViewport{viewport.x,
                                             viewport.y,
                                             viewport.width,
                                             viewport.height,
                                             viewport.minimumDepth,
                                             viewport.maximumDepth};
    for (std::size_t index = 0; index < pixelViewport.size(); ++index) {
        g_mailbox.viewport[index].store(bits(pixelViewport[index]), std::memory_order_relaxed);
    }
    const bool fullViewport = full_viewport(normalizedViewport);
    g_mailbox.lastSeenTick.store(observedAtTick, std::memory_order_relaxed);
    g_mailbox.engineFrame.store(engineFrame, std::memory_order_relaxed);
    g_mailbox.matrixHash.store(matrixHash, std::memory_order_relaxed);
    g_mailbox.threadId.store(threadId, std::memory_order_relaxed);
    g_mailbox.viewIndex.store(viewIndex, std::memory_order_relaxed);
    g_mailbox.viewCount.store(viewCount, std::memory_order_relaxed);
    g_mailbox.passFlags.store(passFlags, std::memory_order_relaxed);
    g_mailbox.framebufferWidth.store(width, std::memory_order_relaxed);
    g_mailbox.framebufferHeight.store(height, std::memory_order_relaxed);
    g_mailbox.fullViewport.store(fullViewport, std::memory_order_relaxed);
    g_mailbox.observed.store(true, std::memory_order_relaxed);
    g_mailbox.publication.fetch_add(1, std::memory_order_relaxed);
    g_mailbox.lockSequence.fetch_add(1, std::memory_order_release);
    g_mailboxWriter.clear(std::memory_order_release);
}

void capture(const std::byte* rendererFrame,
             const std::byte* view,
             const float* matrix,
             std::uint32_t passFlags) noexcept {
    g_calls.fetch_add(1, std::memory_order_acq_rel);
    if (rendererFrame == nullptr || view == nullptr || matrix == nullptr
        || reinterpret_cast<const std::byte*>(matrix) != view + kMatrixOffset) {
        g_rejectedSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    std::array<float, 16> copiedMatrix{};
    std::array<float, 4> normalizedViewport{};
    std::memcpy(copiedMatrix.data(), matrix, sizeof copiedMatrix);
    std::memcpy(normalizedViewport.data(), view + kViewportOffset, sizeof normalizedViewport);
    const std::uint16_t width = read<std::uint16_t>(view + kFramebufferWidthOffset);
    const std::uint16_t height = read<std::uint16_t>(view + kFramebufferHeightOffset);
    const std::byte* const viewArray = read<const std::byte*>(rendererFrame + kViewArrayOffset);
    const std::int32_t signedViewCount = read<std::int32_t>(rendererFrame + kViewCountOffset);
    if (viewArray == nullptr || signedViewCount <= 0
        || signedViewCount > static_cast<std::int32_t>(kMaximumViewCount)) {
        g_rejectedSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    const std::uintptr_t viewAddress = reinterpret_cast<std::uintptr_t>(view);
    const std::uintptr_t arrayAddress = reinterpret_cast<std::uintptr_t>(viewArray);
    if (viewAddress < arrayAddress) {
        g_rejectedSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    const std::uintptr_t viewOffset = viewAddress - arrayAddress;
    const std::uint32_t viewCount = static_cast<std::uint32_t>(signedViewCount);
    const std::uint32_t viewIndex = static_cast<std::uint32_t>(viewOffset / kViewStride);
    if (viewOffset % kViewStride != 0 || viewIndex >= viewCount) {
        g_rejectedSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    inspection::RenderViewport viewport{};
    if (!make_observed_view(copiedMatrix, normalizedViewport, width, height, viewport)) {
        g_rejectedSamples.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    publish_candidate(copiedMatrix,
                      normalizedViewport,
                      viewport,
                      width,
                      height,
                      viewIndex,
                      viewCount,
                      passFlags,
                      read<std::uint64_t>(reinterpret_cast<const std::byte*>(g_frameCounter)));
}

__declspec(noinline) void __fastcall observe(void* commands,
                                             void* rendererFrame,
                                             void* context,
                                             const std::byte* view,
                                             const void* origin,
                                             const void* negativeOrigin,
                                             const float* matrix,
                                             std::uint32_t passFlags) noexcept {
    ReplacementScope scope{};
    const Boundary next = original();
    if (next == nullptr) {
        return;
    }
    if (!g_stopping.load(std::memory_order_acquire)) {
        capture(static_cast<const std::byte*>(rendererFrame), view, matrix, passFlags);
    }
    next(commands, rendererFrame, context, view, origin, negativeOrigin, matrix, passFlags);
}

} // namespace

bool install_observer() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    clear_observations();
    g_stopping.store(false, std::memory_order_release);
    g_signaturesValid.store(false, std::memory_order_release);
    g_failure.store(ObserverFailure::none, std::memory_order_release);
    g_installPublishing.store(true, std::memory_order_release);
    std::byte* const snapshot =
        patterns::scan_main_image_unique(kSnapshot, "native_render_debug_frame_snapshot");
    if (snapshot == nullptr) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::snapshotSignature, "install");
        return false;
    }
    std::byte* const boundary =
        patterns::scan_main_image_unique(kBoundary, "native_render_per_view_boundary");
    if (boundary == nullptr) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::boundarySignature, "install");
        return false;
    }
    std::byte* const callSite =
        patterns::scan_main_image_unique(kCallSite, "native_render_per_view_call_site");
    if (callSite == nullptr) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::callSiteSignature, "install");
        return false;
    }
    std::byte* const frameCounterGetter =
        patterns::scan_main_image_unique(kFrameCounterGetter, "native_render_frame_counter_getter");
    if (frameCounterGetter == nullptr) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::frameCounterSignature, "install");
        return false;
    }
    if (callSite[kCallOpcodeOffset] != std::byte{0xE8}
        || patterns::resolve_relative(callSite + kCallOperandOffset, callSite + kCallEndOffset)
               != boundary) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::callRelationship, "install");
        return false;
    }
    const std::byte* const frameCounterAddress =
        patterns::resolve_relative(frameCounterGetter + 2, frameCounterGetter + 6);
    MEMORY_BASIC_INFORMATION frameCounterMemory{};
    if ((reinterpret_cast<std::uintptr_t>(frameCounterAddress) % alignof(std::uint64_t)) != 0
        || VirtualQuery(frameCounterAddress, &frameCounterMemory, sizeof frameCounterMemory)
               != sizeof frameCounterMemory
        || frameCounterMemory.State != MEM_COMMIT
        || (frameCounterMemory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::frameCounterSignature, "install");
        return false;
    }
    g_frameCounter = reinterpret_cast<const std::uint64_t*>(frameCounterAddress);
    g_signaturesValid.store(true, std::memory_order_release);
    const hooking::detour::Spec spec{boundary, reinterpret_cast<void*>(&observe)};
    if (!hooking::detour::install(spec, g_handle)) {
        g_frameCounter = nullptr;
        g_installPublishing.store(false, std::memory_order_release);
        set_failure(ObserverFailure::detourAttach, "install");
        return false;
    }
    g_original.store(reinterpret_cast<Boundary>(g_handle.original), std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=native_render_observer stage=install result=ok mode=passive");
    return true;
}

bool uninstall_observer() noexcept {
    g_stopping.store(true, std::memory_order_release);
    if (g_handle.attached) {
        const std::array protectedEntries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe)}};
        const hooking::detour::UninstallResult removal =
            hooking::detour::uninstall(g_handle, protectedEntries, &replacement_idle);
        if (removal != hooking::detour::UninstallResult::removed) {
            set_failure(ObserverFailure::detourDetach, "uninstall");
            return false;
        }
    }
    g_handle = {};
    g_original.store(nullptr, std::memory_order_release);
    g_frameCounter = nullptr;
    g_installed.store(false, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    g_signaturesValid.store(false, std::memory_order_release);
    clear_observations();
    return true;
}

ObserverStatus observer_status() noexcept {
    ObserverStatus result{};
    result.calls = g_calls.load(std::memory_order_acquire);
    result.acceptedSamples = g_acceptedSamples.load(std::memory_order_acquire);
    result.rejectedSamples = g_rejectedSamples.load(std::memory_order_acquire);
    result.concurrentSamples = g_concurrentSamples.load(std::memory_order_acquire);
    result.observedThreadCount =
        static_cast<std::uint32_t>(std::popcount(g_threadMask.load(std::memory_order_acquire)));
    result.viewShapeCount = g_viewShapeCount.load(std::memory_order_acquire);
    result.pairedFrames = g_pairedFrames.load(std::memory_order_acquire);
    result.equalMatrixFrames = g_equalMatrixFrames.load(std::memory_order_acquire);
    result.differentMatrixFrames = g_differentMatrixFrames.load(std::memory_order_acquire);
    result.failure = g_failure.load(std::memory_order_acquire);
    result.signaturesValid = g_signaturesValid.load(std::memory_order_acquire);
    result.installed = g_installed.load(std::memory_order_acquire);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = g_mailbox.lockSequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        for (std::size_t index = 0; index < result.matrix.size(); ++index) {
            result.matrix[index] = lane(g_mailbox.matrix[index].load(std::memory_order_relaxed));
        }
        for (std::size_t index = 0; index < result.normalizedViewport.size(); ++index) {
            result.normalizedViewport[index] =
                lane(g_mailbox.normalizedViewport[index].load(std::memory_order_relaxed));
        }
        std::array<float, 6> viewport{};
        for (std::size_t index = 0; index < viewport.size(); ++index) {
            viewport[index] = lane(g_mailbox.viewport[index].load(std::memory_order_relaxed));
        }
        result.viewport = {
            viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]};
        result.sequence = g_mailbox.publication.load(std::memory_order_relaxed);
        result.lastSeenTick = g_mailbox.lastSeenTick.load(std::memory_order_relaxed);
        result.engineFrame = g_mailbox.engineFrame.load(std::memory_order_relaxed);
        result.matrixHash = g_mailbox.matrixHash.load(std::memory_order_relaxed);
        result.threadId = g_mailbox.threadId.load(std::memory_order_relaxed);
        result.viewIndex = g_mailbox.viewIndex.load(std::memory_order_relaxed);
        result.viewCount = g_mailbox.viewCount.load(std::memory_order_relaxed);
        result.passFlags = g_mailbox.passFlags.load(std::memory_order_relaxed);
        result.framebufferWidth = g_mailbox.framebufferWidth.load(std::memory_order_relaxed);
        result.framebufferHeight = g_mailbox.framebufferHeight.load(std::memory_order_relaxed);
        result.fullViewport = g_mailbox.fullViewport.load(std::memory_order_relaxed);
        result.observed = g_mailbox.observed.load(std::memory_order_relaxed);
        const std::uint64_t after = g_mailbox.lockSequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            return result;
        }
    }
    result.observed = false;
    return result;
}

const char* observer_failure_name(ObserverFailure failure) noexcept {
    switch (failure) {
    case ObserverFailure::none:
        return "none";
    case ObserverFailure::snapshotSignature:
        return "snapshot_signature";
    case ObserverFailure::boundarySignature:
        return "boundary_signature";
    case ObserverFailure::callSiteSignature:
        return "call_site_signature";
    case ObserverFailure::frameCounterSignature:
        return "frame_counter_signature";
    case ObserverFailure::callRelationship:
        return "call_relationship";
    case ObserverFailure::detourAttach:
        return "detour_attach";
    case ObserverFailure::detourDetach:
        return "detour_detach";
    }
    return "unknown";
}

} // namespace sunrise::client::hooks::graphics::renderer::native_debug
