#include "player_hold.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

#include "../../player/player_position.h"
#include "../noclip/runtime.h"
#include "../teleport/runtime.h"
#include "../viewer_camera/viewer_camera.h"

namespace sunrise::client::hooks::player_hold {
namespace {

struct Anchor {
    Vector position{};
    std::uintptr_t componentIdentity{};
    std::uintptr_t bodyIdentity{};
    std::uint64_t session{};
    bool valid{};
};

std::atomic_bool g_holding{};
std::atomic_uint64_t g_holdingSession{};
SRWLOCK g_anchorLock{SRWLOCK_INIT};
Anchor g_anchor{};

void invalidate_anchor() noexcept {
    g_holding.store(false, std::memory_order_release);
    g_holdingSession.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&g_anchorLock);
    g_anchor = {};
    ReleaseSRWLockExclusive(&g_anchorLock);
}

[[nodiscard]] bool
anchor_for(void* component, void* body, std::uint64_t session, Anchor& output) noexcept {
    if (component == nullptr || body == nullptr || session == 0) {
        return false;
    }
    const auto componentIdentity = reinterpret_cast<std::uintptr_t>(component);
    const auto bodyIdentity = reinterpret_cast<std::uintptr_t>(body);

    AcquireSRWLockExclusive(&g_anchorLock);
    if (!g_anchor.valid || g_anchor.session != session
        || g_anchor.componentIdentity != componentIdentity
        || g_anchor.bodyIdentity != bodyIdentity) {
        noclip::Vector position{};
        noclip::read_body_position(body, position);
        g_anchor = Anchor{position, componentIdentity, bodyIdentity, session, true};
    }
    output = g_anchor;
    ReleaseSRWLockExclusive(&g_anchorLock);
    return output.valid;
}

[[nodiscard]] bool
current_anchor(void* component, void* body, std::uint64_t session, Anchor& output) noexcept {
    const auto componentIdentity = reinterpret_cast<std::uintptr_t>(component);
    const auto bodyIdentity = reinterpret_cast<std::uintptr_t>(body);
    AcquireSRWLockShared(&g_anchorLock);
    output = g_anchor;
    ReleaseSRWLockShared(&g_anchorLock);
    return output.valid && output.session == session
           && output.componentIdentity == componentIdentity && output.bodyIdentity == bodyIdentity;
}

void restore_body(void* body, const Vector& position) noexcept {
    noclip::write_body_position(body, position);
    noclip::write_body_velocity(body, {});
}

} // namespace

StepContext before_havok_step() noexcept {
    StepContext context{};
    const std::uint64_t session = viewer::camera::active_session();
    if (session == 0) {
        g_holding.store(false, std::memory_order_release);
        g_holdingSession.store(0, std::memory_order_release);
        return context;
    }
    void* const component = client::player::position::component_candidate();
    if (!teleport::owns_local_player(component)) {
        invalidate_anchor();
        return context;
    }
    void* const body = teleport::transient_body(component);
    Anchor anchor{};
    if (!anchor_for(component, body, session, anchor)) {
        return context;
    }
    noclip::write_body_velocity(body, {});
    context.anchor = anchor.position;
    context.body = body;
    context.componentIdentity = anchor.componentIdentity;
    context.bodyIdentity = anchor.bodyIdentity;
    context.session = session;
    context.valid = true;
    return context;
}

void after_havok_step(const StepContext& context) noexcept {
    if (!context.valid || context.session == 0
        || context.session != viewer::camera::active_session()) {
        return;
    }
    void* const component = client::player::position::component_candidate();
    if (reinterpret_cast<std::uintptr_t>(component) != context.componentIdentity
        || !teleport::owns_local_player(component)) {
        invalidate_anchor();
        return;
    }
    void* const body = teleport::transient_body(component);
    if (body != context.body || reinterpret_cast<std::uintptr_t>(body) != context.bodyIdentity) {
        invalidate_anchor();
        return;
    }
    Anchor anchor{};
    if (!current_anchor(component, body, context.session, anchor)) {
        return;
    }
    restore_body(body, anchor.position);
    g_holdingSession.store(context.session, std::memory_order_release);
    g_holding.store(true, std::memory_order_release);
}

void apply_sync(void* component) noexcept {
    const std::uint64_t session = viewer::camera::active_session();
    if (session == 0 || !teleport::owns_local_player(component)) {
        return;
    }
    void* const body = teleport::transient_body(component);
    Anchor anchor{};
    if (!anchor_for(component, body, session, anchor)) {
        return;
    }
    restore_body(body, anchor.position);
    g_holdingSession.store(session, std::memory_order_release);
    g_holding.store(true, std::memory_order_release);
}

bool holding() noexcept {
    const std::uint64_t session = viewer::camera::active_session();
    return session != 0 && g_holding.load(std::memory_order_acquire)
           && g_holdingSession.load(std::memory_order_acquire) == session;
}

bool blocks_teleport() noexcept {
    return viewer::camera::requested() || viewer::camera::active_session() != 0;
}

ReanchorResult reanchor(void* component, const Vector& position) noexcept {
    const std::uint64_t session = viewer::camera::active_session();
    if (session == 0) {
        return ReanchorResult::inactive;
    }
    if (!teleport::owns_local_player(component)) {
        return ReanchorResult::failed;
    }
    void* const body = teleport::transient_body(component);
    if (body == nullptr) {
        return ReanchorResult::failed;
    }
    const auto componentIdentity = reinterpret_cast<std::uintptr_t>(component);
    const auto bodyIdentity = reinterpret_cast<std::uintptr_t>(body);
    AcquireSRWLockExclusive(&g_anchorLock);
    g_anchor = Anchor{position, componentIdentity, bodyIdentity, session, true};
    ReleaseSRWLockExclusive(&g_anchorLock);
    restore_body(body, position);
    g_holdingSession.store(session, std::memory_order_release);
    g_holding.store(true, std::memory_order_release);
    return ReanchorResult::updated;
}

void reset() noexcept {
    invalidate_anchor();
}

} // namespace sunrise::client::hooks::player_hold
