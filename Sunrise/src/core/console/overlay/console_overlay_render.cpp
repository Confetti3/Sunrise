#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../../resources/resource.h"
#include "../../ui/components/logo/ui_logo_component.h"
#include "../../ui/scaling/dpi/ui_dpi_scaling.h"
#include "../output/console_format.h"
#include "../output/console_output.h"
#include "../parser/console_line_parse.h"
#include "../queue/console_queue.h"
#include "../registry/console_registry.h"
#include "console_builtins.h"
#include "console_completion.h"
#include "console_history.h"
#include "console_overlay.h"

namespace sunrise::core::console::overlay {
namespace {

/** The console covers the top of the viewport, which is where a reader expects one to drop. */
constexpr float kViewportHeightShare = 0.45F;
/**
 * Suggestions shown at once.
 *
 * The list takes its rows from the scrollback, so a taller one is paid for in history a reader can
 * no longer see. Five is enough to choose from and leaves the answers above it readable.
 */
constexpr std::size_t kVisibleSuggestions = 5;
/** 1 scrolls to the bottom edge of the newest line. */
constexpr float kScrollBottom = 1.0F;
/** The prompt is the whole width, so a long line is edited without a horizontal scroll. */
constexpr float kFullWidth = -1.0F;
/** Fixed prompt label, kept out of the input so it cannot be edited away. */
constexpr char kPromptLabel[] = "##console_input";
/** Child region holding the scrollback. */
constexpr char kScrollbackId[] = "##console_scrollback";
/** Window title, hidden by the flags but still the window's identity. */
constexpr char kWindowTitle[] = "Sunrise Console##console_window";
/** 32 authored pixels make the mark as tall as the two rows beside it, as the HUD card does. */
constexpr float kHeaderLogoExtent = 32.0F;
/** The console names itself with the same wordmark every other surface carries. */
constexpr char kWordmark[] = "SUNRISE";
/** The lighter half of the brand gradient, so the wordmark reads as part of the mark. */
constexpr ImVec4 kWordmarkTint{0.965F, 0.886F, 0.478F, 1.0F};
/** Shown once, the first time the console opens, so the first screen is never blank. */
constexpr char kBannerHint[] = "Start typing to see what exists. Tab takes the highlighted name, "
                               "Up and Down move through it.";

/**
 * The console owns its whole strip, so nothing about it is moved, resized or remembered.
 *
 * It is deliberately allowed to come to the front. The HUD overlays draw whether a surface is
 * open or not and sit in the same corner, so a console held behind them would be read through
 * its own logo card.
 */
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                          | ImGuiWindowFlags_NoSavedSettings;
/** A border separates the lines already answered from the line being typed. */
constexpr ImGuiChildFlags kScrollbackFlags = ImGuiChildFlags_Borders;
/** Enter submits, and the two callbacks carry selection and insertion. */
constexpr ImGuiInputTextFlags kInputFlags = ImGuiInputTextFlags_EnterReturnsTrue
                                            | ImGuiInputTextFlags_CallbackHistory
                                            | ImGuiInputTextFlags_CallbackCompletion;

/** Echoed lines are dimmer than answers, so the eye finds the answers first. */
constexpr ImVec4 kInputTint{0.62F, 0.62F, 0.66F, 1.0F};
/** Answers keep the theme's own text color. */
constexpr ImVec4 kAnswerTint{0.90F, 0.90F, 0.92F, 1.0F};
/** A refusal is red enough to find while scrolling, without being the only cue. */
constexpr ImVec4 kFailureTint{0.93F, 0.44F, 0.40F, 1.0F};
/** What the console says about itself sits apart from what a module answered. */
constexpr ImVec4 kNoticeTint{0.55F, 0.75F, 0.95F, 1.0F};
/** An unselected suggestion is quiet enough that the selected one reads as the answer. */
constexpr ImVec4 kSuggestionTint{0.58F, 0.62F, 0.70F, 1.0F};
/** The selected suggestion carries the accent, which is what the pages use for a selection. */
constexpr ImVec4 kSelectedTint{0.95F, 0.42F, 0.16F, 1.0F};
/** A type, a range or a current value is context, never the thing being read. */
constexpr ImVec4 kDetailTint{0.48F, 0.53F, 0.61F, 1.0F};

/** The line being edited. */
std::array<char, parser::kLineCapacity> g_input{};
History g_history{};
/** Matches for the name being typed, refreshed from the buffer each frame. */
Completion g_suggestions{};
/** Which match is highlighted. It is what Tab inserts. */
std::size_t g_selected{};
/** Set while the prompt should take focus, which is the frame after the console opens. */
bool g_focusPrompt{true};
/** Set while the scrollback should jump to its newest line. */
bool g_scrollToBottom{true};
/** Visibility from the previous frame, so opening can be told from staying open. */
bool g_wasVisible{};
/** The banner is a greeting, so it is written once per process rather than once per opening. */
bool g_bannerWritten{};
/** Set when the reader asked to see everything from an empty prompt. */
bool g_browseAll{};
/** Set while the list offers values for an argument rather than names of entries. */
bool g_offeringValues{};

/** @param kind Line kind to tint. @return Its color. */
[[nodiscard]] const ImVec4& tint_for(output::LineKind kind) noexcept {
    switch (kind) {
    case output::LineKind::input:
        return kInputTint;
    case output::LineKind::failure:
        return kFailureTint;
    case output::LineKind::notice:
        return kNoticeTint;
    case output::LineKind::answer:
        break;
    }
    return kAnswerTint;
}

/** @return What is currently typed. */
[[nodiscard]] std::string_view typed_line() noexcept {
    return {g_input.data()};
}

/**
 * @return True while the reader is still typing an entry name.
 *
 * A space ends the name and starts the arguments, which is also what turns the suggestion list
 * off and hands the arrow keys back to the history.
 */
[[nodiscard]] bool naming() noexcept {
    return typed_line().find(' ') == std::string_view::npos;
}

/** @return True while a suggestion list is on screen and owns the arrow keys. */
[[nodiscard]] bool list_open() noexcept {
    return g_suggestions.count != 0;
}

/** @return Offset of the token being typed, which is one past the last space. */
[[nodiscard]] std::size_t token_start() noexcept {
    const std::string_view typed = typed_line();
    const std::size_t lastSpace = typed.find_last_of(' ');
    return lastSpace == std::string_view::npos ? 0 : lastSpace + 1;
}

/**
 * Collects the values an argument declares, narrowed by what is typed of it.
 *
 * The choices are already declared, already enforced by the reader and already printed by help.
 * Offering them here is the one place that declaration was not being spent.
 *
 * @param output Filled with the matching choices.
 * @return True when the token being typed has declared choices at all.
 */
[[nodiscard]] bool collect_values(Completion& output) noexcept {
    const std::string_view typed = typed_line();
    const std::size_t nameEnd = typed.find(' ');
    if (nameEnd == std::string_view::npos) {
        return false;
    }
    registry::Descriptor entry{};
    if (!registry::find(typed.substr(0, nameEnd), entry)) {
        return false;
    }

    std::span<const std::string_view> choices{};
    if (entry.kind == registry::Kind::variable) {
        choices = entry.choices;
    } else {
        // Spaces already passed say which argument the token belongs to.
        std::size_t index = 0;
        for (std::size_t at = nameEnd; at < typed.size(); ++at) {
            if (typed[at] != ' ' && (at == 0 || typed[at - 1] == ' ')) {
                ++index;
            }
        }
        const std::size_t current = index == 0 ? 0 : index - 1;
        if (current >= entry.arguments.size()) {
            return false;
        }
        choices = entry.arguments[current].choices;
    }
    if (choices.empty()) {
        return false;
    }

    const std::string_view partial = typed.substr(token_start());
    output = Completion{};
    for (const std::string_view choice : choices) {
        if (!contains_folded(choice, partial)) {
            continue;
        }
        if (output.count >= kCompletionCapacity) {
            output.truncated = true;
            break;
        }
        output.matches[output.count] = choice;
        ++output.count;
    }
    return output.count != 0;
}

/** @return True when a signature row will be drawn, which is what reserves its height. */
[[nodiscard]] bool signature_open() noexcept {
    const std::string_view typed = typed_line();
    const std::size_t nameEnd = typed.find(' ');
    if (nameEnd == std::string_view::npos) {
        return false;
    }
    registry::Descriptor entry{};
    return registry::find(typed.substr(0, nameEnd), entry);
}

/** Refreshes the matches for what is typed, keeping the highlight inside them. */
void refresh_suggestions() noexcept {
    const std::string_view typed = typed_line();
    // An empty line narrows nothing, so the list stays away rather than spending the scrollback's
    // rows on a wall of every name. It opens on the first letter, or when the reader asks for the
    // whole list from an empty prompt.
    const bool wanted = !typed.empty() || g_browseAll;
    g_offeringValues = false;
    if (naming()) {
        g_suggestions = wanted ? suggest(typed) : Completion{};
    } else {
        Completion values{};
        g_offeringValues = collect_values(values);
        g_suggestions = g_offeringValues ? values : Completion{};
    }
    if (g_selected >= g_suggestions.count) {
        g_selected = 0;
    }
}

/** Receives one drained result on the render thread and prints it. */
void on_result(std::uint64_t, const Result& result) noexcept {
    output::write_result(result);
    g_scrollToBottom = true;
}

/** Replaces what is typed with one line, leaving the cursor at its end. */
void replace_input(ImGuiInputTextCallbackData* data, std::string_view line) noexcept {
    data->DeleteChars(0, data->BufTextLen);
    if (!line.empty()) {
        data->InsertChars(0, line.data(), line.data() + line.size());
    }
}

/**
 * Moves the highlight, or walks the history when no list is open.
 *
 * The arrow keys do both jobs because a console needs both and has one pair of keys. Which job
 * they are doing is never ambiguous: the list is on screen or it is not.
 */
void apply_arrow(ImGuiInputTextCallbackData* data) noexcept {
    const bool up = data->EventKey == ImGuiKey_UpArrow;
    if (!list_open() && typed_line().empty() && !up) {
        // Down on an empty prompt is the shell gesture for "show me everything", and it is the
        // only way left to browse once the list stopped opening on its own.
        g_browseAll = true;
        g_selected = 0;
        return;
    }
    if (list_open()) {
        if (up) {
            g_selected = g_selected == 0 ? g_suggestions.count - 1 : g_selected - 1;
        } else {
            g_selected = g_selected + 1 >= g_suggestions.count ? 0 : g_selected + 1;
        }
        return;
    }

    std::string_view recalled{};
    const bool moved = up ? step_back(g_history, recalled) : step_forward(g_history, recalled);
    if (moved) {
        replace_input(data, recalled);
    }
}

/** Takes the highlighted name, or completes as far as every match agrees when none is listed. */
void apply_completion(ImGuiInputTextCallbackData* data) noexcept {
    if (g_offeringValues) {
        if (g_selected >= g_suggestions.count) {
            return;
        }
        // Only the token being typed is replaced, so the name and the arguments before it stay.
        // Its start is measured from the buffer the callback was handed, not from the one the
        // list was built against a frame earlier: a space typed in between would move it, and
        // deleting from the older offset would take the entry name with it.
        const std::string_view live{data->Buf, static_cast<std::size_t>(data->BufTextLen)};
        const std::size_t lastSpace = live.find_last_of(' ');
        const auto start =
            static_cast<int>(lastSpace == std::string_view::npos ? 0 : lastSpace + 1);
        const std::string_view value = g_suggestions.matches[g_selected];
        data->DeleteChars(start, data->BufTextLen - start);
        data->InsertChars(data->BufTextLen, value.data(), value.data() + value.size());
        return;
    }
    if (!naming()) {
        return;
    }
    if (g_suggestions.count != 0 && g_selected < g_suggestions.count) {
        // A name plus its space, because every entry that takes an argument needs one next.
        const std::string_view name = g_suggestions.matches[g_selected];
        replace_input(data, name);
        data->InsertChars(data->BufTextLen, " ");
        return;
    }
    const Completion prefixed = complete(typed_line());
    if (prefixed.count != 0 && prefixed.shared.size() > typed_line().size()) {
        replace_input(data, prefixed.shared);
    }
}

/** Routes one Dear ImGui input callback. */
int input_callback(ImGuiInputTextCallbackData* data) noexcept {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        apply_arrow(data);
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        apply_completion(data);
    }
    return 0;
}

/** Echoes one submitted line, reads it, and takes it for the drain. */
void submit_line(std::string_view line) noexcept {
    output::write(output::LineKind::input, line);
    remember(g_history, line);
    g_scrollToBottom = true;

    const parser::Outcome outcome = parser::parse_line(line);
    if (outcome.status != Status::ok) {
        Result rejected{};
        rejected.status = outcome.status;
        if (outcome.status == Status::unknownName && !outcome.requestedName.empty()) {
            Value named{};
            named.type = Type::text;
            store_text(outcome.requestedName, named.text, named.textLength);
            static_cast<void>(add_row(rejected, "name", named));
        }
        output::write_result(rejected);
        if (outcome.status == Status::unknownName && !outcome.requestedName.empty()) {
            // A refused name is where a reader most needs the right one.
            const Completion nearby = suggest(outcome.requestedName);
            for (std::size_t index = 0; index < nearby.count; ++index) {
                output::write(output::LineKind::notice, nearby.matches[index]);
            }
        }
        return;
    }
    if (queue::submit(outcome.invocation) == queue::kNoTicket) {
        Result refused{};
        refused.status = Status::refused;
        set_summary(refused, "Too much is already waiting to run.");
        output::write_result(refused);
    }
}

/** Draws every retained line, tinted by what it is. */
void draw_scrollback() noexcept {
    const output::Scrollback view = output::snapshot();
    for (const output::Line& line : view.lines()) {
        ImGui::PushStyleColor(ImGuiCol_Text, tint_for(line.kind));
        ImGui::TextUnformatted(line.text.data(), line.text.data() + line.length);
        ImGui::PopStyleColor();
    }
    if (g_scrollToBottom) {
        ImGui::SetScrollHereY(kScrollBottom);
        g_scrollToBottom = false;
    }
}

/** Writes one detail run after the current item, on the same line. */
void draw_detail(std::string_view text) noexcept {
    if (text.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDetailTint);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

/** Draws the type, range and, for a variable, the value it currently holds. */
void draw_entry_detail(const registry::Descriptor& entry) noexcept {
    std::array<char, output::kScrollbackLineCapacity> usage{};
    std::size_t usageLength = 0;
    output::format_usage(entry, usage, usageLength);
    // The usage repeats the name, which is already on the line, so only the tail is shown.
    const std::string_view whole{usage.data(), usageLength};
    const std::size_t tail = whole.size() > entry.name.size() ? entry.name.size() : whole.size();
    draw_detail(whole.substr(tail));

    if (entry.kind != registry::Kind::variable || entry.read == nullptr) {
        return;
    }
    Value current{};
    if (!entry.read(current)) {
        return;
    }
    current.type = entry.type;
    std::array<char, output::kFormattedValueCapacity> printed{};
    std::size_t printedLength = 0;
    output::format_value(current, printed, printedLength);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDetailTint);
    ImGui::TextUnformatted("=");
    ImGui::PopStyleColor();
    draw_detail({printed.data(), printedLength});
}

/** Draws the matches for the name being typed, with the highlighted one carrying the accent. */
void draw_suggestions() noexcept {
    const std::size_t shown =
        g_suggestions.count < kVisibleSuggestions ? g_suggestions.count : kVisibleSuggestions;
    // The window slides with the highlight, so moving past the last visible row scrolls the list
    // instead of moving a highlight nobody can see.
    const std::size_t first = g_selected < shown ? 0 : g_selected - shown + 1;
    for (std::size_t offset = 0; offset < shown; ++offset) {
        const std::size_t index = first + offset;
        if (index >= g_suggestions.count) {
            break;
        }
        const bool selected = index == g_selected;
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? kSelectedTint : kSuggestionTint);
        const std::string_view name = g_suggestions.matches[index];
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::PopStyleColor();

        registry::Descriptor entry{};
        if (!g_offeringValues && registry::find(name, entry)) {
            if (selected) {
                draw_entry_detail(entry);
                draw_detail(entry.help);
            } else {
                draw_detail(entry.help);
            }
        }
    }
    if (g_suggestions.count > shown) {
        std::array<char, 64> more{};
        const int written = std::snprintf(
            more.data(), more.size(), "%zu of %zu", g_selected + 1, g_suggestions.count);
        if (written > 0) {
            draw_detail({more.data(), static_cast<std::size_t>(written)});
        }
    }
}

