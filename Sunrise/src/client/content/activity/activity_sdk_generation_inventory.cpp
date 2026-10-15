#include <Windows.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/named_tags.h"
#include "../../../state/build_data/runtime.h"
#include "../hash_names/hash_name_build.h"
#include "activity_sdk_generation_worker_internal.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {

/** Adapts the selected cancellation source to the inventory builder. */
[[nodiscard]] bool inventory_cancelled(void*) noexcept {
    return cancel_requested();
}

/** Appends one scenario only when its complete identity fits the public shard schema. */
[[nodiscard]] bool
append_scenario(std::vector<Scenario>& output, std::uint32_t tag, std::string_view name) noexcept {
    if (tag == 0 || name.empty() || name.size() >= Scenario{}.name.size()) {
        return false;
    }
    for (const Scenario& existing : output) {
        if (existing.tag == tag) {
            return true;
        }
    }
    if (output.size() >= manifest::kMaximumRecords) {
        return false;
    }
    Scenario row{};
    row.tag = tag;
    row.nameLength = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), row.name.begin());
    try {
        output.push_back(row);
        return true;
    } catch (...) {
        return false;
    }
}
/** Maps every inventory name status and refuses unknown enum values. */
[[nodiscard]] bool convert_name_status(inventory::NameStatus input,
                                       manifest::ActivityRootSelectionStatus& output) noexcept {
    switch (input) {
    case inventory::NameStatus::exact:
        output = manifest::ActivityRootSelectionStatus::exact;
        return true;
    case inventory::NameStatus::ambiguous:
        output = manifest::ActivityRootSelectionStatus::ambiguous;
        return true;
    case inventory::NameStatus::staleAliasesOnly:
        output = manifest::ActivityRootSelectionStatus::staleAliasesOnly;
        return true;
    case inventory::NameStatus::unnamed:
        output = manifest::ActivityRootSelectionStatus::unnamed;
        return true;
    }
    return false;
}

/** Maps every inventory join status and refuses unknown enum values. */
[[nodiscard]] bool convert_join_status(inventory::JoinStatus input,
                                       manifest::ActivityJoinStatus& output) noexcept {
    switch (input) {
    case inventory::JoinStatus::exact:
        output = manifest::ActivityJoinStatus::exact;
        return true;
    case inventory::JoinStatus::liveNameMissing:
        output = manifest::ActivityJoinStatus::liveNameMissing;
        return true;
    case inventory::JoinStatus::sourceNameMissing:
        output = manifest::ActivityJoinStatus::sourceNameMissing;
        return true;
    case inventory::JoinStatus::liveNameAmbiguous:
        output = manifest::ActivityJoinStatus::ambiguous;
        return true;
    }
    return false;
}

/** Maps every binding disposition and refuses unknown enum values. */
[[nodiscard]] bool convert_binding_disposition(inventory::BindingDisposition input,
                                               manifest::BindingDisposition& output) noexcept {
    switch (input) {
    case inventory::BindingDisposition::fixedScenario:
        output = manifest::BindingDisposition::fixedScenario;
        return true;
    case inventory::BindingDisposition::namedDefinitionUnavailable:
        output = manifest::BindingDisposition::namedDefinitionUnavailable;
        return true;
    case inventory::BindingDisposition::noDirectFixedActivityName:
        output = manifest::BindingDisposition::noDirectFixedActivityName;
        return true;
    case inventory::BindingDisposition::unresolvedRunnable:
        output = manifest::BindingDisposition::unresolvedRunnable;
        return true;
    }
    return false;
}

