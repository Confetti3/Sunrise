#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "activity_sdk_squad_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail {

/** @return True when one topology text owns exactly its declared bytes. */
[[nodiscard]] inline bool text_view(const topology_inventory::Text& text,
                                    std::string_view& output) noexcept {
    output = {};
    if (text.length >= text.value.size()) {
        return false;
    }
    output = std::string_view(text.value.data(), text.length);
    return text.value[text.length] == '\0';
}

/** Formats one bounded identity string. */
template <typename... Values>
[[nodiscard]] bool format_text(std::string& output, const char* pattern, Values... values) {
    std::array<char, 192> bytes{};
    const int count = std::snprintf(bytes.data(), bytes.size(), pattern, values...);
    if (count < 0 || static_cast<std::size_t>(count) >= bytes.size()) {
        return false;
    }
    output.assign(bytes.data(), static_cast<std::size_t>(count));
    return true;
}

/**
 * Appends one unsigned value in little-endian order.
 * @tparam Value Unsigned type whose whole width is appended.
 * @return False when the buffer cannot grow.
 */
template <typename Value>
[[nodiscard]] bool append_little(std::vector<std::byte>& output, Value value) {
    static_assert(std::is_unsigned_v<Value>);
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - sizeof(Value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & Value{0xFFU}));
    }
    return true;
}

/** Appends one length-prefixed hash input part. @return False when the buffer cannot grow. */
[[nodiscard]] inline bool append_hash_part(std::vector<std::byte>& output, std::string_view value) {
    if (!append_little(output, static_cast<std::uint64_t>(value.size()))
        || value.size() > (std::numeric_limits<std::size_t>::max)() - output.size()) {
        return false;
    }
    output.reserve(output.size() + value.size());
    for (const unsigned char byte : value) {
        output.push_back(static_cast<std::byte>(byte));
    }
    return true;
}

/** Derives one descriptor identity from its exact object and slot tuple. */
[[nodiscard]] bool descriptor_id(const topology_inventory::Snapshot& topology,
                                 const DescriptorFact& descriptor,
                                 std::string& output);

/** Checks the topology rows consumed by extraction and linking. */
[[nodiscard]] bool valid_topology(const topology_inventory::Snapshot& source) noexcept;

/** Validates all normalized facts before linking begins. */
[[nodiscard]] bool validate_facts(const topology_inventory::Snapshot& topology, const Facts& facts);

/** @return True when two parsed spawner definitions from different paths agree exactly. */
[[nodiscard]] bool same_spawner(const SpawnerFact& left, const SpawnerFact& right) noexcept;

/** @return True when two parsed spawn-rule definitions from different paths agree exactly. */
[[nodiscard]] bool same_rule(const RuleFact& left, const RuleFact& right) noexcept;

/** Parses one exact authored type-1 config. */
[[nodiscard]] bool
parse_spawner(std::uint32_t tag, std::span<const std::byte> blob, SpawnerFact& output);

/** Builds the rule row a spawner's own point set stands for. */
[[nodiscard]] RuleFact inline_rule(const SpawnerFact& spawner);

/** Parses one exact authored type-66 config. */
[[nodiscard]] bool parse_rule(std::uint32_t tag, std::span<const std::byte> blob, RuleFact& output);

/** Builds one member row and retains an unresolved actor link when needed. */
[[nodiscard]] bool build_member(const MemberFact& source,
                                std::string_view squadDigest,
                                std::uint32_t ordinal,
                                ActorResolver actorResolver,
                                void* actorContext,
                                SquadMember& output,
                                bool& actorLinksComplete);

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail
