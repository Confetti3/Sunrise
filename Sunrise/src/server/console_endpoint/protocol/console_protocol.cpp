#include "console_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../core/console/registry/console_registry.h"

namespace sunrise::server::console_endpoint::protocol {

namespace console = core::console;
namespace registry = core::console::registry;

namespace {

/** JSON writes no code point below this as itself; each has to arrive escaped. */
constexpr unsigned char kFirstPrintableCodePoint = 0x20;
/** JSON integers are base 10 with no prefix. */
constexpr std::uint64_t kDecimalRadix = 10;
/** Bits one nibble carries, which is what the `\uXXXX` escape shifts by to spell a byte out. */
constexpr unsigned kNibbleBits = 4;
/** Low nibble of one byte. */
constexpr unsigned kNibbleMask = 0x0F;
/**
 * Digits one number may print as. Twenty covers a 64-bit integer and twenty-four the widest
 * shortest-form double, so this leaves room for either and its sign.
 */
constexpr std::size_t kNumberCapacity = 32;
/** Bytes one request key may carry. The longest this reads is `describe`. */
constexpr std::size_t kKeyCapacity = 16;
/** Closes the array a response carries. */
constexpr std::string_view kArrayClose = "]";
/** Names a cut, so a caller that never reads this knows it was given everything. */
constexpr std::string_view kTruncatedFlag = ",\"truncated\":true";
/** Closes the response object. */
constexpr std::string_view kObjectClose = "}";
/** Opens the array a result carries, with the punctuation that precedes it. */
constexpr std::string_view kRowsOpen = ",\"rows\":[";
/** Opens the array a describe carries, with the punctuation that precedes it. */
constexpr std::string_view kEntriesOpen = ",\"entries\":[";
/** Bytes held back for the closing until it is written. */
constexpr std::size_t kClosingSize =
    kArrayClose.size() + kTruncatedFlag.size() + kObjectClose.size();
/**
 * Bytes held back for the array opening until it is written.
 *
 * The summary sits between the head and the opening, so without this a long summary could fill
 * the buffer and leave the array it belongs to unopened — an object that closes an array it
 * never started.
 */
constexpr std::size_t kOpeningSize = kRowsOpen.size() > kEntriesOpen.size() ? kRowsOpen.size()
                                                                            : kEntriesOpen.size();
/** Digits an unsigned 64-bit id may print as. */
constexpr std::size_t kWidestIdDigits = 20;

// `kMinimumCapacity` is published in the header, so a caller sizing a buffer can assert against it.
// The assertion below is what holds it above the widest response carrying nothing of the caller's
// own.
// `wrongArgumentCount` is the longest name any status prints as.
static_assert(kMinimumCapacity >= std::string_view{"{\"id\":"}.size() + kWidestIdDigits
                                      + std::string_view{",\"status\":\""}.size()
                                      + status_name(console::Status::wrongArgumentCount).size()
                                      + std::string_view{"\",\"summary\":\"\""}.size()
                                      + kOpeningSize + kClosingSize);
static_assert(kResponseCapacity >= kMinimumCapacity);
static_assert(kRegistryCapacity >= kMinimumCapacity);

/**
 * A bounded writer that cuts rather than overruns.
 *
 * `reserved` is what keeps a cut object closable: every write leaves that many bytes free, so the
 * closing brace and the flag that names the cut always have somewhere to go.
 */
struct Writer {
    std::span<char> buffer;
    std::size_t length{};
    std::size_t reserved{};
    bool truncated{};
};

/**
 * Appends raw bytes, or appends nothing and marks the writer cut.
 *
 * @param writer Destination.
 * @param raw Bytes to append, already in their wire form.
 */
void put(Writer& writer, std::string_view raw) noexcept {
    const std::size_t free = writer.buffer.size() - writer.length;
    if (free < writer.reserved || free - writer.reserved < raw.size()) {
        writer.truncated = true;
        return;
    }
    for (const char byte : raw) {
        writer.buffer[writer.length] = byte;
        ++writer.length;
    }
}

/** @param value One nibble. @return Its lowercase hexadecimal digit. */
[[nodiscard]] constexpr char hex_digit(unsigned value) noexcept {
    constexpr unsigned kDecimalDigits = 10;
    return value < kDecimalDigits ? static_cast<char>('0' + value)
                                  : static_cast<char>('a' + (value - kDecimalDigits));
}

/**
 * Appends one JSON string, escaping what JSON requires and nothing else.
 *
 * A byte at or above the printable floor goes out unchanged, so text that arrived as UTF-8 leaves
 * as the same UTF-8. Room for the closing quote is held back before the first byte is written, so
 * a string that runs out of room is short but still closed and the writer is marked cut.
 *
 * @param writer Destination.
 * @param value Text to quote and escape.
 */
void put_string(Writer& writer, std::string_view value) noexcept {
    put(writer, "\"");
    ++writer.reserved;
    for (const char byte : value) {
        if (writer.truncated) {
            break;
        }
        switch (byte) {
        case '"':
            put(writer, "\\\"");
            continue;
        case '\\':
            put(writer, "\\\\");
            continue;
        case '\b':
            put(writer, "\\b");
            continue;
        case '\f':
            put(writer, "\\f");
            continue;
        case '\n':
            put(writer, "\\n");
            continue;
        case '\r':
            put(writer, "\\r");
            continue;
        case '\t':
            put(writer, "\\t");
            continue;
        default:
            break;
        }
        const auto code = static_cast<unsigned>(static_cast<unsigned char>(byte));
        if (code < kFirstPrintableCodePoint) {
            // A control byte with no short escape still has to leave as an escape, or a reader
            // would take the raw byte for the end of the response.
            const std::array<char, 6> escape{
                '\\', 'u', '0', '0', hex_digit(code >> kNibbleBits), hex_digit(code & kNibbleMask)};
            put(writer, std::string_view{escape.data(), escape.size()});
            continue;
        }
        put(writer, std::string_view{&byte, 1});
    }
    --writer.reserved;
    put(writer, "\"");
}

/**
 * Appends one number in `std::to_chars` shortest form, which reads back as the same value.
 *
 * @param writer Destination.
 * @param value Number to print.
 */
template <typename Number> void put_number(Writer& writer, Number value) noexcept {
    std::array<char, kNumberCapacity> digits{};
    const std::to_chars_result written =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (written.ec != std::errc{}) {
        // Unreachable at this width. A number that would not print still must not print as
        // nothing, which would leave the object unparseable rather than merely wrong.
        put(writer, "null");
        return;
    }
    put(writer,
        std::string_view{digits.data(), static_cast<std::size_t>(written.ptr - digits.data())});
}

/**
 * Appends one real number.
 *
 * JSON has neither an infinity nor a NaN, so a value that is neither goes out as `null`: a caller
 * reading `null` knows it was given no number, where a bare `inf` token would cost it the whole
 * response rather than the one field.
 *
 * @param writer Destination.
 * @param value Number to print.
 */
void put_real(Writer& writer, double value) noexcept {
    if (!std::isfinite(value)) {
        put(writer, "null");
        return;
    }
    put_number(writer, value);
}

/**
 * Appends one typed value in the JSON form its declared domain calls for.
 *
 * @param writer Destination.
 * @param value Value to print.
 */
void put_value(Writer& writer, const console::Value& value) noexcept {
    switch (value.type) {
    case console::Type::boolean:
        put(writer, value.boolean ? "true" : "false");
        return;
    case console::Type::integer:
        put_number(writer, value.integer);
        return;
    case console::Type::real:
        put_real(writer, value.real);
        return;
    case console::Type::text: {
        const std::size_t safeLength = (std::min)(value.textLength, value.text.size());
        put_string(writer, std::string_view{value.text.data(), safeLength});
        return;
    }
    case console::Type::count:
        break;
    }
    // The sentinel is not a domain, so there is no value to print and `null` says exactly that.
    put(writer, "null");
}

/** @param kind Entry kind to name. @return Its wire name, as a describe carries it. */
[[nodiscard]] constexpr std::string_view kind_name(registry::Kind kind) noexcept {
    return kind == registry::Kind::variable ? "variable" : "command";
}

/**
 * Rolls one array element back when it did not fit whole.
 *
 * An element cut in half would close the array in the middle of a key, so a cut discards what it
 * started rather than leaving part of it behind. The writer stays marked, which is what stops the
 * enclosing loop and names the cut in the closing.
 *
 * @param writer Writer to inspect.
 * @param start Length the element began at.
 * @return True when the element fit and the array may continue.
 */
[[nodiscard]] bool keep_element(Writer& writer, std::size_t start) noexcept {
    if (!writer.truncated) {
        return true;
    }
    writer.length = start;
    return false;
}

/**
 * Closes the array and the object around it.
 *
 * A cut response is well-formed JSON that is quietly short, which is the worst thing to hand a
 * caller that cannot see the game for itself. The cut is named in the object rather than left to
 * be inferred from an entry that never arrived, so a caller that never sees the flag knows it
 * has everything.
 *
 * @param writer Destination.
 */
void close_object(Writer& writer) noexcept {
    const bool cut = writer.truncated;
    // The reserve existed for exactly these bytes, so it is released to let them through.
    writer.reserved = 0;
    put(writer, kArrayClose);
    if (cut) {
        put(writer, kTruncatedFlag);
    }
    put(writer, kObjectClose);
}

/**
 * Opens the array a response carries, releasing the room held back for it.
 *
 * @param writer Destination.
 * @param opening Array name with the punctuation that precedes it.
 */
void open_array(Writer& writer, std::string_view opening) noexcept {
    writer.reserved = kClosingSize;
    put(writer, opening);
}

/** Reads one flat request object: three known keys, no nesting, no arrays. */
struct Reader {
    std::string_view text;
    std::size_t position{};
};

/** Steps over the whitespace JSON allows between tokens. @param reader Reader to advance. */
void skip_whitespace(Reader& reader) noexcept {
    while (reader.position < reader.text.size()) {
        const char byte = reader.text[reader.position];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return;
        }
        ++reader.position;
    }
}

/**
 * Reads one expected punctuation byte.
 * @param reader Reader to advance. On failure it is left at the byte that did not match.
 * @param expected Byte the grammar calls for here.
 * @return True when that byte was there.
 */
[[nodiscard]] bool consume(Reader& reader, char expected) noexcept {
    skip_whitespace(reader);
    if (reader.position >= reader.text.size() || reader.text[reader.position] != expected) {
        return false;
    }
    ++reader.position;
    return true;
}

/**
 * Reads one expected keyword.
 * @param reader Reader to advance. On failure it is left at the word that did not match, so a
 * caller may try another.
 * @param word Keyword the grammar calls for here.
 * @return True when that keyword was there.
 */
[[nodiscard]] bool literal(Reader& reader, std::string_view word) noexcept {
    skip_whitespace(reader);
    if (!reader.text.substr(reader.position).starts_with(word)) {
        return false;
    }
    reader.position += word.size();
    return true;
}

/**
 * Reads one JSON string, unescaping it into a fixed buffer.
 *
 * `\u` is refused rather than decoded: a request carries a line the reader could have typed, and
 * a decoder that guessed at a code point would hand the console bytes nobody sent. A refusal is
 * something the caller can see and correct.
 *
 * A string that does not fit is refused rather than stored short. A console line cut to fit would
 * run as a different command, which is the one thing this must never do.
 *
 * @param reader Reader positioned at the opening quote.
 * @param buffer Destination. Its last byte is never written, so a zero-filled buffer stays
 * null-terminated at the stored length.
 * @param length Receives the unescaped length, only on success.
 * @return True when a whole string was read and it fit.
 */
[[nodiscard]] bool
read_string(Reader& reader, std::span<char> buffer, std::size_t& length) noexcept {
    if (!consume(reader, '"')) {
        return false;
    }
    std::size_t written = 0;
    while (reader.position < reader.text.size()) {
        const char byte = reader.text[reader.position];
        ++reader.position;
        if (byte == '"') {
            length = written;
            return true;
        }
        if (static_cast<unsigned char>(byte) < kFirstPrintableCodePoint) {
            return false;
        }
        char decoded = byte;
        if (byte == '\\') {
            if (reader.position >= reader.text.size()) {
                return false;
            }
            const char escape = reader.text[reader.position];
            ++reader.position;
            switch (escape) {
            case '"':
                decoded = '"';
                break;
            case '\\':
                decoded = '\\';
                break;
            case '/':
                decoded = '/';
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            default:
                return false;
            }
        }
        if (written + 1 >= buffer.size()) {
            return false;
        }
        buffer[written] = decoded;
        ++written;
    }
    return false;
}

/**
 * Reads the correlation id as a plain JSON unsigned integer.
 *
 * A quoted, negative or fractional id is refused rather than coerced: the id is the only thing
 * pairing a response with its request, so a caller that sent something else has to be told.
 *
 * @param reader Reader positioned at the number.
 * @param output Receives the id, only on success.
 * @return True when a whole unsigned integer was read.
 */
[[nodiscard]] bool read_id(Reader& reader, std::uint64_t& output) noexcept {
    skip_whitespace(reader);
    const std::size_t start = reader.position;
    std::uint64_t value = 0;
    while (reader.position < reader.text.size() && reader.text[reader.position] >= '0'
           && reader.text[reader.position] <= '9') {
        const auto digit = static_cast<std::uint64_t>(reader.text[reader.position] - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / kDecimalRadix) {
            return false;
        }
        value = value * kDecimalRadix + digit;
        ++reader.position;
    }
    if (reader.position == start) {
        return false;
    }
    // A fraction or an exponent is a number, but it is not an id anything ever issued.
    if (reader.position < reader.text.size()) {
        const char next = reader.text[reader.position];
        if (next == '.' || next == 'e' || next == 'E') {
            return false;
        }
    }
    // A leading zero is not how JSON writes a number, and accepting it would let one id arrive
    // spelled two ways.
    if (reader.position - start > 1 && reader.text[start] == '0') {
        return false;
    }
    output = value;
    return true;
}

/**
 * Reads a JSON true or false literal.
 * @param reader Reader positioned at the literal.
 * @param output Receives the flag, only on success.
 * @return True when a literal was read.
 */
[[nodiscard]] bool read_boolean(Reader& reader, bool& output) noexcept {
    if (literal(reader, "true")) {
        output = true;
        return true;
    }
    if (literal(reader, "false")) {
        output = false;
        return true;
    }
    return false;
}

/**
 * Writes the opening every response shares: its correlation id and its outcome.
 *
 * Both are bounded — an id at `kWidestIdDigits`, a status at its longest name — which is what lets
 * `kMinimumCapacity` promise that any response this writes has room to open and to close.
 *
 * @param writer Destination, already holding the closing reserve.
 * @param id Correlation id to echo back.
 * @param status Outcome to name.
 */
void put_opening(Writer& writer, std::uint64_t id, console::Status status) noexcept {
    put(writer, "{\"id\":");
    put_number(writer, id);
    put(writer, ",\"status\":\"");
    put(writer, status_name(status));
    put(writer, "\"");
}

/**
 * Refuses one request, handing back the id when the object got far enough to carry one.
 *
 * A typo anywhere in an otherwise well-addressed request used to be answered on zero, and since
 * nothing in this system times out, a caller indexing its replies by id waited on that request for
 * the rest of the session. The id is the one field worth recovering from a failure, because it is
 * the only one that says who to tell.
 *
 * @param candidate What was read before the failure.
 * @param hasId True when the id field was read whole.
 * @param output Receives the recovered id, and nothing else.
 * @return False, always — the caller's refusal, spelled once.
 */
[[nodiscard]] bool refuse(const Request& candidate, bool hasId, Request& output) noexcept {
    // Unconditional, not just when `hasId`: a caller that reuses one `Request` across lines would
    // otherwise have this leave its previous id in place, and a refusal on that stale id would
    // mis-correlate to a request already answered rather than to the one that just failed.
    output.id = hasId ? candidate.id : 0;
    return false;
}

} // namespace

