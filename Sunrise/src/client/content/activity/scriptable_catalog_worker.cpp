#include "scriptable_catalog_worker.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "../../../middleware/content/packages/tables/type23_placement_identifier_reader.h"
#include "../../../middleware/crypto/sha256.h"
#include "../../../state/build_data/scenarios/definition.h"
#include "../../../state/build_data/scriptables/coverage.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "scriptable_catalog_authored_placements.h"
#include "scriptable_catalog_builder.h"
#include "scriptable_catalog_container_placements.h"
#include "scriptable_catalog_embedded_placements.h"
#include "scriptable_catalog_finalize.h"
#include "scriptable_catalog_inline_names.h"
#include "scriptable_catalog_names.h"
#include "scriptable_catalog_reference_scan.h"
#include "scriptable_catalog_spatial_graph.h"
#include "scriptable_catalog_trigger_volumes.h"
#include "scriptable_catalog_type23_placement_links.h"
#include "source.h"

namespace sunrise::client::content::activity::scriptables {
namespace {

namespace catalog = state::build_data::scriptables;
namespace package_reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace sha256 = middleware::crypto::sha256;

/** Fixed package ids and bounds keep extraction inside validated layouts and owned storage. */
constexpr std::size_t kObjectCapacity = 65'536;
constexpr std::size_t kSlotCapacity = 262'144;
constexpr std::size_t kDescriptorCapacity = 262'144;
constexpr std::size_t kReferenceCapacity = 262'144;
constexpr std::size_t kTriggerVolumeInputCapacity = 262'144;

struct AnalysisSlot final {
    std::uint32_t nameHash{};
    std::uint16_t type{};
};

/** One validated descriptor plus its class-specific optional placement identifier. */
struct AnalysisDescriptor final {
    tables::SlotDescriptor descriptor{};
    std::uint64_t placementIdentifier{};
    bool placementIdentifierRead{};
};

/** One exact inline string found in a source tag used by an object analysis. */
struct AnalysisInlineName final {
    std::uint32_t hash{};
    std::string value{};
};

/** Inline strings from one first-seen source tag, kept in package encounter order. */
struct AnalysisTagEvidence final {
    std::uint32_t tag{};
    std::vector<AnalysisInlineName> names{};
};

/** Everything one scenario analysis produced, keyed and shared between build passes. */
struct Analysis final {
    std::vector<AnalysisSlot> slots{};
    std::vector<AnalysisDescriptor> descriptors{};
    std::vector<internal::RawReference> references{};
    internal::AuthoredPlacementAnalysis authored{};
    std::vector<std::uint32_t> observedConfigs{};
    std::vector<std::uint32_t> resolvedConfigs{};
    std::vector<catalog::PlacedSubblock> placedSubblocks{};
    std::vector<catalog::PlacedLeaf> placedLeaves{};
    std::vector<catalog::PlacedHop> placedHops{};
    std::vector<catalog::PlacedConfigOccurrence> placedConfigOccurrences{};
    std::vector<catalog::PlacedBareTarget> placedBareTargets{};
    std::vector<AnalysisTagEvidence> tagEvidence{};
    std::uint32_t configCount{};
    /** Authored placements whose flags and class definition let the game replicate them. */
    std::uint32_t replicatedPlacementCount{};
    bool readComplete{true};
};

using AnalysisMap = std::unordered_map<std::uint64_t, std::shared_ptr<const Analysis>>;

/** Inputs and owned scratch of one scenario build pass. */
struct BuildContext final {
    const package_reader::Source* source{};
    internal::BuilderCancelCheck cancel{};
    ScenarioSource scenarioSource{};
    std::vector<std::byte> scenario{};
    std::vector<std::byte> chain{};
    std::vector<std::byte> classBytes{};
    /** Replication bit per placed class definition, read once per tag. */
    std::unordered_map<std::uint32_t, bool> classReplication{};
    std::vector<internal::InlineName> inlineNames{};
    std::vector<internal::TriggerVolumeInput> triggerVolumeInputs{};
    AnalysisMap analyses{};
    AnalysisMap* sharedAnalyses{};
    Analysis* recordingAnalysis{};
    std::unordered_set<std::uint32_t> recordedAnalysisTags{};
    bool recordingCacheable{};
    /** Tags whose inline strings were already banked, so no blob is scanned twice. */
    std::unordered_set<std::uint32_t> scannedTags{};
    /** Total tag reads this scenario asked for, including every revisit. */
    std::size_t tagReads{};
    std::size_t analysisHits{};
    std::size_t analysisMisses{};
    std::shared_ptr<catalog::Snapshot> output{};
    tables::WalkResult walk{};
    bool failed{};
};

[[nodiscard]] bool cancelled(internal::BuilderCancelCheck check) noexcept {
    return check != nullptr && check();
}

/** @return True when this tag's inline strings have not been banked yet. */
[[nodiscard]] bool inline_names_pending(BuildContext& context, std::uint32_t tag) noexcept {
    try {
        return context.scannedTags.insert(tag).second;
    } catch (...) {
        // Scanning twice only appends rows the canonical pass drops, so a full bank is safe.
        return true;
    }
}

void set_detail(catalog::Snapshot& output, const char* detail) noexcept {
    output.detail = {};
    if (detail != nullptr) {
        (void)std::snprintf(output.detail.data(), output.detail.size(), "%s", detail);
    }
}

/** Names the extraction step that stopped this build. */
void set_step_detail(catalog::Snapshot& output, const char* step, bool wasCancelled) noexcept {
    output.detail = {};
    (void)std::snprintf(output.detail.data(),
                        output.detail.size(),
                        "%s at %s",
                        wasCancelled ? "catalog extraction cancelled"
                                     : "catalog extraction stopped",
                        step);
}

struct AnalysisInlineNameContext final {
    AnalysisTagEvidence* evidence{};
};

/** Captures one validated inline string for later scenario-local replay. */
[[nodiscard]] bool
capture_inline_name(void* opaque, std::uint32_t hash, std::span<const std::byte> bytes) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<AnalysisInlineNameContext*>(opaque);
    if (context.evidence == nullptr) {
        return false;
    }
    try {
        const char* value = reinterpret_cast<const char*>(bytes.data());
        context.evidence->names.push_back({hash, std::string(value, bytes.size())});
        return true;
    } catch (...) {
        return false;
    }
}

/** Extracts one source tag's inline strings once. */
[[nodiscard]] bool collect_tag_evidence(std::uint32_t tag,
                                        std::span<const std::byte> blob,
                                        AnalysisTagEvidence& output) noexcept {
    output = {};
    output.tag = tag;
    AnalysisInlineNameContext context{&output};
    return internal::visit_inline_names(blob, &capture_inline_name, &context);
}

/** Adds one source tag's cached evidence to the scenario-local banks. */
[[nodiscard]] bool append_tag_evidence(BuildContext& context,
                                       const AnalysisTagEvidence& evidence) noexcept {
    if (context.output == nullptr) {
        return false;
    }
    catalog::Snapshot& output = *context.output;
    constexpr std::size_t maximum = (std::numeric_limits<std::uint32_t>::max)();
    for (const AnalysisInlineName& candidate : evidence.names) {
        if (output.inlineNameCandidates.size() >= maximum || output.inlineNameBytes.size() > maximum
            || candidate.value.size() > maximum - output.inlineNameBytes.size()) {
            return false;
        }
        const std::size_t firstByte = output.inlineNameBytes.size();
        const std::size_t firstCandidate = output.inlineNameCandidates.size();
        const std::size_t firstResolver = context.inlineNames.size();
        try {
            const auto* begin = reinterpret_cast<const std::byte*>(candidate.value.data());
            output.inlineNameBytes.insert(
                output.inlineNameBytes.end(), begin, begin + candidate.value.size());
            output.inlineNameCandidates.push_back(
                {candidate.hash,
                 static_cast<std::uint32_t>(firstByte),
                 static_cast<std::uint32_t>(candidate.value.size())});
            if (candidate.value.size() < catalog::kNameCapacity) {
                context.inlineNames.push_back({candidate.hash, evidence.tag, candidate.value});
            }
        } catch (...) {
            output.inlineNameBytes.resize(firstByte);
            output.inlineNameCandidates.resize(firstCandidate);
            context.inlineNames.resize(firstResolver);
            return false;
        }
    }
    return true;
}

/** Retains raw evidence and the bounded source-owned display candidate from one blob. */
[[nodiscard]] bool offer_inline_strings(BuildContext& context,
                                        std::uint32_t sourceTag,
                                        std::span<const std::byte> blob) noexcept {
    AnalysisTagEvidence evidence{};
    return collect_tag_evidence(sourceTag, blob, evidence)
           && append_tag_evidence(context, evidence);
}

/** Records source evidence needed when this object analysis is reused. */
[[nodiscard]] const AnalysisTagEvidence* record_analysis_evidence(BuildContext& context,
                                                                  std::uint32_t tag,
                                                                  std::span<const std::byte> blob) {
    if (context.recordingAnalysis == nullptr || !context.recordingCacheable) {
        return nullptr;
    }
    try {
        if (!context.recordedAnalysisTags.insert(tag).second) {
            return nullptr;
        }
        context.recordingAnalysis->tagEvidence.emplace_back();
        AnalysisTagEvidence& evidence = context.recordingAnalysis->tagEvidence.back();
        if (!collect_tag_evidence(tag, blob, evidence)) {
            context.recordingAnalysis->tagEvidence.pop_back();
            context.recordingCacheable = false;
            return nullptr;
        }
        return &evidence;
    } catch (...) {
        context.recordingCacheable = false;
        return nullptr;
    }
}

/** Replays source evidence in the same first-read order as an uncached analysis. */
[[nodiscard]] bool replay_analysis_evidence(BuildContext& context,
                                            const Analysis& analysis) noexcept {
    for (const AnalysisTagEvidence& evidence : analysis.tagEvidence) {
        if (inline_names_pending(context, evidence.tag)
            && !append_tag_evidence(context, evidence)) {
            return false;
        }
    }
    return true;
}

/** Reads one tag and offers its inline names to the current build. */
[[nodiscard]] bool read_tag(BuildContext& context,
                            std::uint32_t tag,
                            std::vector<std::byte>& bytes,
                            std::uint32_t& classId) noexcept {
    ++context.tagReads;
    if (cancelled(context.cancel) || context.source == nullptr
        || !package_reader::read_tag(
            *context.source, (*context.scenarioSource.scratch), tag, bytes, classId)) {
        return false;
    }
    const AnalysisTagEvidence* recorded = record_analysis_evidence(context, tag, bytes);
    // The same tag yields the same strings every visit, so it is banked once.
    if (inline_names_pending(context, tag)
        && !((recorded != nullptr && append_tag_evidence(context, *recorded))
             || (recorded == nullptr && offer_inline_strings(context, tag, bytes)))) {
        context.failed = true;
        return false;
    }
    return true;
}

[[nodiscard]] bool read_exact(BuildContext& context,
                              std::uint32_t tag,
                              std::uint32_t expectedClass,
                              std::vector<std::byte>& bytes) noexcept {
    std::uint32_t classId = 0;
    return read_tag(context, tag, bytes, classId) && classId == expectedClass;
}

/** Supplies one validated nested scenario blob to the shared scenario walker. */
[[nodiscard]] bool scenario_tag_reader(void* opaque,
                                       tables::ReadSlot slot,
                                       std::uint32_t tag,
                                       std::span<const std::byte>& blob) noexcept {
    auto& context = *static_cast<BuildContext*>(opaque);
    const std::size_t index = static_cast<std::size_t>(slot);
    if (index >= context.scenarioSource.slots.size()) {
        return false;
    }
    constexpr std::array<std::uint32_t, static_cast<std::size_t>(tables::ReadSlot::count)> expected{
        tables::kSliceEntryClass, tables::kObjectRegistryClass, tables::kObjectClass};
    std::vector<std::byte>& bytes = context.scenarioSource.slots[index];
    if (!read_exact(context, tag, expected[index], bytes)) {
        return false;
    }
    blob = bytes;
    return true;
}

struct DescriptorContext final {
    Analysis* analysis{};
    const std::vector<AnalysisSlot>* slots{};
    std::span<const std::byte> config{};
};

/** Per-chain state carried through one placed-object walk. */
struct PlacedChainContext final {
    BuildContext* build{};
    Analysis* analysis{};
    std::uint32_t registryKey{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t subblockRow{};
    std::uint32_t leafRow{};
    std::uint32_t subblockOrdinal{};
    std::uint32_t leafOrdinal{};
    bool leafComplete{true};
    bool fatal{};
};

/** Retains descriptors that match the owning object's exact declared slot. */
[[nodiscard]] bool collect_descriptor(void* opaque,
                                      const tables::SlotDescriptor& descriptor) noexcept {
    auto& context = *static_cast<DescriptorContext*>(opaque);
    if (context.analysis == nullptr || context.slots == nullptr
        || descriptor.slotIndex >= context.slots->size()
        || (*context.slots)[descriptor.slotIndex].type != descriptor.slotType) {
        return true;
    }
    try {
        AnalysisDescriptor row{};
        row.descriptor = descriptor;
        row.placementIdentifierRead = tables::type23_placement_identifier(
            context.config, descriptor, row.placementIdentifier);
        context.analysis->descriptors.push_back(row);
    } catch (...) {
        return false;
    }
    return true;
}

/** Supplies one tag to the path-aware placed-chain observer. */
[[nodiscard]] bool placed_chain_reader(void* opaque,
                                       std::uint32_t tag,
                                       std::span<const std::byte>& blob,
                                       std::uint32_t& classId) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.build == nullptr || !read_tag(*context.build, tag, context.build->chain, classId)) {
        blob = {};
        return false;
    }
    blob = context.build->chain;
    return true;
}

