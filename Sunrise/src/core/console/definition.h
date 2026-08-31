#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace sunrise::core::console {

/**
 * Entries the fixed registry holds. Every wired module publishes its own commands and variables
 * into this one table, so the bound covers their sum rather than any single module.
 */
inline constexpr std::size_t kEntryCapacity = 128;
/**
 * Storage for one entry name, which is always `module.member`. Both halves are readable words
 * rather than abbreviations, so this leaves room for a long pair and its null.
 */
inline constexpr std::size_t kNameCapacity = 48;
/**
 * Storage for one help line. The settings headers already justify each value in a sentence, and
 * this is what carries such a sentence over unshortened.
 */
inline constexpr std::size_t kHelpCapacity = 192;
/** Arguments one command declares. Past this a command is really two commands. */
inline constexpr std::size_t kArgumentCapacity = 4;
/** Named choices one text entry may offer. It is what bounds an enumerated variable. */
inline constexpr std::size_t kChoiceCapacity = 16;
/** Rows one result reports. A caller wanting more should ask a narrower question. */
inline constexpr std::size_t kRowCapacity = 16;
/**
 * Storage for one text value.
 *
 * A text value is one token a reader types, so this is sized to hold a long one rather than to
 * the longest any module happens to publish today, which the next module would change.
 */
inline constexpr std::size_t kTextCapacity = 64;

/**
 * Value domains an entry may declare.
 *
 * Every domain reads and writes as a single token, which is what lets the parser split a line on
 * spaces alone and lets one value print without an escape rule. An enumerated variable is `text`
 * carrying a choice list, not a domain of its own: that keeps the parser at four cases while
 * still giving completion and validation everything they need.
 */
enum class Type : std::uint8_t {
    boolean,
    integer,
    real,
    text,
    count,
};

/**
 * One typed value. The member the declared type names is the one that carries meaning; the rest
 * hold their zero. A plain struct rather than a union keeps this trivially copyable, which is
 * what lets a result cross the queue by value.
 */
struct Value {
    Type type{Type::boolean};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    std::array<char, kTextCapacity> text{};
    std::size_t textLength{};
};

/**
 * Outcomes one invocation may report.
 *
 * Each names a distinct cause, so a caller decides what to do without reading the summary. That
 * is what lets a machine consumer share one handler with the on-screen console.
 */
enum class Status : std::uint8_t {
    /** The invocation ran and its rows, if any, are meaningful. */
    ok,
    /** No entry carries that name. */
    unknownName,
    /** The entry exists but was given a different number of arguments than it declares. */
    wrongArgumentCount,
    /** An argument did not read as its declared type. */
    badArgument,
    /** An argument read correctly but fell outside the declared bounds or choices. */
    outOfRange,
    /** The entry was reached but declined, which is a state problem rather than an input one. */
    refused,
    /** The entry accepted the call and the work itself failed. */
    failed,
};

/** One named value a result reports. */
struct Row {
    std::array<char, kNameCapacity> key{};
    std::size_t keyLength{};
    Value value{};
};

/**
 * What one invocation reports back.
 *
 * The summary is a sentence for a reader; the rows are the same answer in named values. Both are
 * filled by the same handler so the two consumers never diverge: the console prints the summary
 * and then the rows, and a machine consumer reads the rows and ignores the prose.
 */
struct Result {
    Status status{Status::ok};
    std::array<char, kHelpCapacity> summary{};
    std::size_t summaryLength{};
    std::array<Row, kRowCapacity> rows{};
    std::size_t rowCount{};
};

/**
 * Folds one byte to lowercase, ASCII only.
 *
 * Entry names are lowercase by convention and the registry enforces it, so folding what a reader
 * types is enough to make every lookup and search forgiving about case.
 *
 * @param character Byte to fold.
 * @return Its lowercase form, or the byte unchanged.
 */
[[nodiscard]] constexpr char fold(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

/** @param text Text to test. @return True when no byte would change under folding. */
[[nodiscard]] constexpr bool is_folded(std::string_view text) noexcept {
    for (const char character : text) {
        if (fold(character) != character) {
            return false;
        }
    }
    return true;
}

/** @return True when both runs are equal once folded. */
[[nodiscard]] constexpr bool equals_folded(std::string_view first,
                                           std::string_view second) noexcept {
    if (first.size() != second.size()) {
        return false;
    }
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (fold(first[index]) != fold(second[index])) {
            return false;
        }
    }
    return true;
}

