#include "../coordinator/network_call_coordinator.h"
#include "../platform.h"
#include "internal.h"

namespace sunrise::client::hooks::network::http {
namespace {

#pragma code_seg(push, http_guard, ".srguard")
/** Exact protected replacement body for the common HTTP executor. */
__declspec(noinline) std::int64_t __fastcall execute_request_body(void* wrapper,
                                                                  std::byte* descriptor) noexcept {
    coordinator::CallLease lease;
    coordinator::g_callIngress(
        lease, HookSlot::httpExecuteRequest, coordinator::ConsumerKind::http);
    std::int64_t result{};
    __try {
        result = route_descriptor(lease, wrapper, descriptor);
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

#pragma code_seg(pop, http_guard)

} // namespace

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* execute_request_entry_point() noexcept {
    return reinterpret_cast<void*>(&execute_request_body);
}

} // namespace sunrise::client::hooks::network::http