/**
 * Draws the signature of the entry being given arguments, marking the one being typed.
 *
 * Once a name is settled the suggestion list has nothing left to offer, and what a reader needs
 * instead is which argument comes next and what it may hold.
 */
void draw_signature() noexcept {
    const std::string_view typed = typed_line();
    const std::size_t nameEnd = typed.find(' ');
    if (nameEnd == std::string_view::npos) {
        return;
    }
    registry::Descriptor entry{};
    if (!registry::find(typed.substr(0, nameEnd), entry)) {
        return;
    }

    // Tokens already finished say which argument is being typed now.
    std::size_t typedArguments = 0;
    for (std::size_t index = nameEnd; index < typed.size(); ++index) {
        const bool starts = typed[index] != ' ' && (index == 0 || typed[index - 1] == ' ');
        if (starts) {
            ++typedArguments;
        }
    }
    const std::size_t current = typedArguments == 0 ? 0 : typedArguments - 1;

    ImGui::PushStyleColor(ImGuiCol_Text, kDetailTint);
    ImGui::TextUnformatted(entry.name.data(), entry.name.data() + entry.name.size());
    ImGui::PopStyleColor();

    if (entry.kind == registry::Kind::variable) {
        draw_entry_detail(entry);
        return;
    }
    for (std::size_t index = 0; index < entry.arguments.size(); ++index) {
        const registry::Argument& argument = entry.arguments[index];
        std::array<char, 96> text{};
        const int written = std::snprintf(text.data(),
                                          text.size(),
                                          "<%.*s:%.*s>",
                                          static_cast<int>(argument.name.size()),
                                          argument.name.data(),
                                          static_cast<int>(type_name(argument.type).size()),
                                          type_name(argument.type).data());
        if (written <= 0) {
            continue;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, index == current ? kSelectedTint : kDetailTint);
        ImGui::TextUnformatted(text.data(), text.data() + written);
        ImGui::PopStyleColor();
    }
    if (current < entry.arguments.size()) {
        draw_detail(entry.arguments[current].help);
    }
}

