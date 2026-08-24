#include "graphics_depth_observer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace sunrise::client::hooks::graphics::renderer::depth_observer {
namespace {

constexpr std::size_t kClassCapacity = 16;
constexpr std::size_t kContextCapacity = 32;
constexpr float kClearTolerance = 0.001F;

struct FrameSample final {
    std::uint64_t matrixHash{};
    float clearDepth{std::numeric_limits<float>::quiet_NaN()};
    D3D11_COMPARISON_FUNC comparison{D3D11_COMPARISON_NEVER};
    bool qualifying{};
};

struct IdentityEntry final {
    std::uintptr_t identity{};
    float lastClear{std::numeric_limits<float>::quiet_NaN()};
    bool clearValid{};
};

struct ClassEntry final {
    Descriptor descriptor{};
    std::array<IdentityEntry, kIdentityCapacity> identities{};
    std::array<FrameSample, kProofFrameCount> frames{};
    std::uint64_t bindings{};
    std::uint64_t clears{};
    std::uint64_t sequence{};
    std::uint32_t frameCursor{};
    std::uint32_t frameCount{};
    std::uint32_t qualifyingCount{};
    std::uint32_t maximumDistinctMatrixHashes{};
    std::uint32_t identityCursor{};
    std::uintptr_t lastBoundIdentity{};
    float lastClear{std::numeric_limits<float>::quiet_NaN()};
    D3D11_COMPARISON_FUNC lastComparison{D3D11_COMPARISON_NEVER};
    bool occupied{};
    bool identityOverflow{};
};

struct ContextEntry final {
    ID3D11DeviceContext* identity{};
    D3D11_COMPARISON_FUNC comparison{D3D11_COMPARISON_NEVER};
    std::size_t boundClass{kClassCapacity};
    std::uintptr_t boundIdentity{};
    std::uint64_t boundSequence{};
    bool occupied{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::atomic<ID3D11Device*> g_selectedDevice{};
std::atomic<ID3D11DeviceContext*> g_selectedContext{};
std::atomic_bool g_hooksResolved{};
std::atomic_bool g_vtableMatches{};
std::atomic_uint64_t g_dropped{};
std::atomic_bool g_captureRequested{};
std::atomic_bool g_captureArmed{};
std::atomic_size_t g_provenClass{kClassCapacity};
std::atomic<Convention> g_provenConvention{Convention::unknown};
SRWLOCK g_mailboxLock{SRWLOCK_INIT};
CapturedDepth g_mailbox{};
std::atomic_uint64_t g_captureSequence{};
std::array<ClassEntry, kClassCapacity> g_classes{};
std::array<ContextEntry, kContextCapacity> g_contexts{};
std::size_t g_classCount{};
std::size_t g_contextCursor{};
std::size_t g_lastBoundClass{kClassCapacity};
std::uint64_t g_lastBindingSequence{};
std::uint64_t g_bindings{};
std::uint64_t g_stateChanges{};
std::uint64_t g_clears{};
std::uint64_t g_presentFrames{};
std::uint64_t g_lastObservationTick{};
std::uint64_t g_lastEngineFrame{};
std::uint64_t g_lastViewPublication{};
std::uint32_t g_selectedThreadId{};
thread_local std::uint32_t g_internalCallDepth{};

[[nodiscard]] bool same_descriptor(const Descriptor& left, const Descriptor& right) noexcept {
    return left.format == right.format && left.width == right.width && left.height == right.height
           && left.sampleCount == right.sampleCount && left.sampleQuality == right.sampleQuality
           && left.singleSliceMipZero == right.singleSliceMipZero;
}

[[nodiscard]] bool selected_context_device(ID3D11DeviceContext* context) noexcept {
    // The proof is defined at the selected swap-chain's Present boundary. A deferred
    // context can belong to the same device and bind an identically described depth
    // resource without that resource being the final main-view surface. Mixing those
    // calls made the descriptor proof look sound while the ownership mailbox could
    // retain an auxiliary worker surface. Only the exact immediate context recovered
    // from the selected device participates in proof or capture.
    return context != nullptr && g_internalCallDepth == 0
           && context == g_selectedContext.load(std::memory_order_acquire)
           && g_selectedDevice.load(std::memory_order_acquire) != nullptr
           && g_vtableMatches.load(std::memory_order_acquire);
}

[[nodiscard]] bool describe_depth_view(ID3D11DepthStencilView* view,
                                       Descriptor& descriptor,
                                       std::uintptr_t& identity) noexcept {
    if (view == nullptr) {
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    const bool mipZero = viewDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D
                             ? viewDesc.Texture2D.MipSlice == 0
                             : viewDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMS;
    if (!mipZero) {
        return false;
    }

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr) {
        return false;
    }
    ID3D11Texture2D* texture = nullptr;
    const HRESULT query =
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    identity = reinterpret_cast<std::uintptr_t>(resource);
    resource->Release();
    if (FAILED(query) || texture == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    texture->Release();
    descriptor.format = viewDesc.Format;
    descriptor.width = textureDesc.Width;
    descriptor.height = textureDesc.Height;
    descriptor.sampleCount = textureDesc.SampleDesc.Count;
    descriptor.sampleQuality = textureDesc.SampleDesc.Quality;
    descriptor.singleSliceMipZero = textureDesc.ArraySize == 1 && mipZero;
    return true;
}

[[nodiscard]] std::size_t find_or_add_class(const Descriptor& descriptor) noexcept {
    for (std::size_t index = 0; index < g_classCount; ++index) {
        if (same_descriptor(g_classes[index].descriptor, descriptor)) {
            return index;
        }
    }
    if (g_classCount >= g_classes.size()) {
        return g_classes.size();
    }
    const std::size_t index = g_classCount++;
    g_classes[index] = {};
    g_classes[index].descriptor = descriptor;
    g_classes[index].occupied = true;
    return index;
}

[[nodiscard]] IdentityEntry& note_identity(ClassEntry& entry, std::uintptr_t identity) noexcept {
    for (IdentityEntry& existing : entry.identities) {
        if (existing.identity == identity) {
            return existing;
        }
    }
    for (IdentityEntry& existing : entry.identities) {
        if (existing.identity == 0) {
            existing = {};
            existing.identity = identity;
            return existing;
        }
    }
    // Keep the current rotating set bounded. Overflow remains diagnostic evidence.
    IdentityEntry& replacement = entry.identities[entry.identityCursor++ % entry.identities.size()];
    replacement = {};
    replacement.identity = identity;
    entry.identityOverflow = true;
    return replacement;
}

[[nodiscard]] ContextEntry& context_entry(ID3D11DeviceContext* identity) noexcept {
    for (ContextEntry& entry : g_contexts) {
        if (entry.occupied && entry.identity == identity) {
            return entry;
        }
    }
    for (ContextEntry& entry : g_contexts) {
        if (!entry.occupied) {
            entry = {};
            entry.identity = identity;
            entry.occupied = true;
            return entry;
        }
    }
    ContextEntry& entry = g_contexts[g_contextCursor++ % g_contexts.size()];
    entry = {};
    entry.identity = identity;
    entry.occupied = true;
    return entry;
}

[[nodiscard]] ContextEntry* find_context(ID3D11DeviceContext* identity) noexcept {
    for (ContextEntry& entry : g_contexts) {
        if (entry.occupied && entry.identity == identity) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] std::uint64_t hash_matrix(const std::array<float, 16>& matrix) noexcept {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto* bytes = reinterpret_cast<const unsigned char*>(matrix.data());
    for (std::size_t index = 0; index < sizeof(float) * matrix.size(); ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::uint32_t distinct_hashes(const ClassEntry& entry) noexcept {
    std::array<std::uint64_t, kRequiredMatrixHashes> hashes{};
    std::uint32_t count{};
    for (std::uint32_t index = 0; index < entry.frameCount; ++index) {
        const FrameSample& sample = entry.frames[index];
        if (!sample.qualifying || sample.matrixHash == 0) {
            continue;
        }
        bool found = false;
        for (std::uint32_t existing = 0; existing < count; ++existing) {
            found = found || hashes[existing] == sample.matrixHash;
        }
        if (!found) {
            hashes[count++] = sample.matrixHash;
            if (count == hashes.size()) {
                break;
            }
        }
    }
    return count;
}

[[nodiscard]] Convention entry_convention(const ClassEntry& entry) noexcept {
    Convention result = Convention::unknown;
    bool observed = false;
    for (std::uint32_t index = 0; index < entry.frameCount; ++index) {
        const FrameSample& sample = entry.frames[index];
        if (!sample.qualifying) {
            continue;
        }
        const Convention current = classify_convention(sample.clearDepth, sample.comparison);
        if (current == Convention::unknown) {
            return Convention::unknown;
        }
        if (!observed) {
            result = current;
            observed = true;
        } else if (current != result) {
            return Convention::unknown;
        }
    }
    return observed ? result : Convention::unknown;
}

[[nodiscard]] bool clear_consistent(const ClassEntry& entry, Convention convention) noexcept {
    if (convention == Convention::unknown) {
        return false;
    }
    for (std::uint32_t index = 0; index < entry.frameCount; ++index) {
        const FrameSample& sample = entry.frames[index];
        if (!sample.qualifying) {
            continue;
        }
        const bool standard = std::fabs(sample.clearDepth - 1.0F) <= kClearTolerance;
        const bool reversed = std::fabs(sample.clearDepth) <= kClearTolerance;
        if ((convention == Convention::standard && !standard)
            || (convention == Convention::reversed && !reversed)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool comparison_consistent(const ClassEntry& entry, Convention convention) noexcept {
    if (convention == Convention::unknown) {
        return false;
    }
    for (std::uint32_t index = 0; index < entry.frameCount; ++index) {
        const FrameSample& sample = entry.frames[index];
        if (!sample.qualifying) {
            continue;
        }
        const bool standard = sample.comparison == D3D11_COMPARISON_LESS
                              || sample.comparison == D3D11_COMPARISON_LESS_EQUAL
                              || sample.comparison == D3D11_COMPARISON_EQUAL;
        const bool reversed = sample.comparison == D3D11_COMPARISON_GREATER
                              || sample.comparison == D3D11_COMPARISON_GREATER_EQUAL
                              || sample.comparison == D3D11_COMPARISON_EQUAL;
        if ((convention == Convention::standard && !standard)
            || (convention == Convention::reversed && !reversed)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t identity_count(const ClassEntry& entry) noexcept {
    return static_cast<std::uint32_t>(std::count_if(
        entry.identities.begin(), entry.identities.end(), [](const IdentityEntry& value) {
            return value.identity != 0;
        }));
}

void clear_evidence_locked() noexcept {
    g_classes = {};
    g_contexts = {};
    g_classCount = 0;
    g_contextCursor = 0;
    g_lastBoundClass = kClassCapacity;
    g_lastBindingSequence = 0;
    g_bindings = 0;
    g_stateChanges = 0;
    g_clears = 0;
    g_presentFrames = 0;
    g_lastObservationTick = 0;
    g_lastEngineFrame = 0;
    g_lastViewPublication = 0;
    g_selectedThreadId = 0;
    g_dropped.store(0, std::memory_order_release);
}

void clear_mailbox() noexcept {
    CapturedDepth stale{};
    AcquireSRWLockExclusive(&g_mailboxLock);
    stale = g_mailbox;
    g_mailbox = {};
    ReleaseSRWLockExclusive(&g_mailboxLock);
    if (stale.view != nullptr) {
        stale.view->Release();
    }
}

void publish_mailbox(ID3D11DepthStencilView* view,
                     const Descriptor& descriptor,
                     Convention convention) noexcept {
    if (view == nullptr || convention == Convention::unknown
        || !g_captureRequested.load(std::memory_order_acquire)
        || !g_captureArmed.load(std::memory_order_acquire)
        || !TryAcquireSRWLockExclusive(&g_mailboxLock)) {
        return;
    }
    view->AddRef();
    ID3D11DepthStencilView* stale = g_mailbox.view;
    g_mailbox.view = view;
    g_mailbox.descriptor = descriptor;
    g_mailbox.convention = convention;
    g_mailbox.sequence = g_captureSequence.fetch_add(1, std::memory_order_acq_rel) + 1U;
    ReleaseSRWLockExclusive(&g_mailboxLock);
    if (stale != nullptr) {
        stale->Release();
    }
}

} // namespace

InternalCallScope::InternalCallScope() noexcept {
    ++g_internalCallDepth;
}

InternalCallScope::~InternalCallScope() noexcept {
    if (g_internalCallDepth != 0) {
        --g_internalCallDepth;
    }
}

bool supported_depth_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D24_UNORM_S8_UINT
           || format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

bool descriptor_matches_viewport(const Descriptor& descriptor,
                                 const inspection::RenderViewport& viewport) noexcept {
    if (!descriptor.singleSliceMipZero || descriptor.width == 0 || descriptor.height == 0
        || descriptor.sampleCount == 0 || !supported_depth_format(descriptor.format)
        || viewport.width <= 0.0F || viewport.height <= 0.0F) {
        return false;
    }
    return std::fabs(static_cast<float>(descriptor.width) - viewport.width) <= 1.0F
           && std::fabs(static_cast<float>(descriptor.height) - viewport.height) <= 1.0F;
}

Convention classify_convention(float clearDepth, D3D11_COMPARISON_FUNC comparison) noexcept {
    const bool standardCompare = comparison == D3D11_COMPARISON_LESS
                                 || comparison == D3D11_COMPARISON_LESS_EQUAL
                                 || comparison == D3D11_COMPARISON_EQUAL;
    const bool reversedCompare = comparison == D3D11_COMPARISON_GREATER
                                 || comparison == D3D11_COMPARISON_GREATER_EQUAL
                                 || comparison == D3D11_COMPARISON_EQUAL;
    if (std::fabs(clearDepth - 1.0F) <= kClearTolerance && standardCompare) {
        return Convention::standard;
    }
    if (std::fabs(clearDepth) <= kClearTolerance && reversedCompare) {
        return Convention::reversed;
    }
    return Convention::unknown;
}

bool proof_eligible(const ProofEvidence& evidence) noexcept {
    return evidence.selectedDevice && evidence.viewportMatches
           && evidence.descriptor.singleSliceMipZero
           && supported_depth_format(evidence.descriptor.format)
           && evidence.observedFrames >= kProofFrameCount
           && evidence.qualifyingFrames >= kRequiredQualifyingFrames
           && evidence.distinctMatrixHashes >= kRequiredMatrixHashes
           && evidence.eligibleClassCount == 1 && evidence.clearConsistent
           && evidence.comparisonConsistent
           && classify_convention(evidence.clearDepth, evidence.comparison) != Convention::unknown;
}

void select_device(ID3D11Device* device,
                   ID3D11DeviceContext* immediateContext,
                   bool contextVtableMatches) noexcept {
    g_selectedDevice.store(nullptr, std::memory_order_release);
    g_selectedContext.store(nullptr, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lock);
    clear_evidence_locked();
    g_hooksResolved.store(contextVtableMatches, std::memory_order_release);
    g_vtableMatches.store(contextVtableMatches, std::memory_order_release);
    if (device != nullptr && immediateContext != nullptr && contextVtableMatches) {
        ContextEntry& immediate = context_entry(immediateContext);
        immediate.boundClass = kClassCapacity;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (device != nullptr && immediateContext != nullptr && contextVtableMatches) {
        g_selectedDevice.store(device, std::memory_order_release);
        g_selectedContext.store(immediateContext, std::memory_order_release);
    }
}

void clear_selection() noexcept {
    g_captureRequested.store(false, std::memory_order_release);
    g_captureArmed.store(false, std::memory_order_release);
    g_provenClass.store(kClassCapacity, std::memory_order_release);
    g_provenConvention.store(Convention::unknown, std::memory_order_release);
    clear_mailbox();
    g_selectedContext.store(nullptr, std::memory_order_release);
    g_selectedDevice.store(nullptr, std::memory_order_release);
    g_vtableMatches.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lock);
    clear_evidence_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void reset() noexcept {
    clear_selection();
    g_hooksResolved.store(false, std::memory_order_release);
}

void set_capture_requested(bool requested) noexcept {
    g_captureRequested.store(requested, std::memory_order_release);
    if (!requested) {
        clear_mailbox();
    }
}

bool take_latest(CapturedDepth& captured) noexcept {
    captured = {};
    if (!g_captureRequested.load(std::memory_order_acquire)
        || !g_captureArmed.load(std::memory_order_acquire)
        || !TryAcquireSRWLockExclusive(&g_mailboxLock)) {
        return false;
    }
    captured = g_mailbox;
    g_mailbox = {};
    ReleaseSRWLockExclusive(&g_mailboxLock);
    return captured.view != nullptr;
}

void release_captured(CapturedDepth& captured) noexcept {
    if (captured.view != nullptr) {
        captured.view->Release();
    }
    captured = {};
}

void observe_render_targets(ID3D11DeviceContext* context,
                            ID3D11DepthStencilView* depthView) noexcept {
    // A null DSV is a real state transition. Forgetting it lets later
    // OMSetDepthStencilState calls contaminate the class that used to be bound. Only
    // known selected-device contexts are touched, avoiding GetDevice on this hot path.
    if (depthView == nullptr) {
        if (!selected_context_device(context)) {
            return;
        }
        if (!TryAcquireSRWLockExclusive(&g_lock)) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (ContextEntry* known = find_context(context); known != nullptr) {
            known->boundClass = kClassCapacity;
            known->boundIdentity = 0;
            known->boundSequence = 0;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (!selected_context_device(context)) {
        return;
    }
    Descriptor descriptor{};
    std::uintptr_t identity{};
    if (!describe_depth_view(depthView, descriptor, identity)) {
        if (TryAcquireSRWLockExclusive(&g_lock)) {
            context_entry(context).boundClass = kClassCapacity;
            ReleaseSRWLockExclusive(&g_lock);
        } else {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++g_bindings;
    g_selectedThreadId = GetCurrentThreadId();
    g_lastObservationTick = GetTickCount64();
    const std::size_t classIndex = find_or_add_class(descriptor);
    if (classIndex == g_classes.size()) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    ClassEntry& entry = g_classes[classIndex];
    ++entry.bindings;
    entry.sequence = ++g_lastBindingSequence;
    IdentityEntry& identityEntry = note_identity(entry, identity);
    ContextEntry& contextState = context_entry(context);
    contextState.boundClass = classIndex;
    contextState.boundIdentity = identity;
    contextState.boundSequence = entry.sequence;
    entry.lastBoundIdentity = identity;
    entry.lastClear = identityEntry.clearValid ? identityEntry.lastClear
                                               : std::numeric_limits<float>::quiet_NaN();
    entry.lastComparison = contextState.comparison;
    g_lastBoundClass = classIndex;
    const bool publish = g_captureArmed.load(std::memory_order_acquire)
                         && classIndex == g_provenClass.load(std::memory_order_acquire);
    const Convention proven = g_provenConvention.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(&g_lock);
    if (publish) {
        publish_mailbox(depthView, descriptor, proven);
    }
}

void observe_depth_state(ID3D11DeviceContext* context, ID3D11DepthStencilState* state) noexcept {
    if (!selected_context_device(context)) {
        return;
    }
    D3D11_COMPARISON_FUNC comparison = D3D11_COMPARISON_ALWAYS;
    if (state != nullptr) {
        D3D11_DEPTH_STENCIL_DESC descriptor{};
        state->GetDesc(&descriptor);
        comparison = descriptor.DepthEnable ? descriptor.DepthFunc : D3D11_COMPARISON_ALWAYS;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ContextEntry* known = find_context(context);
    if (known == nullptr) {
        known = &context_entry(context);
    }
    ++g_stateChanges;
    g_selectedThreadId = GetCurrentThreadId();
    g_lastObservationTick = GetTickCount64();
    known->comparison = comparison;
    if (known->boundClass < g_classCount && known->boundSequence == g_lastBindingSequence) {
        g_classes[known->boundClass].lastComparison = comparison;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void observe_depth_clear(ID3D11DeviceContext* context,
                         ID3D11DepthStencilView* depthView,
                         UINT clearFlags,
                         FLOAT depth) noexcept {
    if ((clearFlags & D3D11_CLEAR_DEPTH) == 0 || !selected_context_device(context)) {
        return;
    }
    Descriptor descriptor{};
    std::uintptr_t identity{};
    if (!describe_depth_view(depthView, descriptor, identity)) {
        return;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++g_clears;
    g_selectedThreadId = GetCurrentThreadId();
    g_lastObservationTick = GetTickCount64();
    const std::size_t classIndex = find_or_add_class(descriptor);
    if (classIndex == g_classes.size()) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    ClassEntry& entry = g_classes[classIndex];
    ++entry.clears;
    IdentityEntry& identityEntry = note_identity(entry, identity);
    identityEntry.lastClear = depth;
    identityEntry.clearValid = true;
    if (entry.sequence == g_lastBindingSequence && entry.lastBoundIdentity == identity) {
        entry.lastClear = depth;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void sample_present(const inspection::RenderViewSnapshot& view) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++g_presentFrames;
    g_lastEngineFrame = view.engineFrame;
    g_lastViewPublication = view.publication;
    const bool exactView = view.valid && view.exactNative;
    const std::uint64_t matrixHash = exactView ? hash_matrix(view.viewProjection) : 0;
    for (std::size_t index = 0; index < g_classCount; ++index) {
        ClassEntry& entry = g_classes[index];
        FrameSample& old = entry.frames[entry.frameCursor];
        if (entry.frameCount == entry.frames.size() && old.qualifying) {
            --entry.qualifyingCount;
        }
        const bool qualifying = exactView && index == g_lastBoundClass
                                && descriptor_matches_viewport(entry.descriptor, view.viewport);
        old = FrameSample{matrixHash, entry.lastClear, entry.lastComparison, qualifying};
        if (qualifying) {
            ++entry.qualifyingCount;
        }
        entry.frameCursor = (entry.frameCursor + 1U) % entry.frames.size();
        entry.frameCount =
            (std::min)(entry.frameCount + 1U, static_cast<std::uint32_t>(entry.frames.size()));
        entry.maximumDistinctMatrixHashes =
            (std::max)(entry.maximumDistinctMatrixHashes, distinct_hashes(entry));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

Status status() noexcept {
    Status output{};
    AcquireSRWLockShared(&g_lock);
    output.hooksResolved = g_hooksResolved.load(std::memory_order_acquire);
    output.selectedDevice = g_selectedDevice.load(std::memory_order_acquire) != nullptr;
    output.renderTargetBindings = g_bindings;
    output.depthStateChanges = g_stateChanges;
    output.depthClears = g_clears;
    output.presentFrames = g_presentFrames;
    output.lastObservationTick = g_lastObservationTick;
    output.lastEngineFrame = g_lastEngineFrame;
    output.lastViewPublication = g_lastViewPublication;
    output.droppedObservations = g_dropped.load(std::memory_order_acquire);
    output.descriptorClassCount = static_cast<std::uint32_t>(g_classCount);
    output.selectedThreadId = g_selectedThreadId;

    std::array<bool, kClassCapacity> baseEligible{};
    for (std::size_t index = 0; index < g_classCount; ++index) {
        const ClassEntry& entry = g_classes[index];
        const std::uint32_t matrices = entry.maximumDistinctMatrixHashes;
        const Convention convention = entry_convention(entry);
        const bool base = entry.frameCount >= kProofFrameCount
                          && entry.qualifyingCount >= kRequiredQualifyingFrames
                          && matrices >= kRequiredMatrixHashes && convention != Convention::unknown
                          && comparison_consistent(entry, convention);
        baseEligible[index] = base;
        if (base) {
            ++output.eligibleClassCount;
        }
    }

    std::size_t candidate = g_lastBoundClass;
    for (std::size_t index = 0; index < g_classCount; ++index) {
        if (baseEligible[index]) {
            candidate = index;
            break;
        }
    }
    if (candidate < g_classCount) {
        const ClassEntry& entry = g_classes[candidate];
        output.descriptor = entry.descriptor;
        output.candidateSequence = entry.sequence;
        output.observedFrames = entry.frameCount;
        output.qualifyingFrames = entry.qualifyingCount;
        output.currentDistinctMatrixHashes = distinct_hashes(entry);
        output.distinctMatrixHashes = entry.maximumDistinctMatrixHashes;
        output.resourceIdentityCount = identity_count(entry);
        output.clearDepth = entry.lastClear;
        output.comparison = entry.lastComparison;
        output.convention = entry_convention(entry);
        output.clearConsistent = clear_consistent(entry, output.convention);
        output.comparisonConsistent = comparison_consistent(entry, output.convention);
        output.viewportMatches = entry.qualifyingCount != 0;
        output.exactView = output.distinctMatrixHashes != 0;
        output.ambiguous = output.eligibleClassCount > 1;
        const ProofEvidence evidence{output.descriptor,
                                     output.observedFrames,
                                     output.qualifyingFrames,
                                     output.distinctMatrixHashes,
                                     output.eligibleClassCount,
                                     output.clearDepth,
                                     output.comparison,
                                     output.selectedDevice,
                                     output.viewportMatches,
                                     output.clearConsistent,
                                     output.comparisonConsistent};
        output.eligible = proof_eligible(evidence);
    }
    output.captureEnabled = output.eligible && kCaptureReleaseEnabled;
    output.captureRequested = g_captureRequested.load(std::memory_order_acquire);
    output.captureSequence = g_captureSequence.load(std::memory_order_acquire);
    g_provenClass.store(output.eligible ? candidate : kClassCapacity, std::memory_order_release);
    g_provenConvention.store(output.eligible ? output.convention : Convention::unknown,
                             std::memory_order_release);
    g_captureArmed.store(output.captureEnabled, std::memory_order_release);

    if (!output.hooksResolved) {
        output.failure = Failure::hooksUnavailable;
    } else if (!g_vtableMatches.load(std::memory_order_acquire)) {
        output.failure = Failure::contextVtableMismatch;
    } else if (!output.selectedDevice || candidate >= g_classCount) {
        output.failure = Failure::noCandidate;
    } else if (!output.descriptor.singleSliceMipZero
               || !supported_depth_format(output.descriptor.format)) {
        output.failure = Failure::unsupportedDescriptor;
    } else if (output.observedFrames < kProofFrameCount) {
        output.failure = Failure::insufficientFrames;
    } else if (output.qualifyingFrames < kRequiredQualifyingFrames) {
        output.failure = Failure::insufficientFrequency;
    } else if (output.distinctMatrixHashes < kRequiredMatrixHashes) {
        output.failure = Failure::insufficientMatrices;
    } else if (output.ambiguous) {
        output.failure = Failure::ambiguous;
    } else if (!output.clearConsistent
               || (output.convention != Convention::unknown && !output.comparisonConsistent)) {
        output.failure = Failure::conventionInconsistent;
    } else if (output.convention == Convention::unknown) {
        output.failure = Failure::conventionUnknown;
    } else if (output.droppedObservations != 0) {
        output.failure = Failure::observationsDropped;
    } else if (output.eligible && !kCaptureReleaseEnabled) {
        output.failure = Failure::observerOnly;
    } else {
        output.failure = Failure::none;
    }
    ReleaseSRWLockShared(&g_lock);
    return output;
}

const char* convention_name(Convention convention) noexcept {
    switch (convention) {
    case Convention::standard:
        return "standard-z";
    case Convention::reversed:
        return "reversed-z";
    default:
        return "unknown";
    }
}

const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::none:
        return "none";
    case Failure::hooksUnavailable:
        return "hooks unavailable";
    case Failure::contextVtableMismatch:
        return "context vtable mismatch";
    case Failure::noExactView:
        return "exact view unavailable";
    case Failure::noCandidate:
        return "no candidate";
    case Failure::unsupportedDescriptor:
        return "unsupported descriptor";
    case Failure::viewportMismatch:
        return "viewport mismatch";
    case Failure::insufficientFrames:
        return "collecting 300 frames";
    case Failure::insufficientFrequency:
        return "below 95 percent";
    case Failure::insufficientMatrices:
        return "fewer than 30 matrices";
    case Failure::conventionUnknown:
        return "depth convention unknown";
    case Failure::conventionInconsistent:
        return "depth convention inconsistent";
    case Failure::ambiguous:
        return "multiple eligible classes";
    case Failure::observationsDropped:
        return "observations dropped";
    case Failure::observerOnly:
        return "eligible (observer-only release)";
    }
    return "unknown";
}

} // namespace sunrise::client::hooks::graphics::renderer::depth_observer
