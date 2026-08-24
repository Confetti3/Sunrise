#include "native_debug_renderer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>

namespace sunrise::client::hooks::graphics::renderer::native_debug {
namespace {

struct ViewMailbox final {
    std::atomic_uint64_t sequence{};
    std::array<std::atomic_uint32_t, 16> matrix{};
    std::array<std::atomic_uint32_t, 6> viewport{};
    std::atomic_uint64_t engineFrame{};
    std::atomic_uint64_t publication{};
    std::atomic_uint64_t observedAtTick{};
    std::atomic_bool exactNative{};
    std::atomic_bool valid{};
};

ViewMailbox g_view{};
std::atomic_flag g_viewWriter = ATOMIC_FLAG_INIT;
constexpr std::uint64_t kExactViewLifetimeMilliseconds = 100;

[[nodiscard]] std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float lane(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}

} // namespace

bool make_observed_view(const std::array<float, 16>& matrix,
                        const std::array<float, 4>& normalizedViewport,
                        std::uint16_t framebufferWidth,
                        std::uint16_t framebufferHeight,
                        inspection::RenderViewport& viewport) noexcept {
    viewport = {};
    if (framebufferWidth == 0 || framebufferHeight == 0
        || !std::ranges::all_of(matrix, [](float value) { return std::isfinite(value); })
        || !std::ranges::all_of(normalizedViewport,
                                [](float value) { return std::isfinite(value); })) {
        return false;
    }
    const float left = normalizedViewport[0];
    const float top = normalizedViewport[1];
    const float right = normalizedViewport[2];
    const float bottom = normalizedViewport[3];
    if (left < 0.0F || top < 0.0F || right > 1.0F || bottom > 1.0F || right <= left
        || bottom <= top) {
        return false;
    }
    const float width = static_cast<float>(framebufferWidth);
    const float height = static_cast<float>(framebufferHeight);
    viewport.x = width * left;
    viewport.y = height * top;
    viewport.width = width * right - viewport.x;
    viewport.height = height * bottom - viewport.y;
    viewport.minimumDepth = 0.0F;
    viewport.maximumDepth = 1.0F;
    return std::isfinite(viewport.width) && std::isfinite(viewport.height) && viewport.width > 0.0F
           && viewport.height > 0.0F;
}

std::array<float, 16>
canonical_view_projection(const std::array<float, 16>& nativeMatrix) noexcept {
    std::array<float, 16> canonical{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            canonical[row * 4U + column] = nativeMatrix[column * 4U + row];
        }
    }
    return canonical;
}

void publish_view(const inspection::RenderViewSnapshot& view) noexcept {
    if (g_viewWriter.test_and_set(std::memory_order_acquire)) {
        return;
    }
    g_view.sequence.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < view.viewProjection.size(); ++index) {
        g_view.matrix[index].store(bits(view.viewProjection[index]), std::memory_order_relaxed);
    }
    const std::array<float, 6> viewport{view.viewport.x,
                                        view.viewport.y,
                                        view.viewport.width,
                                        view.viewport.height,
                                        view.viewport.minimumDepth,
                                        view.viewport.maximumDepth};
    for (std::size_t index = 0; index < viewport.size(); ++index) {
        g_view.viewport[index].store(bits(viewport[index]), std::memory_order_relaxed);
    }
    g_view.engineFrame.store(view.engineFrame, std::memory_order_relaxed);
    g_view.publication.store(view.publication, std::memory_order_relaxed);
    g_view.observedAtTick.store(view.observedAtTick, std::memory_order_relaxed);
    g_view.exactNative.store(view.exactNative, std::memory_order_relaxed);
    g_view.valid.store(view.valid, std::memory_order_relaxed);
    g_view.sequence.fetch_add(1, std::memory_order_release);
    g_viewWriter.clear(std::memory_order_release);
}

inspection::RenderViewSnapshot view() noexcept {
    inspection::RenderViewSnapshot result{};
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = g_view.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        for (std::size_t index = 0; index < result.viewProjection.size(); ++index) {
            result.viewProjection[index] =
                lane(g_view.matrix[index].load(std::memory_order_relaxed));
        }
        std::array<float, 6> viewport{};
        for (std::size_t index = 0; index < viewport.size(); ++index) {
            viewport[index] = lane(g_view.viewport[index].load(std::memory_order_relaxed));
        }
        result.viewport = {
            viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]};
        result.engineFrame = g_view.engineFrame.load(std::memory_order_relaxed);
        result.publication = g_view.publication.load(std::memory_order_relaxed);
        result.observedAtTick = g_view.observedAtTick.load(std::memory_order_relaxed);
        result.exactNative = g_view.exactNative.load(std::memory_order_relaxed);
        result.valid = g_view.valid.load(std::memory_order_relaxed);
        const std::uint64_t after = g_view.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            if (result.valid
                && !view_current(result, GetTickCount64(), kExactViewLifetimeMilliseconds)) {
                result.valid = false;
            }
            return result;
        }
    }
    return {};
}

bool view_current(const inspection::RenderViewSnapshot& candidate,
                  std::uint64_t currentTick,
                  std::uint64_t maximumAge) noexcept {
    return candidate.valid && candidate.exactNative && candidate.engineFrame != 0
           && candidate.publication != 0 && candidate.observedAtTick != 0
           && candidate.viewport.width > 0.0F && candidate.viewport.height > 0.0F
           && currentTick >= candidate.observedAtTick
           && currentTick - candidate.observedAtTick <= maximumAge;
}

void reset() noexcept {
    publish_view({});
}

} // namespace sunrise::client::hooks::graphics::renderer::native_debug
