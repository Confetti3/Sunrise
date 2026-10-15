#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <type_traits>

#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

enum class Dialect : std::uint8_t {
    json,
    lua,
};

[[nodiscard]] bool append_indent(std::string& output, std::uint32_t depth, std::uint32_t width) {
    if (depth > (std::numeric_limits<std::size_t>::max)() / width) {
        return false;
    }
    output.append(static_cast<std::size_t>(depth) * width, ' ');
    return true;
}

/** Appends JSON/Lua-safe ASCII and refuses non-ASCII bytes without rollback. */
[[nodiscard]] bool append_quoted(std::string_view value, std::string& output) {
    // Escaped bytes are emitted as lowercase hex.
    static constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                output.append("\\u00");
                output.push_back(kHex[(byte >> 4U) & 0xFU]);
                output.push_back(kHex[byte & 0xFU]);
            } else if (byte <= 0x7FU) {
                output.push_back(static_cast<char>(byte));
            } else {
                return false;
            }
            break;
        }
    }
    output.push_back('"');
    return true;
}

[[nodiscard]] bool
render_value(const Value& value, Dialect dialect, std::uint32_t depth, std::string& output);

/** Uses JSON without a trailing comma and Lua with a trailing comma. */
[[nodiscard]] bool render_array(const Value::Array& values,
                                Dialect dialect,
                                std::uint32_t depth,
                                std::string& output) {
    if (values.empty()) {
        output.append(dialect == Dialect::json ? "[]" : "{}");
        return true;
    }
    const char open = dialect == Dialect::json ? '[' : '{';
    const char close = dialect == Dialect::json ? ']' : '}';
    const std::uint32_t width = dialect == Dialect::json ? 2U : 4U;
    output.push_back(open);
    output.push_back('\n');
    for (const Value& item : values) {
        if (!append_indent(output, depth + 1U, width)
            || !render_value(item, dialect, depth + 1U, output)) {
            return false;
        }
        output.push_back(',');
        output.push_back('\n');
    }
    if (dialect == Dialect::json) {
        output.erase(output.end() - 2);
    }
    return append_indent(output, depth, width) && (output.push_back(close), true);
}

/** Emits quoted keys with dialect-specific separators and comma rules. */
[[nodiscard]] bool render_object(const Value::Object& values,
                                 Dialect dialect,
                                 std::uint32_t depth,
                                 std::string& output) {
    if (values.empty()) {
        output.append("{}");
        return true;
    }
    const std::uint32_t width = dialect == Dialect::json ? 2U : 4U;
    output.push_back('{');
    output.push_back('\n');
    for (const auto& [key, item] : values) {
        if (!append_indent(output, depth + 1U, width)) {
            return false;
        }
        if (dialect == Dialect::lua) {
            output.push_back('[');
        }
        if (!append_quoted(key, output)) {
            return false;
        }
        output.append(dialect == Dialect::json ? ": " : "] = ");
        if (!render_value(item, dialect, depth + 1U, output)) {
            return false;
        }
        output.push_back(',');
        output.push_back('\n');
    }
    if (dialect == Dialect::json) {
        output.erase(output.end() - 2);
    }
    return append_indent(output, depth, width) && (output.push_back('}'), true);
}

/** Renders only the seven Value alternatives supported by both output dialects. */
[[nodiscard]] bool
render_value(const Value& value, Dialect dialect, std::uint32_t depth, std::string& output) {
    return std::visit(
        [&](const auto& item) -> bool {
            using Item = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) { // Empty spells differently.
                output.append(dialect == Dialect::json ? "null" : "nil");
                return true;
            } else if constexpr (std::is_same_v<Item, bool>) { // Both dialects share the literals.
                output.append(item ? "true" : "false");
                return true;
            } else if constexpr (std::is_same_v<Item, std::uint64_t>
                                 || std::is_same_v<Item, std::int64_t>) { // No float form.
                output.append(std::to_string(item));
                return true;
            } else if constexpr (std::is_same_v<Item, std::string>) { // Non-ASCII is refused.
                return append_quoted(item, output);
            } else if constexpr (std::is_same_v<Item, Value::Array>) { // Objects take the last arm.
                return render_array(item, dialect, depth, output);
            } else {
                return render_object(item, dialect, depth, output);
            }
        },
        value.data);
}

