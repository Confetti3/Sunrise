#include <Windows.h>

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

#include "../../runtime/storage/internal.h"
#include "internal.h"

namespace sunrise::state::activity::mission {

/** All allocations and callback work precede this guarded, allocation-free State publication. */
Status commit_reloads(std::span<const ReloadCommit> replacements) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<MissionState>);
    if (replacements.empty()) {
        return Status::ready;
    }
    if (replacements.size() > kSessionCapacity) {
        return Status::invalidBinding;
    }
    std::vector<MissionState> preparedStates;
    try {
        preparedStates.reserve(replacements.size());
        for (const auto& replacement : replacements) {
            if (replacement.expected == nullptr || replacement.replacement == nullptr
                || !valid_program(replacement.replacement->state.program)) {
                return Status::invalidProgram;
            }
            preparedStates.push_back(replacement.replacement->state);
        }
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    Status status =
        activity.stateRevision == kMaximumRevision ? Status::revisionExhausted : Status::ready;
    std::array<SessionRecord*, kSessionCapacity> records{};
    for (std::size_t index = 0; status == Status::ready && index < replacements.size(); ++index) {
        const auto& replacement = replacements[index];
        SessionRecord* const record = find_record(activity, replacement.binding);
        const Snapshot* const expected = replacement.expected;
        if (record == nullptr || expected == nullptr || replacement.replacement == nullptr
            || std::find(
                   records.begin(), records.begin() + static_cast<std::ptrdiff_t>(index), record)
                   != records.begin() + static_cast<std::ptrdiff_t>(index)) {
            status = Status::invalidBinding;
        } else if (record->recordRevision != expected->recordRevision
                   || record->mission.revision != expected->state.revision) {
            status = Status::revisionMismatch;
        } else if (!same_program(record->mission.program, expected->state.program)
                   || !record->mission.programBound) {
            status = Status::programMismatch;
        } else if (record->mission.inputSequence != expected->state.inputSequence
                   || record->mission.issuedInputSequence != expected->state.issuedInputSequence) {
            status = Status::inputSequenceMismatch;
        } else {
            records[index] = record;
        }
    }
    if (status == Status::ready) {
        ++activity.stateRevision;
        for (std::size_t index = 0; index < replacements.size(); ++index) {
            Snapshot& prepared = *replacements[index].replacement;
            records[index]->mission = std::move(preparedStates[index]);
            records[index]->recordRevision = activity.stateRevision;
            prepared.recordRevision = activity.stateRevision;
            prepared.activityStateRevision = activity.stateRevision;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

} // namespace sunrise::state::activity::mission