/**
 * Draws the wordmark row the console opens under.
 *
 * The logo art is the one the HUD card already draws, tinted with the same gradient, so the
 * console is recognisably the same tool rather than a second look at it. It is drawn rather than
 * written, because the interface font is proportional and any lettering built out of characters
 * would set ragged.
 */
void draw_header() noexcept {
    const float extent = ui::scaling::dpi::pixels(kHeaderLogoExtent);
    if (ui::components::logo::draw(extent)) {
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, kWordmarkTint);
    ImGui::TextUnformatted(kWordmark);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%s console", SUNRISE_VER_STRING);
    ImGui::EndGroup();
    ImGui::Separator();
}

/** Writes the greeting, once, so the first screen is never blank. */
void write_banner() noexcept {
    if (g_bannerWritten) {
        return;
    }
    g_bannerWritten = true;
    const registry::RegistrySnapshot view = registry::snapshot();
    std::array<char, output::kScrollbackLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "Sunrise console. %zu commands and variables.",
                                      view.entries().size());
    if (written > 0) {
        output::write(output::LineKind::notice, {line.data(), static_cast<std::size_t>(written)});
    }
    output::write(output::LineKind::notice, kBannerHint);
}

} // namespace

/** Publishes the console's own entries and clears its editing state. */
bool initialize() noexcept {
    g_input = {};
    g_history = History{};
    g_suggestions = Completion{};
    g_selected = 0;
    g_focusPrompt = true;
    g_scrollToBottom = true;
    g_wasVisible = false;
    g_bannerWritten = false;
    g_browseAll = false;
    g_offeringValues = false;
    return builtins::initialize();
}