/** Retains one unique config and rolls back its rows when parsing fails. */
[[nodiscard]] bool
collect_placed_config(void* opaque, std::uint32_t tag, std::span<const std::byte> blob) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.analysis == nullptr) {
        return false;
    }
    Analysis& analysis = *context.analysis;
    try {
        if (std::find(analysis.observedConfigs.begin(), analysis.observedConfigs.end(), tag)
            == analysis.observedConfigs.end()) {
            analysis.observedConfigs.push_back(tag);
        }
    } catch (...) {
        return false;
    }
    if (std::find(analysis.resolvedConfigs.begin(), analysis.resolvedConfigs.end(), tag)
        != analysis.resolvedConfigs.end()) {
        return true;
    }
    const std::size_t firstDescriptor = analysis.descriptors.size();
    const std::size_t firstReference = analysis.references.size();
    DescriptorContext descriptorContext{&analysis, &analysis.slots, blob};
    if (!tables::visit_slot_descriptors(
            blob, tag, context.registryKey, &collect_descriptor, &descriptorContext)) {
        analysis.descriptors.resize(firstDescriptor);
        return false;
    }
    try {
        internal::collect_typed_references(blob, tag, analysis.references);
        analysis.resolvedConfigs.push_back(tag);
        return true;
    } catch (...) {
        analysis.descriptors.resize(firstDescriptor);
        analysis.references.resize(firstReference);
        return false;
    }
}

