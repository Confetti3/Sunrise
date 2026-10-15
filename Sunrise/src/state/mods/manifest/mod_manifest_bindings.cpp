#include "mod_manifest_bindings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>

#include "../../../core/json/reader.h"
#include "../../../middleware/content/packages/reader/layout.h"

namespace sunrise::state::mods::manifest {
namespace {

/** Profile names a binding may use. */
constexpr std::string_view kPropertyProfile = "cui-property-v1";
constexpr std::string_view kComparisonProfile = "cui-comparison-v1";
/** SHA-256 text has two hexadecimal characters per digest byte. */
constexpr std::size_t kHexCharactersPerByte = 2;
constexpr unsigned kHexNibbleBits = 4;
constexpr unsigned kHexLetterValue = 10;

/** Fields of one serialized binding, in the same order as kFields. */
enum class Field : std::size_t {
    key,
    profile,
    tag,
    classId,
    size,
    sourceSha256,
    propertyOffset,
    poolOffset,
    bindingOffset,
    offset,
    type,
    count,
};

/** This authored schema refuses unknown and repeated fields. */
constexpr std::array<std::string_view, static_cast<std::size_t>(Field::count)> kFields{
    "key",
    "profile",
    "tag",
    "class",
    "size",
    "sourceSha256",
    "propertyOffset",
    "poolOffset",
    "bindingOffset",
    "offset",
    "type"};

/**
 * Stores a bounded first refusal.
 * @param error Receives the text.
 * @param message Refusal without user-controlled format characters.
 * @param index One-based binding index, or zero for the array.
 * @return Always false.
 */
bool refuse(Error& error, const char* message, std::size_t index = 0) noexcept {
    const int size =
        index == 0
            ? std::snprintf(error.bytes.data(), error.bytes.size(), "configBindings: %s", message)
            : std::snprintf(
                  error.bytes.data(), error.bytes.size(), "config binding %zu: %s", index, message);
    error.length =
        size > 0 ? (std::min)(static_cast<std::size_t>(size), error.bytes.size() - 1) : 0;
    return false;
}

/** @return A hexadecimal nibble, or -1 for another character. */
int nibble(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + static_cast<int>(kHexLetterValue);
    }
    return value >= 'a' && value <= 'f' ? value - 'a' + static_cast<int>(kHexLetterValue) : -1;
}

/** Reads the `configBindings` array. */
class BindingParser final : private core::json::Reader {
public:
    BindingParser(std::string_view json, Error& error) noexcept : Reader(json), error_(error) {}

    /**
     * Reads every binding. check_bindings matches keys to `config` later.
     * @param output Receives the sites and their count.
     * @return False on the first format error.
     */
    bool read(Manifest& output) noexcept {
        output.configBindingCount = 0;
        if (!consume('[')) {
            return fail("must be an array");
        }
        if (consume(']')) {
            return at_end() || fail("text follows the array");
        }
        for (;;) {
            index_ = output.configBindingCount + 1;
            if (output.configBindingCount == output.configBindings.size()) {
                return fail("too many sites");
            }
            if (!site(output.configBindings[output.configBindingCount])) {
                return false;
            }
            ++output.configBindingCount;
            if (consume(']')) {
                return at_end() || fail("text follows the array");
            }
            if (!consume(',')) {
                return fail("expected ',' or ']'");
            }
        }
    }

private:
    /** @return False with the current binding identified. */
    bool fail(const char* message) noexcept {
        return refuse(error_, message, index_);
    }

    /**
     * Decodes one bounded JSON string.
     * @param output Receives text with a spare terminating byte.
     * @return False on a bad string or excess length.
     */
    template <std::size_t Capacity> bool text(Text<Capacity>& output) noexcept {
        std::string_view raw;
        output = {};
        return string(raw)
               && core::json::decode_string(
                   raw, std::span(output.bytes).first(Capacity - 1), output.length);
    }

