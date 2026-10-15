#pragma once

#include "../platform/sdk.h"

namespace sunrise::client::hooks::egress::dns {

/** Answers an ANSI DNS query from the local loopback name; address literals pass through. */
DNS_STATUS WINAPI query_a(PCSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept;

/** Answers a wide DNS query from the local loopback name; address literals pass through. */
DNS_STATUS WINAPI query_w(PCWSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept;

/** Answers a UTF-8 DNS query from the local loopback name; address literals pass through. */
DNS_STATUS WINAPI query_utf8(PCSTR name,
                             WORD type,
                             DWORD options,
                             PVOID extra,
                             PDNS_RECORD* results,
                             PVOID* reserved) noexcept;

/** Answers an extended DNS query from the local loopback name; address literals pass through. */
DNS_STATUS WINAPI query_ex(PDNS_QUERY_REQUEST request,
                           PDNS_QUERY_RESULT results,
                           PDNS_QUERY_CANCEL cancel) noexcept;

/** Blocks a raw DNS query without scheduling its callback. */
DNS_STATUS WINAPI query_raw(DNS_QUERY_RAW_REQUEST* request, DNS_QUERY_RAW_CANCEL* cancel) noexcept;

} // namespace sunrise::client::hooks::egress::dns