/** Maps every stable binding reason and refuses unknown enum values. */
[[nodiscard]] bool convert_binding_reason(inventory::BindingReason input,
                                          manifest::BindingReason& output) noexcept {
    switch (input) {
    case inventory::BindingReason::exactActivityRootScenarioEdge:
        output = manifest::BindingReason::exactActivityRootScenarioEdge;
        return true;
    case inventory::BindingReason::installedRouteAbsent:
        output = manifest::BindingReason::installedRouteAbsent;
        return true;
    case inventory::BindingReason::noDirectFixedActivityName:
        output = manifest::BindingReason::noDirectFixedActivityName;
        return true;
    case inventory::BindingReason::activityRootNameAmbiguous:
        output = manifest::BindingReason::activityRootNameAmbiguous;
        return true;
    case inventory::BindingReason::activityRootEdgeMissing:
        output = manifest::BindingReason::activityRootEdgeMissing;
        return true;
    }
    return false;
}

/** Maps every evidence basis and refuses unknown enum values. */
[[nodiscard]] bool convert_evidence_basis(inventory::BindingEvidenceBasis input,
                                          manifest::BindingEvidenceBasis& output) noexcept {
    switch (input) {
    case inventory::BindingEvidenceBasis::effectiveActivityRootNamePlusPayloadScenarioEdge:
        output = manifest::BindingEvidenceBasis::effectiveActivityRootNamePlusPayloadScenarioEdge;
        return true;
    case inventory::BindingEvidenceBasis::effectiveActivityAndScenarioRootNameCensus:
        output = manifest::BindingEvidenceBasis::effectiveActivityAndScenarioRootNameCensus;
        return true;
    case inventory::BindingEvidenceBasis::activityRecordInternalNameEmpty:
        output = manifest::BindingEvidenceBasis::activityRecordInternalNameEmpty;
        return true;
    case inventory::BindingEvidenceBasis::effectiveActivityRootNameCensus:
        output = manifest::BindingEvidenceBasis::effectiveActivityRootNameCensus;
        return true;
    }
    return false;
}

/** Maps every runtime interpretation and refuses unknown enum values. */
[[nodiscard]] bool convert_runnable_status(inventory::RunnableStatus input,
                                           manifest::RunnableStatus& output) noexcept {
    switch (input) {
    case inventory::RunnableStatus::fixedScenarioBound:
        output = manifest::RunnableStatus::fixedScenarioBound;
        return true;
    case inventory::RunnableStatus::unavailableInInstalledEstate:
        output = manifest::RunnableStatus::unavailableInInstalledEstate;
        return true;
    case inventory::RunnableStatus::fixedScenarioNotApplicable:
        output = manifest::RunnableStatus::fixedScenarioNotApplicable;
        return true;
    case inventory::RunnableStatus::unresolved:
        output = manifest::RunnableStatus::unresolved;
        return true;
    }
    return false;
}