    /** @param output Receives a uint32. @return False on another number representation. */
    bool number32(std::uint32_t& output) noexcept {
        std::uint64_t value = 0;
        if (!unsigned_integer(value) || value > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        output = static_cast<std::uint32_t>(value);
        return true;
    }

    /**
     * Reads a 64-character hex SHA-256.
     * @param output Receives the digest.
     * @return False unless exactly 64 hexadecimal characters were decoded.
     */
    bool digest(content_manifest::Fingerprint& output) noexcept {
        Text<content_manifest::kFingerprintSize * kHexCharactersPerByte + 1> value{};
        if (!text(value) || value.length != output.size() * kHexCharactersPerByte) {
            return false;
        }
        for (std::size_t index = 0; index < output.size(); ++index) {
            const int high = nibble(value.bytes[index * kHexCharactersPerByte]);
            const int low = nibble(value.bytes[index * kHexCharactersPerByte + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            output[index] = static_cast<std::byte>((static_cast<unsigned>(high) << kHexNibbleBits)
                                                   | static_cast<unsigned>(low));
        }
        return true;
    }

    /**
     * Reads a profile or a declared scalar type.
     * @param field The string field being read.
     * @param output Receives its decoded enum.
     * @return False on an unsupported value.
     */
    bool kind(Field field, ConfigBinding& output) noexcept {
        Text<kConfigKeyCapacity> value{};
        if (!text(value)) {
            return false;
        }
        if (field == Field::profile) {
            if (value.view() == kPropertyProfile) {
                output.profile = ConfigBindingProfile::cuiProperty;
                return true;
            }
            if (value.view() == kComparisonProfile) {
                output.profile = ConfigBindingProfile::cuiComparison;
                return true;
            }
            return false;
        }
        return config_type_name(value.view(), output.type);
    }

    /** @param field Known field. @param output Receives it. @return False on a bad value. */
    bool member(Field field, ConfigBinding& output) noexcept {
        switch (field) {
        case Field::key:
            return text(output.key) && valid_config_key(output.key.view());
        case Field::profile:
        case Field::type:
            return kind(field, output);
        case Field::tag:
            return number32(output.tag);
        case Field::classId:
            return number32(output.classId);
        case Field::size:
            return number32(output.size);
        case Field::sourceSha256:
            return digest(output.sourceSha256);
        case Field::propertyOffset:
            return number32(output.propertyOffset);
        case Field::poolOffset:
            return number32(output.poolOffset);
        case Field::bindingOffset:
            return number32(output.bindingOffset);
        case Field::offset:
            return number32(output.offset);
        case Field::count:
            return false;
        }
        return false;
    }

    /**
     * Reads one strict profile-specific object.
     * @param output Receives the site.
     * @return False on absent, duplicate, unknown or profile-inapplicable fields.
     */
    bool site(ConfigBinding& output) noexcept {
        output = {};
        if (!consume('{')) {
            return fail("must be an object");
        }
        std::array<bool, kFields.size()> seen{};
        if (!consume('}')) {
            for (;;) {
                std::string_view key;
                if (!string(key) || !consume(':')) {
                    return fail("expected a key");
                }
                const auto found = std::find(kFields.begin(), kFields.end(), key);
                if (found == kFields.end()) {
                    return fail("unknown field");
                }
                const auto index = static_cast<std::size_t>(found - kFields.begin());
                if (seen[index] || !member(static_cast<Field>(index), output)) {
                    return fail("repeated field or invalid value");
                }
                seen[index] = true;
                if (consume('}')) {
                    break;
                }
                if (!consume(',')) {
                    return fail("expected ',' or '}'");
                }
            }
        }
        const auto has = [&](Field field) { return seen[static_cast<std::size_t>(field)]; };
        if (!has(Field::key) || !has(Field::profile) || !has(Field::tag) || !has(Field::classId)
            || !has(Field::size) || !has(Field::sourceSha256) || !has(Field::offset)
            || !has(Field::type)) {
            return fail("missing required field");
        }
        const bool property = output.profile == ConfigBindingProfile::cuiProperty;
        if (property != has(Field::propertyOffset) || property != has(Field::poolOffset)
            || property == has(Field::bindingOffset)
            || (property && output.type == ConfigType::integer)
            || (!property && output.type == ConfigType::boolean)) {
            return fail("fields or scalar type do not match the profile");
        }
        return true;
    }

    Error& error_;
    std::size_t index_{};
};

} // namespace

/** Reads the bounded profile-specific binding array. */
bool read_bindings(std::string_view json, Manifest& output, Error& error) noexcept {
    BindingParser parser(json, error);
    return parser.read(output);
}

/** Checks source consistency and typed non-overlapping sites after declarations are known. */
bool check_bindings(const Manifest& manifest, Error& error) noexcept {
    namespace layout = middleware::content::packages::reader::layout;
    const auto entries = std::span(manifest.config).first(manifest.configCount);
    const auto sites = std::span(manifest.configBindings).first(manifest.configBindingCount);
    std::array<const ConfigBinding*, kConfigBindingLimit> ordered{};
    for (std::size_t index = 0; index < sites.size(); ++index) {
        const ConfigBinding& site = sites[index];
        const auto entry =
            std::find_if(entries.begin(), entries.end(), [&](const ConfigEntry& value) {
                return value.key.view() == site.key.view();
            });
        const std::size_t width =
            site.type == ConfigType::boolean ? sizeof(std::uint8_t) : sizeof(std::uint32_t);
        if (entry == entries.end() || entry->defaultValue.type != site.type) {
            return refuse(error, "key is undeclared or its type differs", index + 1);
        }
        if (site.tag < layout::kTagBase
            || ((site.tag - layout::kTagBase) >> layout::kTagEntryBits)
                   > content_manifest::kMaximumPackageId
            || site.classId != kConfigCuiResourceClass || site.size == 0
            || site.size > kConfigEntryByteLimit || site.offset > site.size
            || width > site.size - site.offset) {
            return refuse(error, "invalid entry identity or scalar range", index + 1);
        }
        ordered[index] = &site;
    }
    auto sorted = std::span(ordered).first(sites.size());
    std::sort(
        sorted.begin(), sorted.end(), [](const ConfigBinding* first, const ConfigBinding* second) {
            return first->tag != second->tag ? first->tag < second->tag
                                             : first->offset < second->offset;
        });
    for (std::size_t index = 1; index < sorted.size(); ++index) {
        const ConfigBinding& previous = *sorted[index - 1];
        const ConfigBinding& site = *sorted[index];
        if (site.tag != previous.tag) {
            continue;
        }
        const std::size_t width =
            previous.type == ConfigType::boolean ? sizeof(std::uint8_t) : sizeof(std::uint32_t);
        if (site.classId != previous.classId || site.size != previous.size
            || site.sourceSha256 != previous.sourceSha256) {
            return refuse(error, "one entry has inconsistent source identity");
        }
        if (site.offset - previous.offset < width) {
            return refuse(error, "scalar sites overlap");
        }
    }
    return true;
}

} // namespace sunrise::state::mods::manifest
