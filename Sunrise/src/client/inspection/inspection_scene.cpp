#include "inspection_scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

#include "inspection_descriptors.h"

namespace sunrise::client::inspection {
namespace {

struct PickRequestMailbox final {
    std::atomic<std::uint64_t> publication{};
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint64_t> capturedFrame{};
    std::atomic<std::uint64_t> engineFrame{};
    std::atomic<std::uint64_t> viewPublication{};
    std::atomic<std::uint64_t> depthSequence{};
    std::atomic<std::uint32_t> graphGeneration{};
    std::atomic<std::uint32_t> x{};
    std::atomic<std::uint32_t> y{};
};

struct PickResultMailbox final {
    std::atomic<std::uint64_t> publication{};
    std::atomic<std::uint64_t> requestSequence{};
    std::atomic<std::uint64_t> capturedFrame{};
    std::atomic<std::uint64_t> engineFrame{};
    std::atomic<std::uint64_t> viewPublication{};
    std::atomic<std::uint64_t> depthSequence{};
    std::atomic<std::uint32_t> graphGeneration{};
    std::atomic<std::uint64_t> node{};
    std::atomic<bool> ready{};
};

PickRequestMailbox g_pickRequest{};
PickResultMailbox g_pickResult{};

} // namespace

void publish_pick_request(const PickRequest& request) noexcept {
    g_pickRequest.publication.fetch_add(1, std::memory_order_acq_rel);
    g_pickRequest.sequence.store(request.sequence, std::memory_order_relaxed);
    g_pickRequest.capturedFrame.store(request.capturedFrame, std::memory_order_relaxed);
    g_pickRequest.engineFrame.store(request.engineFrame, std::memory_order_relaxed);
    g_pickRequest.viewPublication.store(request.viewPublication, std::memory_order_relaxed);
    g_pickRequest.depthSequence.store(request.depthSequence, std::memory_order_relaxed);
    g_pickRequest.graphGeneration.store(request.graphGeneration, std::memory_order_relaxed);
    g_pickRequest.x.store(request.x, std::memory_order_relaxed);
    g_pickRequest.y.store(request.y, std::memory_order_relaxed);
    g_pickRequest.publication.fetch_add(1, std::memory_order_release);
}

PickRequest pick_request() noexcept {
    PickRequest request{};
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = g_pickRequest.publication.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        request.sequence = g_pickRequest.sequence.load(std::memory_order_relaxed);
        request.capturedFrame = g_pickRequest.capturedFrame.load(std::memory_order_relaxed);
        request.engineFrame = g_pickRequest.engineFrame.load(std::memory_order_relaxed);
        request.viewPublication = g_pickRequest.viewPublication.load(std::memory_order_relaxed);
        request.depthSequence = g_pickRequest.depthSequence.load(std::memory_order_relaxed);
        request.graphGeneration = g_pickRequest.graphGeneration.load(std::memory_order_relaxed);
        request.x = g_pickRequest.x.load(std::memory_order_relaxed);
        request.y = g_pickRequest.y.load(std::memory_order_relaxed);
        const std::uint64_t after = g_pickRequest.publication.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            return request;
        }
    }
    return {};
}

void publish_pick_result(const PickResult& result) noexcept {
    g_pickResult.publication.fetch_add(1, std::memory_order_acq_rel);
    g_pickResult.requestSequence.store(result.requestSequence, std::memory_order_relaxed);
    g_pickResult.capturedFrame.store(result.capturedFrame, std::memory_order_relaxed);
    g_pickResult.engineFrame.store(result.engineFrame, std::memory_order_relaxed);
    g_pickResult.viewPublication.store(result.viewPublication, std::memory_order_relaxed);
    g_pickResult.depthSequence.store(result.depthSequence, std::memory_order_relaxed);
    g_pickResult.graphGeneration.store(result.graphGeneration, std::memory_order_relaxed);
    g_pickResult.node.store(result.node.value, std::memory_order_relaxed);
    g_pickResult.ready.store(result.ready, std::memory_order_relaxed);
    g_pickResult.publication.fetch_add(1, std::memory_order_release);
}

PickResult pick_result() noexcept {
    PickResult result{};
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = g_pickResult.publication.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        result.requestSequence = g_pickResult.requestSequence.load(std::memory_order_relaxed);
        result.capturedFrame = g_pickResult.capturedFrame.load(std::memory_order_relaxed);
        result.engineFrame = g_pickResult.engineFrame.load(std::memory_order_relaxed);
        result.viewPublication = g_pickResult.viewPublication.load(std::memory_order_relaxed);
        result.depthSequence = g_pickResult.depthSequence.load(std::memory_order_relaxed);
        result.graphGeneration = g_pickResult.graphGeneration.load(std::memory_order_relaxed);
        result.node.value = g_pickResult.node.load(std::memory_order_relaxed);
        result.ready = g_pickResult.ready.load(std::memory_order_relaxed);
        const std::uint64_t after = g_pickResult.publication.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            return result;
        }
    }
    return {};
}

