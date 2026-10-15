#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::memory {

/**
 * Copies one range of this process through ReadProcessMemory.
 * @param context Unused callback context.
 * @param address Source address.
 * @param output Receives the bytes.
 * @return False unless every byte arrived.
 */
[[nodiscard]] bool
read_current_process(void* context, std::uintptr_t address, std::span<std::byte> output) noexcept;

/**
 * Copies one range of this process in place under a fault guard, with no system call.
 * @param context Unused callback context.
 * @param address Source address.
 * @param output Receives the bytes.
 * @return False when the address is null or the copy faults.
 */
[[nodiscard]] bool
copy_current_process(void* context, std::uintptr_t address, std::span<std::byte> output) noexcept;

} // namespace sunrise::client::memory