/** Reads one request object. */
bool decode_request(std::string_view text, Request& output) noexcept {
    Reader reader{text, 0};
    Request candidate{};
    bool hasId = false;
    bool hasLine = false;
    bool hasDescribe = false;
    if (!consume(reader, '{')) {
        // Not even an object, so there is nowhere an id could have been. This is one of the two
        // refusals that cannot name what it is refusing.
        return refuse(candidate, hasId, output);
    }

    if (!consume(reader, '}')) {
        for (;;) {
            std::array<char, kKeyCapacity> key{};
            std::size_t keyLength = 0;
            if (!read_string(reader, key, keyLength) || !consume(reader, ':')) {
                return refuse(candidate, hasId, output);
            }
            const std::string_view name{key.data(), keyLength};
            // The id is required first so every later syntax refusal remains correlatable.
            if (!hasId && name != "id") {
                return refuse(candidate, hasId, output);
            }
            if (name == "id") {
                if (hasId || !read_id(reader, candidate.id)) {
                    return refuse(candidate, hasId, output);
                }
                hasId = true;
            } else if (name == "line") {
                if (hasLine || !read_string(reader, candidate.line, candidate.lineLength)) {
                    return refuse(candidate, hasId, output);
                }
                hasLine = true;
            } else if (name == "describe") {
                if (hasDescribe || !read_boolean(reader, candidate.describe)) {
                    return refuse(candidate, hasId, output);
                }
                hasDescribe = true;
            } else {
                // Refusing an unknown key is what keeps a misspelled one from looking accepted.
                return refuse(candidate, hasId, output);
            }
            if (consume(reader, '}')) {
                break;
            }
            if (!consume(reader, ',')) {
                return refuse(candidate, hasId, output);
            }
        }
    }

    // One object per line, so anything after the closing brace was never part of this request.
    skip_whitespace(reader);
    if (reader.position != reader.text.size()) {
        return refuse(candidate, hasId, output);
    }
    if (!hasId || candidate.id == 0) {
        // A zero id is refused, and recovering it would report the same zero either way.
        return refuse(candidate, hasId, output);
    }
    // Exactly one thing to do: an object carrying both leaves their order undefined, and one
    // carrying neither has nothing for the endpoint to answer.
    const bool asksDescribe = hasDescribe && candidate.describe;
    if (asksDescribe == hasLine) {
        return refuse(candidate, hasId, output);
    }
    output = candidate;
    return true;
}