void clear_pick_state() noexcept {
    publish_pick_request({});
    publish_pick_result({});
}

const char* depth_capability_name(DepthCapability capability) noexcept {
    switch (capability) {
    case DepthCapability::supported:
        return "supported";
    case DepthCapability::invalidDimensions:
        return "invalid dimensions";
    case DepthCapability::singleSample:
        return "single-sample depth unsupported";
    case DepthCapability::unsupportedSamples:
        return "sample count unsupported";
    case DepthCapability::unsupportedFormat:
        return "format unsupported";
    case DepthCapability::captureFailed:
        return "capture failed";
    case DepthCapability::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

bool overlay_category_enabled(NodeKind kind, const OverlayPolicy& policy) noexcept {
    switch (descriptor(kind).category) {
    case NodeCategory::geometry:
        return policy.showGeometry;
    case NodeCategory::entity:
    case NodeCategory::physics:
        return policy.showEntities;
    case NodeCategory::spawn:
        return policy.showSpawns;
    case NodeCategory::logic:
        return policy.showLogic;
    case NodeCategory::trigger:
        return policy.showTriggers;
    case NodeCategory::audio:
        return policy.showAudio;
    default:
        return false;
    }
}

const char* helper_backend_name(HelperBackend backend) noexcept {
    switch (backend) {
    case HelperBackend::sunriseDepth:
        return "Sunrise D3D depth";
    case HelperBackend::none:
        return "None";
    }
    return "None";
}

const char* helper_failure_name(HelperFailureReason reason) noexcept {
    switch (reason) {
    case HelperFailureReason::none:
        return "none";
    case HelperFailureReason::depthUnavailable:
        return "depth unavailable";
    case HelperFailureReason::exactViewUnavailable:
        return "exact native view unavailable";
    case HelperFailureReason::drawFailed:
        return "draw failed";
    }
    return "unknown";
}

const char* picking_readiness_name(PickingReadiness readiness) noexcept {
    switch (readiness) {
    case PickingReadiness::unavailable:
        return "unavailable";
    case PickingReadiness::waitingForView:
        return "waiting for exact view";
    case PickingReadiness::waitingForReadback:
        return "waiting for readback";
    case PickingReadiness::ready:
        return "ready";
    }
    return "unavailable";
}

const char* overlay_detail_name(OverlayDetail detail) noexcept {
    switch (detail) {
    case OverlayDetail::selectedOnly:
        return "Selected only";
    case OverlayDetail::selectedNearby:
        return "Selected and nearby";
    case OverlayDetail::all:
        return "All admitted";
    case OverlayDetail::adaptive:
        return "Adaptive";
    case OverlayDetail::cameraNearby:
        return "Camera nearby";
    }
    return "Adaptive";
}

const char* helper_glyph_name(HelperGlyph glyph) noexcept {
    switch (glyph) {
    case HelperGlyph::spawn:
        return "Spawn";
    case HelperGlyph::runtimeEntity:
        return "Runtime entity";
    case HelperGlyph::spawnRule:
        return "Spawn rule";
    case HelperGlyph::squad:
        return "Squad";
    case HelperGlyph::trigger:
        return "Trigger";
    case HelperGlyph::spatial:
        return "Spatial";
    case HelperGlyph::objective:
        return "Objective";
    case HelperGlyph::device:
        return "Device";
    case HelperGlyph::interactable:
        return "Interactable";
    case HelperGlyph::action:
        return "Action";
    case HelperGlyph::target:
        return "Target";
    case HelperGlyph::condition:
        return "Condition";
    case HelperGlyph::competitive:
        return "Competitive";
    case HelperGlyph::audio:
        return "Audio";
    case HelperGlyph::physics:
        return "Physics";
    case HelperGlyph::geometry:
        return "Geometry";
    case HelperGlyph::unknown:
        return "Unknown";
    }
    return "Unknown";
}

HelperMarkerShape helper_marker_shape(HelperGlyph glyph) noexcept {
    switch (glyph) {
    case HelperGlyph::spawn:
    case HelperGlyph::runtimeEntity:
    case HelperGlyph::squad:
    case HelperGlyph::audio:
    case HelperGlyph::physics:
        return HelperMarkerShape::circle;
    default:
        return HelperMarkerShape::square;
    }
}

std::array<float, 4> helper_role_color(HelperGlyph glyph) noexcept {
    constexpr float scale = 1.0F / 255.0F;
    // The helper palette is built from Indigo, Watermelon, Golden Pollen,
    // Honeydew, and Charcoal. Related shades keep all eighteen roles distinct;
    // marker shapes and contrast-aware outlines remain the secondary cue.
    switch (glyph) {
    case HelperGlyph::spawn:
        return {84.0F * scale, 13.0F * scale, 110.0F * scale, 1.0F};
    case HelperGlyph::runtimeEntity:
        return {238.0F * scale, 66.0F * scale, 102.0F * scale, 1.0F};
    case HelperGlyph::spawnRule:
        return {255.0F * scale, 210.0F * scale, 63.0F * scale, 1.0F};
    case HelperGlyph::squad:
        return {243.0F * scale, 252.0F * scale, 240.0F * scale, 1.0F};
    case HelperGlyph::trigger:
        return {207.0F * scale, 62.0F * scale, 91.0F * scale, 1.0F};
    case HelperGlyph::spatial:
        return {113.0F * scale, 56.0F * scale, 133.0F * scale, 1.0F};
    case HelperGlyph::objective:
        return {253.0F * scale, 218.0F * scale, 98.0F * scale, 1.0F};
    case HelperGlyph::device:
        return {141.0F * scale, 102.0F * scale, 156.0F * scale, 1.0F};
    case HelperGlyph::interactable:
        return {211.0F * scale, 220.0F * scale, 208.0F * scale, 1.0F};
    case HelperGlyph::action:
        return {239.0F * scale, 99.0F * scale, 127.0F * scale, 1.0F};
    case HelperGlyph::target:
        return {240.0F * scale, 133.0F * scale, 152.0F * scale, 1.0F};
    case HelperGlyph::condition:
        return {251.0F * scale, 229.0F * scale, 138.0F * scale, 1.0F};
    case HelperGlyph::competitive:
        return {170.0F * scale, 148.0F * scale, 176.0F * scale, 1.0F};
    case HelperGlyph::audio:
        return {102.0F * scale, 36.0F * scale, 124.0F * scale, 1.0F};
    case HelperGlyph::physics:
        return {128.0F * scale, 79.0F * scale, 146.0F * scale, 1.0F};
    case HelperGlyph::geometry:
        return {221.0F * scale, 184.0F * scale, 58.0F * scale, 1.0F};
    case HelperGlyph::unknown:
        return {31.0F * scale, 39.0F * scale, 27.0F * scale, 1.0F};
    }
    return {31.0F * scale, 39.0F * scale, 27.0F * scale, 1.0F};
}

std::array<float, 4> helper_volume_color() noexcept {
    constexpr float scale = 1.0F / 255.0F;
    return {18.0F * scale, 218.0F * scale, 235.0F * scale, 1.0F};
}

bool helper_geometry_current(const HelperRenderStatus& status,
                             std::uint64_t capturedFrame) noexcept {
    return status.drawn && status.capturedFrame != 0 && status.capturedFrame == capturedFrame
           && status.backend == HelperBackend::sunriseDepth;
}

bool pick_result_current(const PickResult& result,
                         std::uint64_t requestSequence,
                         std::uint64_t capturedFrame,
                         std::uint64_t engineFrame,
                         std::uint64_t viewPublication,
                         std::uint64_t depthSequence,
                         std::uint32_t graphGeneration) noexcept {
    return result.ready && result.requestSequence == requestSequence
           && result.capturedFrame == capturedFrame && result.engineFrame == engineFrame
           && result.viewPublication == viewPublication && result.depthSequence == depthSequence
           && result.graphGeneration == graphGeneration;
}

bool pick_result_fresh(const PickResult& result,
                       const PickRequest& request,
                       std::uint64_t currentCapturedFrame,
                       std::uint64_t currentEngineFrame,
                       std::uint64_t currentViewPublication,
                       std::uint64_t currentDepthSequence,
                       std::uint32_t currentGraphGeneration) noexcept {
    constexpr std::uint64_t kMaximumFrameLag = 2;
    if (!result.ready || request.sequence == 0 || result.requestSequence != request.sequence
        || request.graphGeneration != currentGraphGeneration
        || result.graphGeneration != currentGraphGeneration || request.capturedFrame == 0
        || result.capturedFrame < request.capturedFrame
        || result.capturedFrame - request.capturedFrame > kMaximumFrameLag
        || currentCapturedFrame < result.capturedFrame
        || currentCapturedFrame - result.capturedFrame > kMaximumFrameLag) {
        return false;
    }
    // The ID pass normally executes on the frame after publication. Its exact
    // render metadata must therefore be bracketed by the request and current view,
    // rather than equal to either endpoint.
    return request.engineFrame != 0 && request.viewPublication != 0 && request.depthSequence != 0
           && result.engineFrame >= request.engineFrame && result.engineFrame <= currentEngineFrame
           && result.viewPublication >= request.viewPublication
           && result.viewPublication <= currentViewPublication
           && result.depthSequence >= request.depthSequence
           && result.depthSequence <= currentDepthSequence;
}

void assign_pick_tokens(SceneFrame& frame) {
    frame.picks.clear();
    std::unordered_map<std::uint64_t, std::uint32_t> tokens;
    tokens.reserve(frame.stats.nodesWithLines);
    for (SceneLine& line : frame.lines) {
        if (!line.owner) {
            line.pickToken = 0;
            continue;
        }
        const auto [entry, inserted] =
            tokens.try_emplace(line.owner.value, static_cast<std::uint32_t>(tokens.size() + 1U));
        line.pickToken = entry->second;
        if (inserted) {
            frame.picks.push_back({entry->second, line.owner});
        }
    }
    for (SceneGlyph& glyph : frame.glyphs) {
        if (!glyph.owner) {
            glyph.pickToken = 0;
            continue;
        }
        const auto [entry, inserted] =
            tokens.try_emplace(glyph.owner.value, static_cast<std::uint32_t>(tokens.size() + 1U));
        glyph.pickToken = entry->second;
        if (inserted) {
            frame.picks.push_back({entry->second, glyph.owner});
        }
    }
}

NodeId resolve_pick_token(const SceneFrame& frame, std::uint32_t token) noexcept {
    if (token == 0) {
        return {};
    }
    const auto entry = std::ranges::find(frame.picks, token, &SceneFrame::PickEntry::token);
    return entry == frame.picks.end() ? NodeId{} : entry->node;
}

PickCoordinate map_pick_coordinate(float cursorX,
                                   float cursorY,
                                   float imageMinimumX,
                                   float imageMinimumY,
                                   float imageMaximumX,
                                   float imageMaximumY,
                                   std::uint32_t sourceWidth,
                                   std::uint32_t sourceHeight) noexcept {
    PickCoordinate result{};
    const float width = imageMaximumX - imageMinimumX;
    const float height = imageMaximumY - imageMinimumY;
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY) || width <= 0.0F || height <= 0.0F
        || sourceWidth == 0 || sourceHeight == 0 || cursorX < imageMinimumX
        || cursorY < imageMinimumY || cursorX >= imageMaximumX || cursorY >= imageMaximumY) {
        return result;
    }
    const float normalizedX = (cursorX - imageMinimumX) / width;
    const float normalizedY = (cursorY - imageMinimumY) / height;
    result.x =
        (std::min)(sourceWidth - 1U,
                   static_cast<std::uint32_t>(normalizedX * static_cast<float>(sourceWidth)));
    result.y =
        (std::min)(sourceHeight - 1U,
                   static_cast<std::uint32_t>(normalizedY * static_cast<float>(sourceHeight)));
    result.valid = true;
    return result;
}

