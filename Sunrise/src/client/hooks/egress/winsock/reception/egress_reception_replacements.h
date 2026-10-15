#pragma once

#include "../../platform/sdk.h"

namespace sunrise::client::hooks::egress::winsock::reception {

/** Forwards one contiguous receive without creating egress. */
int WSAAPI receive_bytes(SOCKET socket, char* buffer, int length, int flags) noexcept;

/** Forwards one vectored receive without creating egress. */
int WSAAPI receive_buffers(SOCKET socket,
                           LPWSABUF buffers,
                           DWORD bufferCount,
                           LPDWORD bytesReceived,
                           LPDWORD flags,
                           LPWSAOVERLAPPED overlapped,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

/** Forwards one contiguous datagram receive. */
int WSAAPI receive_bytes_from(SOCKET socket,
                              char* buffer,
                              int length,
                              int flags,
                              sockaddr* source,
                              int* sourceLength) noexcept;

/** Forwards one vectored datagram receive. */
int WSAAPI receive_buffers_from(SOCKET socket,
                                LPWSABUF buffers,
                                DWORD bufferCount,
                                LPDWORD bytesReceived,
                                LPDWORD flags,
                                sockaddr* source,
                                LPINT sourceLength,
                                LPWSAOVERLAPPED overlapped,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

} // namespace sunrise::client::hooks::egress::winsock::reception