/** Appends the seven declared name lanes for one field prefix. */
void append_name_fields(Value::Object& output, std::string_view prefix) {
    const auto add = [&output, prefix](std::string_view suffix, std::string_view type) {
        std::string name(prefix);
        name.append(suffix);
        output.push_back({std::move(name), string(type)});
    };
    add("_name", "string_or_nil");
    add("_name_provenance", "unresolved_package_inline_package_path_or_identifier_candidate");
    add("_name_provenance_code", "u32");
    add("_name_candidate_count", "u32");
    add("_name_source_tag", "u32");
    add("_name_source_class", "u32");
    add("_name_strongest_tier_overflow", "boolean");
}

} // namespace

Value null_value() {
    return {};
}

Value boolean(bool value) {
    return Value{value};
}

Value number(std::uint64_t value) {
    return Value{value};
}

Value signed_number(std::int64_t value) {
    return Value{value};
}

Value string(std::string_view value) {
    return Value{std::string(value)};
}

Value array(Value::Array values) {
    return Value{std::move(values)};
}

Value object(Value::Object values) {
    std::sort(values.begin(), values.end(), [](const auto& first, const auto& second) {
        return first.first < second.first;
    });
    return Value{std::move(values)};
}

Value string_array(std::initializer_list<std::string_view> values) {
    Value::Array output;
    output.reserve(values.size());
    for (const std::string_view value : values) {
        output.push_back(string(value));
    }
    return array(std::move(output));
}

void sort_keys(Value::Object& value) {
    std::sort(value.begin(), value.end(), [](const auto& first, const auto& second) {
        return first.first < second.first;
    });
}

Value field_map(std::initializer_list<Field> source) {
    Value::Object output;
    output.reserve(source.size());
    for (const auto& [name, type] : source) {
        output.push_back({std::string(name), string(type)});
    }
    sort_keys(output);
    return object(std::move(output));
}

/**
 * Builds one view's declared fields, expanding each prefix into its seven name lanes.
 * @param source Plain fields.
 * @param prefixes Field prefixes that each carry the name lanes.
 * @param rowScope Row scope the view declares.
 * @return The view object.
 */
Value named_view(std::initializer_list<Field> source,
                 std::initializer_list<std::string_view> prefixes,
                 std::string_view rowScope) {
    Value::Object fields;
    fields.reserve(source.size() + prefixes.size() * 7U);
    for (const auto& [name, type] : source) {
        fields.push_back({std::string(name), string(type)});
    }
    for (const std::string_view prefix : prefixes) {
        append_name_fields(fields, prefix);
    }
    sort_keys(fields);
    return object({
        {"fields", object(std::move(fields))},
        {"immutable", boolean(true)},
        {"row_scope", string(rowScope)},
        {"stale_read", string("throws")},
    });
}

Value view(std::initializer_list<Field> fields, std::string_view rowScope) {
    return object({
        {"fields", field_map(fields)},
        {"immutable", boolean(true)},
        {"row_scope", string(rowScope)},
        {"stale_read", string("throws")},
    });
}

bool render_json(const Value& value, std::uint32_t initialDepth, std::string& output) noexcept {
    output.clear();
    try {
        return render_value(value, Dialect::json, initialDepth, output);
    } catch (...) {
        output.clear();
        return false;
    }
}

bool render_lua(const Value& value, std::uint32_t initialDepth, std::string& output) noexcept {
    output.clear();
    try {
        return render_value(value, Dialect::lua, initialDepth, output);
    } catch (...) {
        output.clear();
        return false;
    }
}

std::string_view text(const Source& source, format::StringRef reference) noexcept {
    if (reference.length == 0 || reference.offset > source.strings.size()
        || reference.length > source.strings.size() - reference.offset) {
        return {};
    }
    return {reinterpret_cast<const char*>(source.strings.data() + reference.offset),
            reference.length};
}

/** Encodes the 32-byte digest in lowercase with an optional sha256 prefix. */
std::string digest_hex(const std::array<std::byte, 32>& digest, bool prefix) {
    // The digest is published in lowercase hex.
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve((prefix ? 7U : 0U) + digest.size() * 2U);
    if (prefix) {
        output.append("sha256:");
    }
    for (const std::byte byte : digest) {
        const unsigned value = std::to_integer<unsigned>(byte);
        output.push_back(kHex[(value >> 4U) & 0xFU]);
        output.push_back(kHex[value & 0xFU]);
    }
    return output;
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