/** Writes one result as a response object. */
void encode_result(std::uint64_t id,
                   const console::Result& result,
                   std::span<char> buffer,
                   std::size_t& length) noexcept {
    length = 0;
    if (buffer.size() < kMinimumCapacity) {
        return;
    }

    Writer writer{buffer, 0, kClosingSize + kOpeningSize, false};
    put_opening(writer, id, result.status);
    put(writer, ",\"summary\":");
    const std::size_t summaryStart = writer.length;
    const std::size_t safeSummaryLength =
        (std::min)(result.summaryLength, result.summary.size());
    put_string(writer, std::string_view{result.summary.data(), safeSummaryLength});
    if (!keep_element(writer, summaryStart)) {
        // The head has to stay a bounded width for the reserve to mean anything, so a summary
        // that will not fit is dropped whole rather than cut, and the closing says so.
        put(writer, "\"\"");
    }
    open_array(writer, kRowsOpen);
    const std::size_t safeRowCount = (std::min)(result.rowCount, result.rows.size());
    for (std::size_t index = 0; index < safeRowCount && !writer.truncated; ++index) {
        const std::size_t start = writer.length;
        if (index != 0) {
            put(writer, ",");
        }
        const console::Row& row = result.rows[index];
        put(writer, "{\"key\":");
        const std::size_t safeKeyLength = (std::min)(row.keyLength, row.key.size());
        put_string(writer, std::string_view{row.key.data(), safeKeyLength});
        put(writer, ",\"value\":");
        put_value(writer, row.value);
        put(writer, "}");
        static_cast<void>(keep_element(writer, start));
    }
    close_object(writer);
    length = writer.length;
}