PickCoordinate map_pick_coordinate_to_viewport(float cursorX,
                                               float cursorY,
                                               float imageMinimumX,
                                               float imageMinimumY,
                                               float imageMaximumX,
                                               float imageMaximumY,
                                               const RenderViewport& viewport,
                                               std::uint32_t sourceWidth,
                                               std::uint32_t sourceHeight) noexcept {
    PickCoordinate result{};
    const float imageWidth = imageMaximumX - imageMinimumX;
    const float imageHeight = imageMaximumY - imageMinimumY;
    const float viewportMaximumX = viewport.x + viewport.width;
    const float viewportMaximumY = viewport.y + viewport.height;
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY) || !std::isfinite(viewport.x)
        || !std::isfinite(viewport.y) || !std::isfinite(viewport.width)
        || !std::isfinite(viewport.height) || imageWidth <= 0.0F || imageHeight <= 0.0F
        || viewport.x < 0.0F || viewport.y < 0.0F || viewport.width <= 0.0F
        || viewport.height <= 0.0F || sourceWidth == 0 || sourceHeight == 0
        || viewportMaximumX > static_cast<float>(sourceWidth) + 1.0F
        || viewportMaximumY > static_cast<float>(sourceHeight) + 1.0F || cursorX < imageMinimumX
        || cursorY < imageMinimumY || cursorX >= imageMaximumX || cursorY >= imageMaximumY) {
        return result;
    }
    const float normalizedX = (cursorX - imageMinimumX) / imageWidth;
    const float normalizedY = (cursorY - imageMinimumY) / imageHeight;
    result.x = (std::min)(sourceWidth - 1U,
                          static_cast<std::uint32_t>(viewport.x + normalizedX * viewport.width));
    result.y = (std::min)(sourceHeight - 1U,
                          static_cast<std::uint32_t>(viewport.y + normalizedY * viewport.height));
    result.valid = true;
    return result;
}

} // namespace sunrise::client::inspection