/** Converts one retained vector index to the SDK's exact u32 row domain. */
[[nodiscard]] bool row_index(std::size_t value, std::uint32_t& output) noexcept {
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
        output = catalog::kNoRow;
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Maps the validated package-reader shape without assigning new semantics. */
[[nodiscard]] constexpr catalog::PlacedHopShape
placed_hop_shape(tables::PlacedChainShape value) noexcept {
    switch (value) {
    case tables::PlacedChainShape::config:
        return catalog::PlacedHopShape::config;
    case tables::PlacedChainShape::redirect:
        return catalog::PlacedHopShape::redirect;
    case tables::PlacedChainShape::descriptorRedirectArray:
        return catalog::PlacedHopShape::descriptorRedirectArray;
    case tables::PlacedChainShape::bareObjectList:
        return catalog::PlacedHopShape::bareObjectList;
    }
    return catalog::PlacedHopShape::config;
}

/** Retains one exact path-specific hop and any terminal config or object-list edge. */
[[nodiscard]] bool collect_placed_chain_record(void* opaque,
                                               const tables::PlacedChainRecord& source,
                                               std::span<const std::byte> blob) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.build == nullptr || context.analysis == nullptr
        || context.subblockRow >= context.analysis->placedSubblocks.size()
        || context.leafRow >= context.analysis->placedLeaves.size()
        || source.branchPathCount > catalog::kPlacedBranchPathCapacity) {
        context.fatal = true;
        return false;
    }
    Analysis& analysis = *context.analysis;
    std::uint32_t hopRow = 0;
    if (!row_index(analysis.placedHops.size(), hopRow)) {
        context.fatal = true;
        return false;
    }
    catalog::PlacedHop hop{};
    hop.subblockRow = context.subblockRow;
    hop.leafRow = context.leafRow;
    hop.subblockOrdinal = context.subblockOrdinal;
    hop.leafOrdinal = context.leafOrdinal;
    hop.declaredBubbleIndex = context.declaredBubbleIndex;
    hop.tag = source.tag;
    hop.classId = source.classId;
    hop.branchPath = source.branchPath;
    hop.childCount = source.childCount;
    hop.directTargetTag = source.directTargetTag;
    hop.branchPathCount = source.branchPathCount;
    hop.depth = source.depth;
    hop.shape = placed_hop_shape(source.shape);
    hop.complete = true;
    if (!sha256::hash(blob, hop.payloadSha256)) {
        context.fatal = true;
        return false;
    }
    try {
        analysis.placedHops.push_back(hop);
    } catch (...) {
        context.fatal = true;
        return false;
    }

    if (source.shape == tables::PlacedChainShape::config) {
        std::uint32_t occurrenceRow = 0;
        if (!row_index(analysis.placedConfigOccurrences.size(), occurrenceRow)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        catalog::PlacedConfigOccurrence occurrence{};
        occurrence.subblockRow = context.subblockRow;
        occurrence.leafRow = context.leafRow;
        occurrence.terminalHopRow = hopRow;
        occurrence.configTag = source.tag;
        occurrence.declaredBubbleIndex = context.declaredBubbleIndex;
        occurrence.branchPath = source.branchPath;
        occurrence.branchPathCount = source.branchPathCount;
        occurrence.complete = true;
        try {
            analysis.placedConfigOccurrences.push_back(occurrence);
        } catch (...) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        analysis.placedHops[hopRow].configOccurrenceRow = occurrenceRow;
        if (!collect_placed_config(&context, source.tag, blob)) {
            context.leafComplete = false;
            return false;
        }
        return true;
    }

    if (source.shape != tables::PlacedChainShape::bareObjectList) {
        return true;
    }

    std::uint32_t targetRow = 0;
    if (!row_index(analysis.placedBareTargets.size(), targetRow)) {
        analysis.placedHops.pop_back();
        context.fatal = true;
        return false;
    }
    catalog::PlacedBareTarget target{};
    target.subblockRow = context.subblockRow;
    target.leafRow = context.leafRow;
    target.sourceHopRow = hopRow;
    target.declaredBubbleIndex = context.declaredBubbleIndex;
    target.targetTag = source.directTargetTag;
    target.expectedTargetClass = tables::kAuthoredPlacementListClass;
    std::uint32_t targetClass = 0;
    if (!read_tag(*context.build, source.directTargetTag, context.build->chain, targetClass)) {
        target.status = catalog::PlacedBareTargetStatus::unreadableTarget;
        context.leafComplete = false;
        analysis.readComplete = false;
        if (context.build->failed || cancelled(context.build->cancel)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
    } else {
        target.targetClass = targetClass;
        target.targetLogicalSize = context.build->chain.size();
        if (!sha256::hash(context.build->chain, target.targetPayloadSha256)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        if (targetClass != tables::kAuthoredPlacementListClass) {
            target.status = catalog::PlacedBareTargetStatus::targetClassMismatch;
            context.leafComplete = false;
            analysis.readComplete = false;
        } else {
            target.status = catalog::PlacedBareTargetStatus::completeStructuralEdge;
            if (!internal::collect_authored_placements(analysis.authored,
                                                       context.build->chain,
                                                       source.directTargetTag,
                                                       context.declaredBubbleIndex)) {
                context.leafComplete = false;
                analysis.readComplete = false;
            }
        }
    }
    try {
        analysis.placedBareTargets.push_back(target);
    } catch (...) {
        analysis.placedHops.pop_back();
        context.fatal = true;
        return false;
    }
    analysis.placedHops[hopRow].bareTargetRow = targetRow;
    return true;
}

/** Follows every authored branch and retains every exact path-specific row. */
[[nodiscard]] bool follow_handle(BuildContext& context,
                                 Analysis& analysis,
                                 std::uint32_t handle,
                                 std::uint32_t registryKey,
                                 std::int32_t declaredBubbleIndex,
                                 std::uint32_t subblockRow,
                                 std::uint32_t leafRow,
                                 std::uint32_t subblockOrdinal,
                                 std::uint32_t leafOrdinal) noexcept {
    if (leafRow >= analysis.placedLeaves.size()) {
        return false;
    }
    PlacedChainContext walkContext{&context,
                                   &analysis,
                                   registryKey,
                                   declaredBubbleIndex,
                                   subblockRow,
                                   leafRow,
                                   subblockOrdinal,
                                   leafOrdinal};
    tables::PlacedChainObservation observation{};
    const bool walked = tables::visit_placed_chain_records(handle,
                                                           &placed_chain_reader,
                                                           &walkContext,
                                                           &collect_placed_chain_record,
                                                           &walkContext,
                                                           observation);
    catalog::PlacedLeaf& leaf = analysis.placedLeaves[leafRow];
    const std::size_t hopCount = analysis.placedHops.size() - leaf.firstHop;
    const std::size_t configCount =
        analysis.placedConfigOccurrences.size() - leaf.firstConfigOccurrence;
    const std::size_t bareCount = analysis.placedBareTargets.size() - leaf.firstBareTarget;
    if (!row_index(hopCount, leaf.hopCount) || !row_index(configCount, leaf.configOccurrenceCount)
        || !row_index(bareCount, leaf.bareTargetCount) || walkContext.fatal) {
        return false;
    }
    leaf.complete = walked && walkContext.leafComplete && observation.hopCount == leaf.hopCount
                    && observation.bareTargetCount == leaf.bareTargetCount;
    if (!leaf.complete) {
        analysis.readComplete = false;
    }
    return true;
}

/** @return True when the placed class definition marks its objects as network replicated. */
[[nodiscard]] bool class_replicates(BuildContext& context, std::uint32_t classTag) noexcept {
    const auto cached = context.classReplication.find(classTag);
    if (cached != context.classReplication.end()) {
        return cached->second;
    }
    std::uint32_t classId = 0;
    tables::PlacedClassDefinition definition{};
    const bool replicated = read_tag(context, classTag, context.classBytes, classId)
                            && classId == tables::kPlacedClassDefinitionClass
                            && tables::placed_class_definition(context.classBytes, definition)
                            && definition.networkReplicated;
    try {
        context.classReplication.emplace(classTag, replicated);
    } catch (...) {
        // A missed cache entry costs one more read, nothing else.
    }
    return replicated;
}

/** Counts the authored placements the game replicates: entry flag bit 0 clear, class bit set. */
[[nodiscard]] std::uint32_t count_replicated_placements(BuildContext& context,
                                                        const Analysis& analysis) noexcept {
    std::uint32_t count = 0;
    for (const internal::RawAuthoredPlacement& placement : analysis.authored.placements) {
        if ((placement.placementFlagsRaw & tables::kAuthoredPlacementNoReplicationBit) == 0
            && class_replicates(context, placement.classListTag)) {
            ++count;
        }
    }
    return count;
}

/** Reads one object layout and its reachable descriptor/config records. */
[[nodiscard]] bool analyze_object(BuildContext& context,
                                  const tables::Placement& placement,
                                  Analysis& output) noexcept {
    output = {};
    tables::Array slots{};
    if (!tables::object_slots(placement.objectBytes, slots) || slots.count > kSlotCapacity) {
        return false;
    }
    try {
        output.slots.reserve(static_cast<std::size_t>(slots.count));
        for (std::uint64_t index = 0; index < slots.count; ++index) {
            tables::Slot slot{};
            if (!tables::object_slot_at(placement.objectBytes, slots, index, slot) || slot.type == 0
                || slot.type > state::build_data::scenarios::kMaximumSlotType) {
                return false;
            }
            output.slots.push_back({slot.nameHash, static_cast<std::uint16_t>(slot.type)});
        }
    } catch (...) {
        return false;
    }

    tables::Array bubbles{};
    if (!tables::object_bubbles(placement.objectBytes, bubbles)
        || bubbles.count > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    try {
        output.placedSubblocks.reserve(static_cast<std::size_t>(bubbles.count));
    } catch (...) {
        return false;
    }
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
        tables::ObjectBubble bubble{};
        std::uint32_t subblockRow = 0;
        std::uint32_t firstLeaf = 0;
        if (!tables::object_bubble_at(placement.objectBytes, bubbles, bubbleIndex, bubble)
            || bubbleIndex > (std::numeric_limits<std::uint32_t>::max)()
            || !row_index(output.placedSubblocks.size(), subblockRow)
            || !row_index(output.placedLeaves.size(), firstLeaf)) {
            return false;
        }
        catalog::PlacedSubblock subblock{};
        subblock.subblockOrdinal = static_cast<std::uint32_t>(bubbleIndex);
        subblock.declaredBubbleIndex = bubble.bubbleIndex;
        subblock.firstLeaf = firstLeaf;
        subblock.sourceOffset = bubble.sourceOffset;
        try {
            output.placedSubblocks.push_back(subblock);
            if (bubble.handleCount
                > (std::numeric_limits<std::uint32_t>::max)() - output.placedLeaves.size()) {
                return false;
            }
            output.placedLeaves.reserve(output.placedLeaves.size()
                                        + static_cast<std::size_t>(bubble.handleCount));
        } catch (...) {
            return false;
        }
        bool subblockComplete = true;
        for (std::uint64_t leafOrdinal = 0; leafOrdinal < bubble.handleCount; ++leafOrdinal) {
            std::uint32_t handle = 0;
            std::uint32_t leafRow = 0;
            catalog::PlacedLeaf leaf{};
            if (!tables::object_placed_handle_at(placement.objectBytes, bubble, leafOrdinal, handle)
                || leafOrdinal > (std::numeric_limits<std::uint32_t>::max)()
                || !row_index(output.placedLeaves.size(), leafRow)
                || !row_index(output.placedHops.size(), leaf.firstHop)
                || !row_index(output.placedConfigOccurrences.size(), leaf.firstConfigOccurrence)
                || !row_index(output.placedBareTargets.size(), leaf.firstBareTarget)) {
                return false;
            }
            leaf.subblockRow = subblockRow;
            leaf.subblockOrdinal = static_cast<std::uint32_t>(bubbleIndex);
            leaf.leafOrdinal = static_cast<std::uint32_t>(leafOrdinal);
            leaf.declaredBubbleIndex = bubble.bubbleIndex;
            leaf.rootTag = handle;
            leaf.sourceOffset = static_cast<std::uint64_t>(bubble.handleDataOffset)
                                + leafOrdinal * tables::kObjectPlacedHandleStride;
            try {
                output.placedLeaves.push_back(leaf);
            } catch (...) {
                return false;
            }
            if (!follow_handle(context,
                               output,
                               handle,
                               placement.objectKey,
                               bubble.bubbleIndex,
                               subblockRow,
                               leafRow,
                               static_cast<std::uint32_t>(bubbleIndex),
                               static_cast<std::uint32_t>(leafOrdinal))) {
                return false;
            }
            subblockComplete = subblockComplete && output.placedLeaves[leafRow].complete;
        }
        const std::size_t leafCount = output.placedLeaves.size() - firstLeaf;
        if (!row_index(leafCount, output.placedSubblocks[subblockRow].leafCount)) {
            return false;
        }
        output.placedSubblocks[subblockRow].complete = subblockComplete;
    }
    if (!row_index(output.observedConfigs.size(), output.configCount)) {
        return false;
    }
    output.replicatedPlacementCount = count_replicated_placements(context, output);
    std::sort(output.descriptors.begin(),
              output.descriptors.end(),
              [](const AnalysisDescriptor& leftRow, const AnalysisDescriptor& rightRow) noexcept {
                  const tables::SlotDescriptor& left = leftRow.descriptor;
                  const tables::SlotDescriptor& right = rightRow.descriptor;
                  if (left.slotIndex != right.slotIndex) {
                      return left.slotIndex < right.slotIndex;
                  }
                  if (left.componentClass != right.componentClass) {
                      return left.componentClass < right.componentClass;
                  }
                  if (left.senseSchema != right.senseSchema) {
                      return left.senseSchema < right.senseSchema;
                  }
                  if (left.authSchema != right.authSchema) {
                      return left.authSchema < right.authSchema;
                  }
                  if (left.configTag != right.configTag) {
                      return left.configTag < right.configTag;
                  }
                  return left.descriptorOffset < right.descriptorOffset;
              });
    return true;
}

/** Counts descriptors from one config and returns their shared slot index. */
[[nodiscard]] std::size_t descriptors_for_config(const Analysis& analysis,
                                                 std::uint32_t configTag,
                                                 std::uint16_t& slotIndex) noexcept {
    std::size_t count = 0;
    slotIndex = 0;
    for (const AnalysisDescriptor& row : analysis.descriptors) {
        const tables::SlotDescriptor& descriptor = row.descriptor;
        if (descriptor.configTag == configTag) {
            slotIndex = descriptor.slotIndex;
            ++count;
        }
    }
    return count;
}

/** @return True when one local row bank fits after an existing u32-indexed bank. */
[[nodiscard]] bool can_append_rows(std::size_t existing, std::size_t incoming) noexcept {
    constexpr std::size_t maximum = (std::numeric_limits<std::uint32_t>::max)();
    return existing <= maximum && incoming <= maximum - existing;
}

/** Rebases one present local row while preserving the absent sentinel. */
[[nodiscard]] bool
rebase_row(std::uint32_t local, std::uint32_t first, std::uint32_t& output) noexcept {
    if (local == catalog::kNoRow) {
        output = catalog::kNoRow;
        return true;
    }
    if (local > (std::numeric_limits<std::uint32_t>::max)() - first) {
        output = catalog::kNoRow;
        return false;
    }
    output = first + local;
    return true;
}

/** Appends one exact scenario placement and its analyzed rows to the snapshot. */
[[nodiscard]] bool materialize_placement(BuildContext& context,
                                         const tables::Placement& placement) {
    const std::uint64_t key = internal::analysis_key(placement.objectTag, placement.objectKey);
    auto found = context.analyses.find(key);
    if (found == context.analyses.end()) {
        std::shared_ptr<const Analysis> selected{};
        if (context.sharedAnalyses != nullptr) {
            const auto shared = context.sharedAnalyses->find(key);
            if (shared != context.sharedAnalyses->end()) {
                selected = shared->second;
                ++context.analysisHits;
                if (!replay_analysis_evidence(context, *selected)) {
                    return false;
                }
            }
        }
        if (selected == nullptr) {
            ++context.analysisMisses;
            auto built = std::make_shared<Analysis>();
            context.recordedAnalysisTags.clear();
            context.recordingAnalysis = context.sharedAnalyses != nullptr ? built.get() : nullptr;
            context.recordingCacheable = context.recordingAnalysis != nullptr;
            const bool analyzed = analyze_object(context, placement, *built);
            context.recordingAnalysis = nullptr;
            if (!analyzed) {
                return false;
            }
            selected = built;
            if (context.sharedAnalyses != nullptr && context.recordingCacheable
                && built->readComplete) {
                try {
                    const auto shared = context.sharedAnalyses->emplace(key, selected).first;
                    selected = shared->second;
                } catch (...) {
                    // The scenario stays valid when the optional pass cache cannot grow.
                }
            }
        }
        found = context.analyses.emplace(key, std::move(selected)).first;
    }
    const Analysis& analysis = *found->second;
    catalog::Snapshot& output = *context.output;
    if (placement.bubbleIndex >= output.bubbles.size()) {
        return false;
    }
    const std::uint32_t stateRow = output.bubbles[placement.bubbleIndex].firstState
                                   + static_cast<std::uint32_t>(placement.stateIndex);
    if (stateRow >= output.states.size()) {
        return false;
    }
    if (output.objects.size() >= kObjectCapacity
        || analysis.slots.size() > kSlotCapacity - output.slots.size()) {
        return false;
    }
    const std::uint32_t objectRow = static_cast<std::uint32_t>(output.objects.size());
    const std::uint32_t firstSlot = static_cast<std::uint32_t>(output.slots.size());
    catalog::Object object{};
    object.bubbleRow = static_cast<std::uint32_t>(placement.bubbleIndex);
    object.stateRow = stateRow;
    object.registryTag = placement.registryTag;
    object.objectTag = placement.objectTag;
    object.registryKey = placement.objectKey;
    object.firstSlot = firstSlot;
    object.slotCount = static_cast<std::uint32_t>(analysis.slots.size());
    object.configCount = analysis.configCount;
    object.placedSubblockCount = static_cast<std::uint32_t>(analysis.placedSubblocks.size());
    object.placedLeafCount = static_cast<std::uint32_t>(analysis.placedLeaves.size());
    object.placedHopCount = static_cast<std::uint32_t>(analysis.placedHops.size());
    object.bareTargetCount = static_cast<std::uint32_t>(analysis.placedBareTargets.size());
    object.replicatedPlacementCount = analysis.replicatedPlacementCount;
    object.placedConfigOccurrenceCount =
        static_cast<std::uint32_t>(analysis.placedConfigOccurrences.size());
    object.objectIndex = static_cast<std::uint32_t>(placement.objectIndex);
    object.registryDescriptor = static_cast<std::uint16_t>(placement.registryDescriptor);
    object.complete = analysis.readComplete;
    output.objects.push_back(object);

    for (std::size_t slotIndex = 0; slotIndex < analysis.slots.size(); ++slotIndex) {
        const AnalysisSlot& source = analysis.slots[slotIndex];
        catalog::Slot slot{};
        slot.objectRow = objectRow;
        slot.nameHash = source.nameHash;
        slot.slotIndex = static_cast<std::uint16_t>(slotIndex);
        slot.slotType = source.type;
        slot.firstDescriptor = static_cast<std::uint32_t>(output.descriptors.size());
        for (const AnalysisDescriptor& analyzed : analysis.descriptors) {
            const tables::SlotDescriptor& descriptor = analyzed.descriptor;
            if (descriptor.slotIndex != slotIndex) {
                continue;
            }
            if (output.descriptors.size() >= kDescriptorCapacity) {
                return false;
            }
            catalog::Descriptor row{};
            row.slotRow = static_cast<std::uint32_t>(output.slots.size());
            row.configTag = descriptor.configTag;
            row.componentClass = descriptor.componentClass;
            row.senseSchema = descriptor.senseSchema;
            row.authSchema = descriptor.authSchema;
            row.descriptorOffset = descriptor.descriptorOffset;
            row.bubbleIndex = descriptor.bubbleIndex;
            row.placementIdentifier = analyzed.placementIdentifier;
            row.placementIdentifierRead = analyzed.placementIdentifierRead;
            output.descriptors.push_back(row);
            ++slot.descriptorCount;
        }
        output.slots.push_back(slot);
    }

    if (analysis.resolvedConfigs.size()
        > kTriggerVolumeInputCapacity - context.triggerVolumeInputs.size()) {
        return false;
    }
    for (const std::uint32_t configTag : analysis.resolvedConfigs) {
        context.triggerVolumeInputs.push_back({objectRow, configTag});
    }

    for (const internal::RawReference& source : analysis.references) {
        if (output.references.size() >= kReferenceCapacity) {
            return false;
        }
        catalog::TypedReference row{};
        row.sourceObjectRow = objectRow;
        row.sourceConfigTag = source.configTag;
        row.sourceOffset = source.offset;
        row.targetKey = source.targetKey;
        row.targetSlotType = source.targetType;
        row.targetSlotIndex = source.targetIndex;
        std::uint16_t sourceIndex = 0;
        if (descriptors_for_config(analysis, source.configTag, sourceIndex) == 1
            && sourceIndex < analysis.slots.size()) {
            row.sourceSlotRow = firstSlot + sourceIndex;
        }
        output.references.push_back(row);
    }

    return internal::append_authored_placements(analysis.authored, objectRow, output);
}

/** Stops the scenario walk on cancellation or a failed placement append. */
[[nodiscard]] bool placement_visitor(void* opaque, const tables::Placement& placement) noexcept {
    auto& context = *static_cast<BuildContext*>(opaque);
    if (cancelled(context.cancel)) {
        return false;
    }
    try {
        return materialize_placement(context, placement);
    } catch (...) {
        context.failed = true;
        return false;
    }
}

/** Publishes the scenario bubble and state skeleton before placement rows. */
[[nodiscard]] bool build_structure(BuildContext& context) noexcept {
    catalog::Snapshot& output = *context.output;
    tables::Array bubbles{};
    if (!tables::scenario_bubbles(context.scenario, bubbles)
        || bubbles.count > state::build_data::scenarios::kBubbleCapacity) {
        return false;
    }
    try {
        output.bubbles.reserve(static_cast<std::size_t>(bubbles.count));
        for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
            tables::Bubble source{};
            if (!tables::bubble_at(context.scenario, bubbles, bubbleIndex, source)) {
                return false;
            }
            catalog::Bubble bubble{};
            bubble.nameHash = source.nameHash;
            bubble.index = static_cast<std::uint32_t>(bubbleIndex);
            bubble.firstState = static_cast<std::uint32_t>(output.states.size());
            bubble.stateCount = static_cast<std::uint32_t>(source.stateCount);
            for (std::uint64_t stateIndex = 0; stateIndex < source.stateCount; ++stateIndex) {
                tables::SliceState stateSource{};
                if (!tables::slice_state_at(context.scenario, source, stateIndex, stateSource)) {
                    return false;
                }
                if (stateIndex == 0) {
                    bubble.isPublic = stateSource.isPublic;
                }
                catalog::State state{};
                state.stateHash = stateSource.stateHash;
                state.rawU32At12 = stateSource.rawU32At12;
                state.entryTag = stateSource.entryTag;
                state.mapBubbleIndex = stateSource.mapBubbleIndex;
                state.bubbleRow = static_cast<std::uint32_t>(bubbleIndex);
                state.index = static_cast<std::uint32_t>(stateIndex);
                state.enabled = stateSource.enabled;
                std::uint32_t classId = 0;
                std::vector<std::byte>& entry =
                    context.scenarioSource.slots[static_cast<std::size_t>(tables::ReadSlot::entry)];
                std::vector<std::byte>& registry =
                    context.scenarioSource
                        .slots[static_cast<std::size_t>(tables::ReadSlot::registry)];
                tables::SliceEntry parsed{};
                if (read_tag(context, state.entryTag, entry, classId)
                    && classId == tables::kSliceEntryClass && tables::slice_entry(entry, parsed)
                    && read_tag(context, parsed.registryTag, registry, classId)
                    && classId == tables::kObjectRegistryClass) {
                    state.registryTag = parsed.registryTag;
                    state.sliceSetIndex = parsed.index * tables::kSliceSetIndexFactor;
                    state.resolved = true;
                }
                output.states.push_back(state);
            }
            output.bubbles.push_back(bubble);
        }
    } catch (...) {
        return false;
    }
    return true;
}

/** Reports what one scenario build cost, so the hot path is measured instead of guessed. */
void report_scenario_cost(BuildContext& context,
                          const catalog::Snapshot& output,
                          std::uint32_t scenarioTag,
                          ULONGLONG startedTick) noexcept {
    std::array<char, 384> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=sdk_scenario_cost scenario=0x%08X ms=%llu reads=%zu distinct=%zu objects=%zu "
        "descriptors=%zu names=%zu analysis_hits=%zu analysis_misses=%zu block_hits=%llu "
        "block_misses=%llu block_mib=%llu",
        static_cast<unsigned>(scenarioTag),
        static_cast<unsigned long long>(GetTickCount64() - startedTick),
        context.tagReads,
        context.scannedTags.size(),
        output.objects.size(),
        output.descriptors.size(),
        output.inlineNameCandidates.size(),
        context.analysisHits,
        context.analysisMisses,
        static_cast<unsigned long long>((*context.scenarioSource.scratch).blockHits),
        static_cast<unsigned long long>((*context.scenarioSource.scratch).blockMisses),
        static_cast<unsigned long long>((*context.scenarioSource.scratch).blockBytes
                                        / (1024U * 1024U)));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Implements the reusable selected-scenario package build without publishing worker state. */
[[nodiscard]] std::shared_ptr<catalog::Snapshot>
build_scenario_catalog_impl(const package_reader::Source& source,
                            const internal::ContainerIndex& containers,
                            package_reader::Scratch& scratch,
                            AnalysisMap* sharedAnalyses,
                            internal::ContainerPlacementCache* sharedPlacements,
                            internal::StaticSpatialCache* sharedSpatial,
                            std::uint32_t scenarioTag,
                            std::string_view scenarioName,
                            internal::BuilderCancelCheck cancel) {
    const ULONGLONG startedTick = GetTickCount64();
    auto output = std::make_shared<catalog::Snapshot>();
    output->scenarioTag = scenarioTag;
    if (scenarioTag == 0 || scenarioName.empty()
        || scenarioName.size() >= output->scenarioName.size()) {
        set_detail(*output, "scenario identity is outside catalog bounds");
        output->status = catalog::BuildStatus::failed;
        return output;
    }
    std::copy(scenarioName.begin(), scenarioName.end(), output->scenarioName.begin());
    output->scenarioNameLength = static_cast<std::uint8_t>(scenarioName.size());
    output->status = catalog::BuildStatus::building;
    auto context = std::make_unique<BuildContext>();
    context->source = &source;
    context->cancel = cancel;
    context->scenarioSource.source = &source;
    context->scenarioSource.scratch = &scratch;
    context->sharedAnalyses = sharedAnalyses;
    (*context->scenarioSource.scratch).blockHits = 0;
    (*context->scenarioSource.scratch).blockMisses = 0;
    (*context->scenarioSource.scratch).blockBytes = 0;
    context->output = output;
    context->analyses.reserve(4096);
    output->objects.reserve(4096);
    output->slots.reserve(16'384);
    output->descriptors.reserve(16'384);
    output->references.reserve(4096);
    output->authoredPlacements.reserve(4096);
    if (!read_exact(*context, scenarioTag, tables::kScenarioClass, context->scenario)) {
        set_detail(*output, "scenario tag did not read with the expected class");
        output->status = catalog::BuildStatus::failed;
    } else if (!build_structure(*context)) {
        set_detail(*output, "scenario bubble/state structure did not validate");
        output->status = catalog::BuildStatus::failed;
    } else if (!tables::walk_scenario(context->scenario,
                                      &scenario_tag_reader,
                                      context.get(),
                                      &placement_visitor,
                                      context.get(),
                                      context->walk)
               || context->failed) {
        set_detail(*output, "scenario registry/object walk did not complete");
        output->status = catalog::BuildStatus::failed;
    } else {
        output->unresolvedReads = context->walk.unresolved;
        // Each step carries its own name into the detail text so a failure names its step.
        const char* step = nullptr;
        if (!internal::join_references(*output, cancel)) {
            step = "join_references";
        } else if (!internal::classify_presence(*output, cancel)) {
            step = "classify_presence";
        } else {
            if (!internal::append_trigger_volumes(source,
                                                  (*context->scenarioSource.scratch),
                                                  context->triggerVolumeInputs,
                                                  *output,
                                                  cancel)) {
                step = "append_trigger_volumes";
            } else if (!(sharedPlacements != nullptr ? internal::append_container_placements(
                                                           source,
                                                           (*context->scenarioSource.scratch),
                                                           *sharedPlacements,
                                                           containers,
                                                           scenarioName,
                                                           *output,
                                                           cancel)
                                                     : internal::append_container_placements(
                                                           source,
                                                           (*context->scenarioSource.scratch),
                                                           containers,
                                                           scenarioName,
                                                           *output,
                                                           cancel))) {
                step = "append_container_placements";
            } else if (!internal::append_embedded_placements(
                           source, (*context->scenarioSource.scratch), *output, cancel)) {
                step = "append_embedded_placements";
            } else if (!internal::append_type23_placement_links(*output, cancel)) {
                step = "append_type23_placement_links";
            } else {
                const bool spatialComplete = sharedSpatial != nullptr
                                                 ? internal::append_static_spatial_candidates(
                                                       source,
                                                       (*context->scenarioSource.scratch),
                                                       *sharedSpatial,
                                                       containers,
                                                       scenarioName,
                                                       *output,
                                                       cancel)
                                                 : internal::append_static_spatial_candidates(
                                                       source,
                                                       (*context->scenarioSource.scratch),
                                                       containers,
                                                       scenarioName,
                                                       *output,
                                                       cancel);
                if (!spatialComplete) {
                    step = "append_static_spatial_candidates";
                }
            }
        }
        if (step != nullptr) {
            set_step_detail(*output, step, cancelled(cancel));
            output->status = catalog::BuildStatus::failed;
        } else if (!internal::canonicalize_inline_name_evidence(*output)
                   || !internal::resolve_names(*output, context->inlineNames, cancel)) {
            set_detail(*output, "package name candidates did not fit");
            output->status = catalog::BuildStatus::failed;
        } else {
            catalog::refresh_coverage_diagnostics(*output);
            output->coverage = catalog::BuildCoverage::full;
            set_detail(*output, "package-derived read-only catalog");
            output->status = catalog::BuildStatus::ready;
        }
    }
    report_scenario_cost(*context, *output, scenarioTag, startedTick);
    return output;
}

} // namespace

namespace internal {

/** Owns pass-local immutable analyses without exposing their source-only layout. */
struct ScenarioAnalysisCache::Impl final {
    Impl() {
        analyses.reserve(8'192);
    }

    AnalysisMap analyses{};
    ContainerPlacementCache containerPlacements{};
    StaticSpatialCache staticSpatial{};
};

/** Allocates the optional pass cache without making extraction depend on it. */
ScenarioAnalysisCache::ScenarioAnalysisCache() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_.reset();
    }
}

ScenarioAnalysisCache::~ScenarioAnalysisCache() = default;

[[nodiscard]] std::shared_ptr<state::build_data::scriptables::Snapshot>
build_scenario_catalog(const middleware::content::packages::reader::Source& source,
                       const ContainerIndex& containers,
                       middleware::content::packages::reader::Scratch& scratch,
                       std::uint32_t scenarioTag,
                       std::string_view scenarioName,
                       BuilderCancelCheck cancel) {
    return build_scenario_catalog_impl(
        source, containers, scratch, nullptr, nullptr, nullptr, scenarioTag, scenarioName, cancel);
}

[[nodiscard]] std::shared_ptr<state::build_data::scriptables::Snapshot>
build_scenario_catalog(const middleware::content::packages::reader::Source& source,
                       const ContainerIndex& containers,
                       middleware::content::packages::reader::Scratch& scratch,
                       ScenarioAnalysisCache& analyses,
                       std::uint32_t scenarioTag,
                       std::string_view scenarioName,
                       BuilderCancelCheck cancel) {
    AnalysisMap* shared = analyses.impl_ != nullptr ? &analyses.impl_->analyses : nullptr;
    ContainerPlacementCache* placements =
        analyses.impl_ != nullptr ? &analyses.impl_->containerPlacements : nullptr;
    StaticSpatialCache* spatial =
        analyses.impl_ != nullptr ? &analyses.impl_->staticSpatial : nullptr;
    return build_scenario_catalog_impl(source,
                                       containers,
                                       scratch,
                                       shared,
                                       placements,
                                       spatial,
                                       scenarioTag,
                                       scenarioName,
                                       cancel);
}

} // namespace internal

} // namespace sunrise::client::content::activity::scriptables