/** Runs whatever is waiting, then draws the console when it is showing. */
bool render(bool visible) noexcept {
    // Draining first means a line submitted last frame has already answered by the time the
    // scrollback below is read, so an answer never waits a frame to appear.
    static_cast<void>(queue::drain(&on_result));

    if (!visible) {
        g_wasVisible = false;
        return false;
    }
    if (!g_wasVisible) {
        // Opening puts the caret in the prompt, so the console is typed into rather than clicked.
        g_focusPrompt = true;
        g_scrollToBottom = true;
        g_wasVisible = true;
        // Written on the first opening rather than at boot, because the modules publish their
        // entries after Core does and the count would otherwise be short.
        write_banner();
    }
    refresh_suggestions();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size{viewport->WorkSize.x, viewport->WorkSize.y * kViewportHeightShare};
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(size);
    if (!ImGui::Begin(kWindowTitle, nullptr, kWindowFlags)) {
        ImGui::End();
        return true;
    }
    draw_header();

    // The scrollback takes what the prompt and its helper rows leave, so the rows appear by
    // taking space from the history rather than by pushing the prompt off the strip.
    // The header is already drawn, so the height left to divide excludes it. Counting it here
    // too would shorten the scrollback by exactly the header and leave that much dead space
    // under the prompt.
    const std::size_t helperRows =
        list_open() ? (g_suggestions.count < kVisibleSuggestions ? g_suggestions.count
                                                                 : kVisibleSuggestions)
                    : (signature_open() ? 1U : 0U);
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float reserved =
        ImGui::GetFrameHeightWithSpacing() + (static_cast<float>(helperRows) * rowHeight);
    if (ImGui::BeginChild(kScrollbackId, ImVec2{0.0F, -reserved}, kScrollbackFlags)) {
        draw_scrollback();
    }
    ImGui::EndChild();

    if (list_open()) {
        draw_suggestions();
    } else {
        draw_signature();
    }

    if (g_focusPrompt) {
        ImGui::SetKeyboardFocusHere();
        g_focusPrompt = false;
    }
    ImGui::SetNextItemWidth(kFullWidth);
    if (ImGui::InputText(
            kPromptLabel, g_input.data(), g_input.size(), kInputFlags, &input_callback)) {
        const std::string_view line = typed_line();
        if (!line.empty()) {
            submit_line(line);
        }
        g_input = {};
        g_selected = 0;
        g_browseAll = false;
        // Enter gives focus away, so it is taken back or the next line would need a click.
        g_focusPrompt = true;
    }

    ImGui::End();
    return true;
}

/** Removes the console's entries and clears its editing state. */
void shutdown() noexcept {
    builtins::shutdown();
    g_input = {};
    g_history = History{};
    g_suggestions = Completion{};
    g_wasVisible = false;
}

} // namespace sunrise::core::console::overlay