/** Converts the checked native inventory into the bounded persistent catalog rows. */
[[nodiscard]] bool build_inventory(Work& work, const package_reader::Source& source) noexcept {
    inventory::Snapshot snapshot{};
    if (!inventory::build(source, &inventory_cancelled, nullptr, snapshot) || cancel_requested()
        || snapshot.scenarios.size() > manifest::kMaximumScenarioRecords
        || snapshot.activityRoots.size() > manifest::kMaximumActivityRootRecords
        || snapshot.activities.size() > manifest::kMaximumActivityVariantRecords) {
        return false;
    }
    try {
        work.scenarios.clear();
        work.activityRoots.clear();
        work.activityVariants.clear();
        work.scenarios.reserve(snapshot.scenarios.size());
        work.activityRoots.reserve(snapshot.activityRoots.size());
        work.activityVariants.reserve(snapshot.activities.size());

        for (const inventory::ScenarioRoot& sourceScenario : snapshot.scenarios) {
            if (!append_scenario(
                    work.scenarios,
                    sourceScenario.tag,
                    std::string_view(sourceScenario.name.data(), sourceScenario.nameLength))) {
                return false;
            }
        }
        for (const inventory::ActivityRoot& sourceRoot : snapshot.activityRoots) {
            manifest::ActivityRootRecord root{};
            root.activityRootTag = sourceRoot.tag;
            root.scenarioTag = sourceRoot.scenarioTag;
            root.transitionDescriptorTag = sourceRoot.transitionDescriptorTag;
            if (sourceRoot.nameLength >= root.preferredName.size()
                || !convert_name_status(sourceRoot.nameStatus, root.selectionStatus)) {
                return false;
            }
            root.preferredNameLength = static_cast<std::uint8_t>(sourceRoot.nameLength);
            std::copy_n(sourceRoot.name.begin(), sourceRoot.nameLength, root.preferredName.begin());
            work.activityRoots.push_back(root);
        }
        for (const inventory::ActivityVariant& sourceActivity : snapshot.activities) {
            manifest::ActivityVariantRecord activity{};
            activity.activityIndex = sourceActivity.definition.activityIndex;
            activity.definitionHash = sourceActivity.definition.definitionHash;
            activity.activityRootTag = sourceActivity.joinStatus == inventory::JoinStatus::exact
                                           ? sourceActivity.activityRootTag
                                           : manifest::kAbsentTag;
            activity.scenarioTag = sourceActivity.joinStatus == inventory::JoinStatus::exact
                                       ? sourceActivity.scenarioTag
                                       : manifest::kAbsentTag;
            activity.matchmakingConfigTag = sourceActivity.bindingEvidence.matchmakingConfigTag;
            if (sourceActivity.definition.internalNameLength >= activity.internalName.size()
                || !convert_join_status(sourceActivity.joinStatus, activity.joinStatus)
                || !convert_binding_disposition(sourceActivity.bindingDisposition,
                                                activity.bindingDisposition)
                || !convert_binding_reason(sourceActivity.bindingReason, activity.bindingReason)
                || !convert_evidence_basis(sourceActivity.bindingEvidenceBasis,
                                           activity.bindingEvidenceBasis)
                || !convert_runnable_status(sourceActivity.runnableStatus,
                                            activity.runnableStatus)) {
                return false;
            }
            activity.internalNameLength =
                static_cast<std::uint8_t>(sourceActivity.definition.internalNameLength);
            std::copy_n(sourceActivity.definition.internalName.begin(),
                        sourceActivity.definition.internalNameLength,
                        activity.internalName.begin());
            activity.fullSdkAcceptable = sourceActivity.fullSdkAcceptable;
            activity.hasInternalName = sourceActivity.bindingEvidence.hasInternalName;
            activity.hasMatchmakingConfig = sourceActivity.bindingEvidence.hasMatchmakingConfig;
            activity.activityRootCandidateTags =
                sourceActivity.bindingEvidence.activityRootCandidateTags;
            activity.scenarioNameCandidateTags =
                sourceActivity.bindingEvidence.scenarioNameCandidateTags;
            activity.evidenceRootTags = sourceActivity.bindingEvidence.evidenceRootTags;
            activity.locators.reserve(sourceActivity.bindingEvidence.locators.size());
            for (const inventory::PackageLocator& locator :
                 sourceActivity.bindingEvidence.locators) {
                activity.locators.push_back({locator.tag, locator.offset});
            }
            work.activityVariants.push_back(activity);
        }
        const bool converted = work.scenarios.size() == snapshot.scenarios.size()
                               && work.activityRoots.size() == snapshot.activityRoots.size()
                               && work.activityVariants.size() == snapshot.activities.size();
        if (!converted) {
            return false;
        }
        work.activityInventory = std::move(snapshot);
        return true;
    } catch (...) {
        work.scenarios.clear();
        work.activityRoots.clear();
        work.activityVariants.clear();
        return false;
    }
}

