#include "console_invoke.h"

namespace sunrise::core::console::invoke {
namespace {

/** Answers a bare variable name with the value the module currently holds. */
void run_read(const registry::Descriptor& entry, Result& output) noexcept {
    Value current{};
    if (entry.read == nullptr || !entry.read(current)) {
        output.status = Status::failed;
        set_summary(output, "The value could not be read.");
        return;
    }
    current.type = entry.type;
    output.status = Status::ok;
    static_cast<void>(add_row(output, entry.name, current));
}

/**
 * Writes a checked value, then reports what the module holds afterwards.
 *
 * The read-back is deliberate: a module may clamp or ignore a write for a reason of its own, and
 * a reader who is told the new value rather than the requested one learns that at once instead of
 * on the next read.
 */
void run_write(const registry::Descriptor& entry, const Value& value, Result& output) noexcept {
    if (entry.write == nullptr) {
        // The variable reports rather than configures. Refusing names that plainly, instead of
        // accepting the line and changing nothing.
        output.status = Status::refused;
        set_summary(output, "This value only reports; it cannot be set.");
        return;
    }
    const Status written = entry.write(value);
    output.status = written;
    if (written != Status::ok) {
        if (output.summaryLength == 0) {
            set_summary(output, "The module declined the value.");
        }
        return;
    }
    run_read(entry, output);
}

} // namespace

/** Runs one checked invocation and fills its result. */
void run(const parser::Invocation& invocation, Result& output) noexcept {
    output = Result{};
    const registry::Descriptor& entry = invocation.entry;
    if (!registry::is_current(entry.name, invocation.registration)) {
        output.status = Status::refused;
        set_summary(output, "This console entry is no longer registered.");
        return;
    }

    if (entry.kind == registry::Kind::variable) {
        if (invocation.writesValue) {
            run_write(entry, invocation.arguments[0], output);
        } else {
            run_read(entry, output);
        }
        return;
    }

    if (entry.invoke == nullptr) {
        output.status = Status::failed;
        set_summary(output, "The command has no handler.");
        return;
    }
    entry.invoke({invocation.arguments.data(), invocation.argumentCount}, output);
}

} // namespace sunrise::core::console::invoke