/** @return True when the folded needle appears anywhere in the folded haystack. */
[[nodiscard]] constexpr bool contains_folded(std::string_view haystack,
                                             std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) {
        return false;
    }
    // An empty needle matches at the first position, so the loop covers that case too.
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        if (equals_folded(haystack.substr(start, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

/** @return True when the folded haystack begins with the folded needle. */
[[nodiscard]] constexpr bool starts_with_folded(std::string_view haystack,
                                                std::string_view needle) noexcept {
    return needle.size() <= haystack.size()
           && equals_folded(haystack.substr(0, needle.size()), needle);
}

/**
 * Copies text into a fixed buffer, truncating rather than overrunning.
 * @param text Source text.
 * @param buffer Destination, cleared first.
 * @param length Receives the stored length.
 */
constexpr void
store_text(std::string_view text, std::span<char> buffer, std::size_t& length) noexcept {
    if (buffer.empty()) {
        length = 0;
        return;
    }
    length = text.size() < buffer.size() ? text.size() : buffer.size() - 1;
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        buffer[index] = index < length ? text[index] : '\0';
    }
}

/**
 * Sets the sentence a result reports to a reader.
 * @param result Result to fill.
 * @param summary Sentence, truncated when it does not fit.
 */
constexpr void set_summary(Result& result, std::string_view summary) noexcept {
    store_text(summary, result.summary, result.summaryLength);
}

/**
 * Appends one named value to a result.
 *
 * Rows are what a machine caller reads, so a handler that has an answer should add it here even
 * when the summary already states it in prose.
 *
 * @param result Result to append to.
 * @param key Row name.
 * @param value Row value.
 * @return True when the row fit.
 */
constexpr bool add_row(Result& result, std::string_view key, const Value& value) noexcept {
    if (result.rowCount >= kRowCapacity) {
        return false;
    }
    Row& row = result.rows[result.rowCount];
    store_text(key, row.key, row.keyLength);
    row.value = value;
    ++result.rowCount;
    return true;
}

/** @param type Domain to name. @return Its lowercase wire name, as help and completion print it. */
[[nodiscard]] constexpr std::string_view type_name(Type type) noexcept {
    switch (type) {
    case Type::boolean:
        return "bool";
    case Type::integer:
        return "int";
    case Type::real:
        return "float";
    case Type::text:
        return "text";
    case Type::count:
        break;
    }
    return "";
}

/** @param status Outcome to test. @return True when the invocation reached its handler. */
[[nodiscard]] constexpr bool reached_handler(Status status) noexcept {
    return status == Status::ok || status == Status::refused || status == Status::failed;
}

// Every domain a caller may declare has to print, or help and completion would show a blank type.
static_assert(!type_name(Type::boolean).empty());
static_assert(!type_name(Type::integer).empty());
static_assert(!type_name(Type::real).empty());
static_assert(!type_name(Type::text).empty());
// The sentinel is not a domain, so it deliberately has no name.
static_assert(type_name(Type::count).empty());

// An input rejection never runs a handler, which is what lets a caller retry it unchanged.
static_assert(!reached_handler(Status::unknownName));
static_assert(!reached_handler(Status::wrongArgumentCount));
static_assert(!reached_handler(Status::badArgument));
static_assert(!reached_handler(Status::outOfRange));
static_assert(reached_handler(Status::ok));
static_assert(reached_handler(Status::refused));
static_assert(reached_handler(Status::failed));

// Folding is what makes every lookup and search forgiving about how a reader typed a name.
static_assert(equals_folded("Movement.Fly", "movement.fly"));
static_assert(equals_folded("", ""));
static_assert(!equals_folded("movement", "movements"));
static_assert(contains_folded("movement.fly_speed", "SPEED"));
static_assert(contains_folded("movement.fly_speed", ""));
static_assert(!contains_folded("movement.fly_speed", "zzz"));
static_assert(starts_with_folded("movement.fly", "MOV"));
static_assert(!starts_with_folded("movement.fly", "fly"));
static_assert(is_folded("movement.fly_speed"));
static_assert(!is_folded("Movement"));

// A name has to outrun the widest text a row may key, or a row key would truncate silently.
static_assert(kNameCapacity <= kHelpCapacity);
// A result crosses the queue by value, so it must stay copyable without a destructor.
static_assert(std::is_trivially_copyable_v<Value>);

} // namespace sunrise::core::console