/** A native inventory may publish only when every row has a truthful terminal disposition. */
[[nodiscard]] bool binding_ready(const inventory::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept {
    return completeness.total == activityCount
           && completeness.fixedScenario + completeness.namedDefinitionUnavailable
                      + completeness.noDirectFixedActivityName + completeness.unresolvedRunnable
                  == completeness.total
           && completeness.unresolvedRunnable == 0
           && completeness.status == inventory::BindingCompletenessStatus::ready;
}

/** A persisted catalog is usable only when its recomputed partition is publishable. */
[[nodiscard]] bool binding_ready(const manifest::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept {
    return completeness.total == activityCount
           && completeness.fixedScenario + completeness.namedDefinitionUnavailable
                      + completeness.noDirectFixedActivityName + completeness.unresolvedRunnable
                  == completeness.total
           && completeness.unresolvedRunnable == 0
           && completeness.status == manifest::BindingCompletenessStatus::ready;
}

namespace {

namespace named_tags = middleware::content::packages::named_tags;
namespace hash_name_pass = ::sunrise::client::content::hash_names;

/** Keeps one named definition, refusing a name the State catalog would refuse. */
[[nodiscard]] bool collect_definition(void* opaque, const named_tags::Entry& entry) noexcept {
    auto& rows = *static_cast<std::vector<state::content::Definition>*>(opaque);
    if (entry.nameLength == 0 || entry.nameLength >= state::content::kDefinitionNameCapacity) {
        return false;
    }
    state::content::Definition row{};
    std::copy_n(entry.name.data(), entry.nameLength, row.name.data());
    row.nameLength = entry.nameLength;
    row.tag = entry.tag;
    row.classId = entry.classId;
    try {
        rows.push_back(row);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Collects the named definitions of one package source, one row per distinct mapping.
 * @param source Package directory and read mode.
 * @param rows Receives the rows; empty on failure.
 * @return True when the whole directory walk worked.
 */
[[nodiscard]] bool collect_definitions(const package_reader::Source& source,
                                       std::vector<state::content::Definition>& rows) noexcept {
    rows.clear();
    named_tags::DirectoryResult result{};
    if (!named_tags::extract_directory(
            source.directory, source.installedOnly, &collect_definition, &rows, result)) {
        rows.clear();
        return false;
    }
    const auto key = [](const state::content::Definition& row) noexcept {
        return std::tuple(row.tag, row.classId, std::string_view(row.name.data(), row.nameLength));
    };
    std::sort(rows.begin(), rows.end(), [&key](const auto& first, const auto& second) noexcept {
        return key(first) < key(second);
    });
    // One mapping appears once per patch that carries it; the State catalog keeps it once.
    const auto end =
        std::unique(rows.begin(), rows.end(), [&key](const auto& first, const auto& second) {
            return key(first) == key(second);
        });
    rows.erase(end, rows.end());
    return true;
}

} // namespace

/** Builds the layouts, named definitions and identifier names from installed packages only. */
bool build_installed_tables(Work& work, const package_reader::Source& source) noexcept {
    work.tables = {};
    bool layouts = false;
    try {
        auto scratch = std::make_unique<package_reader::Scratch>();
        work.installedLayouts =
            std::make_unique<::sunrise::client::content::scenarios::LayoutSet>();
        layouts = work.installedLayouts->build(source, *scratch);
        package_reader::close_files(*scratch);
    } catch (...) {
        layouts = false;
    }
    const bool names = collect_definitions(source, work.installedDefinitions);
    const bool hashNames = layouts
                           && hash_name_pass::build_private(
                               source, work.installedLayouts->rows(), work.installedHashNames);
    // Identifier names are only a last fallback; without them the pass still builds, but never
    // falls back to the mod-inclusive State table.
    if (!hashNames) {
        work.installedHashNames.clear();
    }
    const bool ready = layouts && names;
    if (ready) {
        work.tables = {
            work.installedLayouts.get(), &work.installedDefinitions, &work.installedHashNames};
    }
    core::log::writef(core::log::Channel::client,
                      !ready ? core::log::Level::error
                             : (hashNames ? core::log::Level::info : core::log::Level::warn),
                      "ev=activity_sdk_generation stage=installed_tables layouts=%zu names=%zu "
                      "hash_names=%zu hash_pass=%s result=%s",
                      work.installedLayouts != nullptr ? work.installedLayouts->rows().size() : 0U,
                      work.installedDefinitions.size(),
                      work.installedHashNames.size(),
                      hashNames ? "ok" : "fail",
                      ready ? "ok" : "fail");
    return ready;
}

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