/** Writes the whole registry as a response object. */
void encode_registry(std::uint64_t id, std::span<char> buffer, std::size_t& length) noexcept {
    length = 0;
    if (buffer.size() < kMinimumCapacity) {
        return;
    }

    const registry::RegistrySnapshot snapshot = registry::snapshot();
    const std::span<const registry::Descriptor> entries = snapshot.entries();

    Writer writer{buffer, 0, kClosingSize + kOpeningSize, false};
    put_opening(writer, id, console::Status::ok);
    open_array(writer, kEntriesOpen);
    for (std::size_t index = 0; index < entries.size() && !writer.truncated; ++index) {
        const std::size_t start = writer.length;
        if (index != 0) {
            put(writer, ",");
        }
        const registry::Descriptor& entry = entries[index];
        put(writer, "{\"name\":");
        put_string(writer, entry.name);
        put(writer, ",\"kind\":\"");
        put(writer, kind_name(entry.kind));
        put(writer, "\"");
        if (entry.kind == registry::Kind::variable) {
            // A command declares no value domain, so printing one would read as a claim it holds
            // a value the console could never show.
            put(writer, ",\"type\":\"");
            put(writer, console::type_name(entry.type));
            put(writer, "\"");
        }
        if (registry::has_bounds(entry)) {
            put(writer, ",\"minimum\":");
            put_real(writer, entry.minimum);
            put(writer, ",\"maximum\":");
            put_real(writer, entry.maximum);
        }
        if (!entry.choices.empty()) {
            // Declared choices are the one thing a caller cannot work out by trying, so a
            // describe that omitted them would leave it guessing at every enumerated variable.
            put(writer, ",\"choices\":[");
            for (std::size_t choice = 0; choice < entry.choices.size(); ++choice) {
                if (choice != 0) {
                    put(writer, ",");
                }
                put_string(writer, entry.choices[choice]);
            }
            put(writer, "]");
        }
        put(writer, ",\"help\":");
        put_string(writer, entry.help);
        put(writer, "}");
        static_cast<void>(keep_element(writer, start));
    }
    close_object(writer);
    length = writer.length;
}

} // namespace sunrise::server::console_endpoint::protocol
