#include "activity_sdk_policy_inventory.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "activity_sdk_policy_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::policy_inventory {
namespace {

using internal::byte_less;
using internal::can_add;
using internal::FailureReasons;
using internal::make_range;
using internal::StringEqual;
using internal::StringHash;

/** The local pool cannot outgrow all bounded policy input and generated strings. */
constexpr std::size_t kMaximumDeferredStringCount = 2000000;

/** Reserve estimates cover aliases, identities, schemas, and a small fixed policy vocabulary. */
constexpr std::size_t kActivityDeferredStringEstimate = 4;
constexpr std::size_t kSlotDeferredStringEstimate = 5;
constexpr std::size_t kFixedDeferredStringEstimate = 64;

/** Slot type 31 contributes the currently unimplemented trigger adapter operation. */
constexpr std::uint32_t kTriggerSlotType = 31;

/** These stable strings are scalar values in canonical format-v12 policy rows. */
constexpr std::string_view kExact = "exact";
constexpr std::string_view kPassed = "passed";
constexpr std::string_view kFailed = "failed";
constexpr std::string_view kUnresolved = "unresolved";
constexpr std::string_view kInspect = "inspect";
constexpr std::string_view kPanelTest = "panel_test";
constexpr std::string_view kScript = "script";
constexpr std::string_view kCandidate = "candidate";
constexpr std::string_view kRefused = "refused";

/**
 * The compiled host inventory contains three implemented and two refused surfaces.
 * `slot.apply_auth` is a mission ABI surface, not a host API subject, so it is not a row here.
 */
constexpr std::array kHostSurfaces{
    HostSurface{"host-api/squad.place", "squad.place"},
    HostSurface{"host-api/scene.activate", "scene.activate"},
    HostSurface{"host-api/slot.set_channel", "slot.set_channel"},
    HostSurface{"host-api/sleep_until", "sleep_until"},
    HostSurface{"host-api/mission.complete", "mission.complete"},
};
static_assert(kHostSurfaces.size() <= kMaximumHostSurfaceCount);

struct AliasModel final {
    std::uint32_t slotIndex{};
    std::string_view value{};
};

struct HostModel final {
    const HostSurface* surface{};
    std::uint32_t subjectIndex{};
};

/** Owns one pending policy projection and its non-final string pool. */
class Builder final {
public:
    Builder(const Inputs& inputs, std::span<const internal::RouteEvidence> routes)
        : inputs_(inputs), routes_(routes) {
        output_.strings.emplace_back();
        stringIndexes_.emplace(std::string{}, 0U);
    }

    /** Builds every policy section in canonical ownership order. */
    [[nodiscard]] bool run() {
        reserve();
        return build_aliases() && build_activity_capabilities() && build_slot_capabilities()
               && build_host_capabilities();
    }

    [[nodiscard]] Snapshot take() noexcept {
        return std::move(output_);
    }

private:
    /** Reserves input-derived estimates without imposing one captured estate size. */
    void reserve() {
        output_.texts.reserve((std::min)(kMaximumPolicyTextCount,
                                         2U * inputs_.activities.size() + 2U * inputs_.slots.size()
                                             + inputs_.slotAliases.size()));
        output_.activityAliases.resize(inputs_.activities.size());
        output_.activityCapabilities.resize(inputs_.activities.size());
        output_.slotAliases.resize(inputs_.slots.size());
        output_.slotCapabilities.resize(inputs_.slots.size());
        const std::size_t expectedStrings =
            (std::min)(kMaximumDeferredStringCount,
                       inputs_.activities.size() * kActivityDeferredStringEstimate
                           + inputs_.slots.size() * kSlotDeferredStringEstimate
                           + kFixedDeferredStringEstimate);
        output_.strings.reserve(expectedStrings);
        stringIndexes_.reserve(expectedStrings);
    }

    /**
     * Interns one borrowed value into the snapshot-owned deferred string pool.
     * @param value The optional UTF-8 value to retain.
     * @param output Receives the stable local string handle.
     * @return True when the value is valid and fits within the policy bound.
     */
    [[nodiscard]] bool intern(std::string_view value, Text& output) {
        output = {};
        if (value.empty()) {
            return true;
        }
        const auto found = stringIndexes_.find(value);
        if (found != stringIndexes_.end()) {
            output.stringIndex = found->second;
            return true;
        }
        if (!internal::valid_text(value) || output_.strings.size() >= kMaximumDeferredStringCount
            || output_.strings.size() >= format::kAbsentIndex) {
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(output_.strings.size());
        std::string owned(value);
        output_.strings.push_back(owned);
        stringIndexes_.emplace(std::move(owned), index);
        output.stringIndex = index;
        return true;
    }

    /**
     * Appends one nonempty canonical Text row.
     * @param value The UTF-8 row value.
     * @param kind The format-v12 text classification.
     * @return True when the value is retained within the Text row bound.
     */
    [[nodiscard]] bool add_text(std::string_view value, format::TextKind kind) {
        if (value.empty() || !can_add(output_.texts.size(), 1, kMaximumPolicyTextCount)) {
            return false;
        }
        TextRow row{};
        row.kind = static_cast<std::uint32_t>(kind);
        if (!intern(value, row.value)) {
            return false;
        }
        output_.texts.push_back(row);
        return true;
    }

    /** @return True after activity and sorted slot aliases are emitted in parent order. */
    [[nodiscard]] bool build_aliases() {
        for (std::size_t index = 0; index < inputs_.activities.size(); ++index) {
            const ActivityInput& row = inputs_.activities[index];
            const std::size_t first = output_.texts.size();
            if ((!row.internalName.empty()
                 && !add_text(row.internalName, format::TextKind::internalAlias))
                || (!row.displayName.empty()
                    && !add_text(row.displayName, format::TextKind::displayAlias))
                || !make_range(
                    first, output_.texts.size() - first, output_.activityAliases[index])) {
                return false;
            }
        }

        std::vector<AliasModel> aliases{};
        aliases.reserve(inputs_.slotAliases.size() + inputs_.slots.size());
        for (std::size_t index = 0; index < inputs_.slots.size(); ++index) {
            if (!inputs_.slots[index].name.empty()) {
                aliases.push_back({static_cast<std::uint32_t>(index), inputs_.slots[index].name});
            }
        }
        for (const SlotAliasInput& row : inputs_.slotAliases) {
            if (!row.value.empty()) {
                aliases.push_back({row.slotIndex, row.value});
            }
        }
        std::sort(aliases.begin(),
                  aliases.end(),
                  [](const AliasModel& left, const AliasModel& right) noexcept {
                      return left.slotIndex != right.slotIndex ? left.slotIndex < right.slotIndex
                                                               : byte_less(left.value, right.value);
                  });
        aliases.erase(std::unique(aliases.begin(),
                                  aliases.end(),
                                  [](const AliasModel& left, const AliasModel& right) noexcept {
                                      return left.slotIndex == right.slotIndex
                                             && left.value == right.value;
                                  }),
                      aliases.end());

        std::size_t nextAlias = 0;
        for (std::size_t slotIndex = 0; slotIndex < inputs_.slots.size(); ++slotIndex) {
            const std::size_t first = output_.texts.size();
            while (nextAlias < aliases.size() && aliases[nextAlias].slotIndex == slotIndex) {
                if (!add_text(aliases[nextAlias].value, format::TextKind::slotAlias)) {
                    return false;
                }
                ++nextAlias;
            }
            if (!make_range(first, output_.texts.size() - first, output_.slotAliases[slotIndex])) {
                return false;
            }
        }
        return nextAlias == aliases.size();
    }

    /**
     * Appends one canonical gate and records a failed reason for later refusal rows.
     * @param name The gate name.
     * @param passed Whether exact evidence satisfied the gate.
     * @param reason The stable reason code used when the gate fails.
     * @param wouldConfirm The evidence needed to satisfy a failed gate.
     * @param failures Receives the failed reason code.
     * @return True when all deferred strings and the gate fit their bounds.
     */
    [[nodiscard]] bool add_gate(std::string_view name,
                                bool passed,
                                std::string_view reason,
                                std::string_view wouldConfirm,
                                FailureReasons& failures) {
        if (!can_add(output_.gates.size(), 1, kMaximumPolicyGateCount)) {
            return false;
        }
        Gate row{};
        if (!intern(name, row.gate) || !intern(passed ? kPassed : kFailed, row.status)
            || !intern(passed ? std::string_view{} : reason, row.reasonCode)
            || !intern(kExact, row.required) || !intern(passed ? kExact : kUnresolved, row.observed)
            || !intern(passed ? std::string_view{} : wouldConfirm, row.wouldConfirm)
            || (!passed && !failures.add(reason))) {
            return false;
        }
        output_.gates.push_back(row);
        return true;
    }

    /**
     * Appends one refusal and its sorted unique Text-owned reason rows.
     * @param capabilityId The canonical capability identity without a refusal prefix.
     * @param exposure The refused exposure surface.
     * @param status The exposure status.
     * @param failures The reasons accumulated by the capability gates.
     * @param capabilityIndex The owning capability row.
     * @return True when all rows and deferred strings fit their bounds.
     */
    [[nodiscard]] bool add_refusal(std::string_view capabilityId,
                                   std::string_view exposure,
                                   std::string_view status,
                                   FailureReasons& failures,
                                   std::uint32_t capabilityIndex) {
        if (!can_add(output_.refusals.size(), 1, kMaximumPolicyRefusalCount)) {
            return false;
        }
        failures.canonicalize();
        std::string id("refusal/");
        id.append(capabilityId);
        id.push_back('/');
        id.append(exposure);
        const std::size_t firstReason = output_.texts.size();
        for (std::size_t index = 0; index < failures.count; ++index) {
            if (!add_text(failures.values[index], format::TextKind::refusalReason)) {
                return false;
            }
        }
        Refusal row{};
        row.capabilityIndex = capabilityIndex;
        if (!intern(id, row.id) || !intern(exposure, row.exposure) || !intern(status, row.status)
            || !make_range(firstReason, output_.texts.size() - firstReason, row.reasonCodes)) {
            return false;
        }
        output_.refusals.push_back(row);
        return true;
    }

    /**
     * Initializes one capability and assigns its future canonical row index.
     * @param subjectId The stable subject identity.
     * @param operation The stable operation name.
     * @param valueSchemaId The optional value schema identity.
     * @param subjectKind The format-v12 subject domain.
     * @param subjectIndex The row within that subject domain.
     * @param exposureFlags The current allowed surfaces.
     * @param candidateExposureFlags The candidate-only surfaces.
     * @param capability Receives the initialized deferred row.
     * @param capabilityId Receives the constructed stable identity.
     * @param capabilityIndex Receives the future canonical row index.
     * @return True when the row identity is valid and fits the section bound.
     */
    [[nodiscard]] bool begin_capability(std::string_view subjectId,
                                        std::string_view operation,
                                        std::string_view valueSchemaId,
                                        format::SubjectKind subjectKind,
                                        std::uint32_t subjectIndex,
                                        std::uint32_t exposureFlags,
                                        std::uint32_t candidateExposureFlags,
                                        Capability& capability,
                                        std::string& capabilityId,
                                        std::uint32_t& capabilityIndex) {
        if (!can_add(output_.capabilities.size(), 1, kMaximumPolicyCapabilityCount)
            || output_.capabilities.size() >= format::kAbsentIndex) {
            return false;
        }
        capabilityId.assign("cap/");
        capabilityId.append(subjectId);
        capabilityId.push_back('/');
        capabilityId.append(operation);
        capabilityIndex = static_cast<std::uint32_t>(output_.capabilities.size());
        capability.subjectKind = static_cast<std::uint32_t>(subjectKind);
        capability.subjectIndex = subjectIndex;
        capability.exposureFlags = exposureFlags;
        capability.candidateExposureFlags = candidateExposureFlags;
        return intern(capabilityId, capability.id) && intern(operation, capability.operation)
               && intern(valueSchemaId, capability.valueSchemaId);
    }

    /**
     * Closes and appends one capability after all owned child rows are present.
     * @param capability The pending capability row.
     * @param firstGate Its first owned Gate row.
     * @param firstRefusal Its first owned Refusal row.
     * @return True when both ranges fit their packed fields.
     */
    [[nodiscard]] bool
    finish_capability(Capability& capability, std::size_t firstGate, std::size_t firstRefusal) {
        if (!make_range(firstGate, output_.gates.size() - firstGate, capability.gates)
            || !make_range(
                firstRefusal, output_.refusals.size() - firstRefusal, capability.refusals)) {
            return false;
        }
        output_.capabilities.push_back(capability);
        return true;
    }

    /** @return True after every activity mission-binding capability is emitted in row order. */
    [[nodiscard]] bool build_activity_capabilities() {
        for (std::size_t activityIndex = 0; activityIndex < inputs_.activities.size();
             ++activityIndex) {
            const ActivityInput& input = inputs_.activities[activityIndex];
            const std::size_t firstCapability = output_.capabilities.size();
            Capability capability{};
            std::string capabilityId{};
            std::uint32_t capabilityIndex = 0;
            if (!begin_capability(input.id,
                                  "mission.bind",
                                  {},
                                  format::SubjectKind::activity,
                                  static_cast<std::uint32_t>(activityIndex),
                                  format::kInspectExposure,
                                  0,
                                  capability,
                                  capabilityId,
                                  capabilityIndex)) {
                return false;
            }
            const std::size_t firstGate = output_.gates.size();
            FailureReasons failures{};
            const bool identityExact = input.joinStatus == ActivityJoinStatus::exact;
            // The gate publishes the activity's binding reason code. Naming the join status here
            // instead would invent a second vocabulary for the same fact.
            std::string_view identityReason = "activity_root_edge_missing";
            switch (input.joinStatus) {
            case ActivityJoinStatus::exact:
                identityReason = {};
                break;
            case ActivityJoinStatus::sourceNameMissing:
                identityReason = "no_direct_fixed_activity_name";
                break;
            case ActivityJoinStatus::liveNameMissing:
                identityReason = "installed_route_absent";
                break;
            case ActivityJoinStatus::liveNameAmbiguous:
                identityReason = "activity_root_name_ambiguous";
                break;
            }
            // Gate rows keep this emission order. It is the row order, not a sort key.
            if (!add_gate("subject_identity",
                          identityExact,
                          identityReason,
                          "One exact live scenario binding",
                          failures)
                || !add_gate(
                    "route", false, "route_unverified", "One exact live activity route", failures)
                || !add_gate("runtime_adapter",
                             false,
                             "runtime_adapter_missing",
                             "The script VM adapter",
                             failures)
                || !add_gate("host_role",
                             false,
                             "unsupported_host_role",
                             "An authoritative activity host",
                             failures)) {
                return false;
            }
            const std::size_t firstRefusal = output_.refusals.size();
            if (!add_refusal(capabilityId, kScript, kRefused, failures, capabilityIndex)
                || !finish_capability(capability, firstGate, firstRefusal)
                || !make_range(firstCapability,
                               output_.capabilities.size() - firstCapability,
                               output_.activityCapabilities[activityIndex])) {
                return false;
            }
        }
        return true;
    }

    /**
     * Emits the always-visible inspection capability for one final slot.
     * @param input The finalized slot evidence.
     * @param slotIndex The canonical slot subject index.
     * @return True when the capability and its gates fit their bounds.
     */
    [[nodiscard]] bool build_inspect_capability(const SlotInput& input, std::uint32_t slotIndex) {
        Capability capability{};
        std::string capabilityId{};
        std::uint32_t capabilityIndex = 0;
        if (!begin_capability(input.id,
                              "slot.inspect",
                              {},
                              format::SubjectKind::slot,
                              slotIndex,
                              format::kInspectExposure,
                              0,
                              capability,
                              capabilityId,
                              capabilityIndex)) {
            return false;
        }
        const std::size_t firstGate = output_.gates.size();
        FailureReasons failures{};
        const bool sourceComplete = input.extractionStatus == ExtractionStatus::complete
                                    || input.extractionStatus == ExtractionStatus::completeEmpty;
        if (!add_gate("source_complete",
                      sourceComplete,
                      "source_partial",
                      "A complete package walk",
                      failures)
            || !add_gate("subject_identity", true, {}, {}, failures)) {
            return false;
        }
        const std::size_t firstRefusal = output_.refusals.size();
        return finish_capability(capability, firstGate, firstRefusal);
    }

    /**
     * Emits one device or trigger operation from final slot and route evidence.
     * @param input The finalized slot evidence.
     * @param slotIndex The canonical slot subject index.
     * @param operation The stable adapter operation.
     * @return True when the capability, gates, and refusals fit their bounds.
     */
    [[nodiscard]] bool build_adapter_capability(const SlotInput& input,
                                                std::uint32_t slotIndex,
                                                std::string_view operation) {
        const bool type23 = input.slotType == format::kDeviceSlotType;
        const bool type5 = input.slotType == format::kSequenceSlotType
                           && input.componentClass == format::kSequenceComponentClass
                           && input.authSchema == format::kSequenceAuthSchema;
        const bool type6 = input.slotType == format::kCinematicSlotType
                           && input.componentClass == format::kCinematicComponentClass
                           && input.authSchema == format::kCinematicAuthSchema;
        const bool type31 = input.slotType == 31U && input.authSchema == 0x80809524U;
        const bool type53 = input.slotType == format::kDialogueSlotType
                            && input.componentClass == format::kDialogueComponentClass
                            && input.authSchema == format::kDialogueAuthSchema
                            && (input.flags & format::kSlotDialogueCuesExact) != 0;
        const bool knownAdapter = type5 || type6 || type23 || type31 || type53;
        const bool shapeExact = knownAdapter && (input.flags & format::kSlotSchemaJoinExact) != 0
                                && (!type23
                                    || (input.componentClass == format::kDeviceComponentClass
                                        && input.senseSchema == format::kDeviceSenseSchema
                                        && input.authSchema == format::kDeviceAuthSchema));
        const bool senseDecoded = !type23 || (shapeExact && !input.senseSchemaId.empty());
        const bool authDecoded = shapeExact && !input.authSchemaId.empty();
        const internal::RouteEvidence& route = routes_[input.objectIndex];
        const bool routeExact =
            route.hasScenario && !route.ambiguous && route.invalidOccurrences == 0;
        const std::string_view routeReason = route.ambiguous ? "bound_scenario_occurrence_ambiguous"
                                                             : "bound_scenario_occurrence_missing";

        const std::uint32_t prospectiveExposure =
            knownAdapter && shapeExact && senseDecoded && authDecoded && routeExact
                ? format::kExposureMask
                : format::kInspectExposure;
        const std::uint32_t prospectiveCandidate =
            prospectiveExposure == format::kExposureMask ? 0U : format::kPanelTestExposure;
        Capability capability{};
        std::string capabilityId{};
        std::uint32_t capabilityIndex = 0;
        if (!begin_capability(input.id,
                              operation,
                              input.authSchemaId,
                              format::SubjectKind::slot,
                              slotIndex,
                              prospectiveExposure,
                              prospectiveCandidate,
                              capability,
                              capabilityId,
                              capabilityIndex)) {
            return false;
        }

        const std::size_t firstGate = output_.gates.size();
        FailureReasons failures{};
        if (knownAdapter) {
            if (!add_gate("activation",
                          authDecoded,
                          "activation_unverified",
                          "The typed native Auth intent producer",
                          failures)
                || !add_gate(
                    "adapter_contract",
                    shapeExact,
                    "typed_adapter_contract_mismatch",
                    "The exact component, Auth schema, descriptor join, and authored bounds",
                    failures)
                || !add_gate("meaning",
                             shapeExact,
                             "meaning_unverified",
                             type5    ? "The verified authored-sequence revision transition"
                             : type6  ? "The verified cinematic generation and active state"
                             : type23 ? "The verified position, power, and lock channel mapping"
                             : type31 ? "The verified configured-trigger pulse mapping"
                                      : "The verified authored-dialogue cue mapping",
                             failures)
                || !add_gate("ownership",
                             true,
                             "ownership_unverified",
                             "The slot's exact owning object join",
                             failures)
                || !add_gate("producer",
                             authDecoded,
                             "producer_unverified",
                             "The typed activity message-5 Auth encoder",
                             failures)
                || !add_gate("reader",
                             shapeExact,
                             "reader_unverified",
                             "The build-pinned native Auth consumer",
                             failures)
                || !add_gate("route",
                             routeExact,
                             routeReason,
                             "Exactly one occurrence of the slot object in every bound scenario",
                             failures)
                || !add_gate("runtime_adapter",
                             shapeExact,
                             "runtime_adapter_missing",
                             type5    ? "The authored sequence adapter"
                             : type6  ? "The authored cinematic adapter"
                             : type23 ? "The SlotView set_channel adapter"
                             : type31 ? "The SlotView fire_trigger adapter"
                                      : "The dialogue cue adapter",
                             failures)
                || !add_gate("schema_decode",
                             senseDecoded,
                             "codec_decode_unverified",
                             type23 ? "The decoded exact type-23 Sense schema"
                                    : "No Sense schema is required by this action",
                             failures)
                || !add_gate("schema_encode",
                             authDecoded,
                             "codec_encoder_unavailable",
                             "The decoded exact Auth schema and implemented typed encoder",
                             failures)
                || !add_gate("source_complete",
                             input.extractionStatus == ExtractionStatus::complete,
                             "source_partial",
                             "A complete package walk",
                             failures)
                || !add_gate("subject_identity",
                             true,
                             "owning_object_missing",
                             "An exact owning object identity",
                             failures)) {
                return false;
            }
        } else {
            // No native evidence inventory currently proves these non-device mutation gates.
            if (!add_gate("activation",
                          false,
                          "activation_unverified",
                          "A verified activation producer",
                          failures)
                || !add_gate("host_role",
                             false,
                             "unsupported_host_role",
                             "An authoritative activity host",
                             failures)
                || !add_gate("live_effect",
                             false,
                             "live_effect_unverified",
                             "One visible end-to-end effect",
                             failures)
                || !add_gate(
                    "meaning", false, "meaning_unverified", "A verified state transition", failures)
                || !add_gate("ownership",
                             false,
                             "ownership_unverified",
                             "An exact owning object join",
                             failures)
                || !add_gate("producer",
                             false,
                             "producer_unverified",
                             "A verified native producer",
                             failures)
                || !add_gate("reader",
                             (input.flags & format::kSlotReaderVerified) != 0,
                             "reader_unverified",
                             "A verified native reader",
                             failures)
                || !add_gate(
                    "route", false, "route_unverified", "One exact live roster route", failures)
                || !add_gate("runtime_adapter",
                             false,
                             "runtime_adapter_missing",
                             "The script VM adapter",
                             failures)
                || !add_gate("schema_decode",
                             !input.senseSchemaId.empty(),
                             "codec_decode_unverified",
                             "A decoded exact schema",
                             failures)
                || !add_gate("schema_encode",
                             false,
                             "codec_encoder_missing",
                             "An implemented and verified encoder for the decoded auth schema",
                             failures)
                || !add_gate("source_complete",
                             input.extractionStatus == ExtractionStatus::complete,
                             "source_partial",
                             "A complete package walk",
                             failures)
                || !add_gate("subject_identity", true, {}, {}, failures)) {
                return false;
            }
        }

        const bool scriptReady = knownAdapter && failures.count == 0;
        capability.exposureFlags = scriptReady ? format::kExposureMask : format::kInspectExposure;
        capability.candidateExposureFlags = scriptReady ? 0U : format::kPanelTestExposure;
        const std::size_t firstRefusal = output_.refusals.size();
        if (!scriptReady
            && (!add_refusal(capabilityId, kPanelTest, kCandidate, failures, capabilityIndex)
                || !add_refusal(capabilityId, kScript, kRefused, failures, capabilityIndex))) {
            return false;
        }
        return finish_capability(capability, firstGate, firstRefusal);
    }

    /** @return True after every slot capability group is emitted in canonical operation order. */
    [[nodiscard]] bool build_slot_capabilities() {
        for (std::size_t index = 0; index < inputs_.slots.size(); ++index) {
            const SlotInput& input = inputs_.slots[index];
            const std::uint32_t slotIndex = static_cast<std::uint32_t>(index);
            const std::size_t firstCapability = output_.capabilities.size();
            if (input.slotType == format::kDeviceSlotType) {
                if (!build_adapter_capability(input, slotIndex, "device.lock")
                    || !build_adapter_capability(input, slotIndex, "device.position")
                    || !build_adapter_capability(input, slotIndex, "device.power")
                    || !build_inspect_capability(input, slotIndex)) {
                    return false;
                }
            } else if (input.slotType == kTriggerSlotType) {
                if (!build_inspect_capability(input, slotIndex)
                    || !build_adapter_capability(input, slotIndex, "trigger.pulse")) {
                    return false;
                }
            } else if (input.slotType == format::kDialogueSlotType) {
                if (!build_inspect_capability(input, slotIndex)
                    || !build_adapter_capability(input, slotIndex, "dialogue.play_line")) {
                    return false;
                }
            } else if (input.slotType == format::kSequenceSlotType) {
                if (!build_inspect_capability(input, slotIndex)
                    || !build_adapter_capability(input, slotIndex, "sequence.play")) {
                    return false;
                }
            } else if (input.slotType == format::kCinematicSlotType) {
                if (!build_inspect_capability(input, slotIndex)
                    || !build_adapter_capability(input, slotIndex, "cinematic.set_active")) {
                    return false;
                }
            } else if (!build_inspect_capability(input, slotIndex)) {
                return false;
            }
            if (!make_range(firstCapability,
                            output_.capabilities.size() - firstCapability,
                            output_.slotCapabilities[index])) {
                return false;
            }
        }
        return true;
    }

    /**
     * Emits one compiled host surface using its implemented adapter gate set.
     * @param model The host declaration and sorted subject index.
     * @return True when the capability, gates, and optional refusal fit their bounds.
     */
    [[nodiscard]] bool build_host_capability(const HostModel& model) {
        const HostSurface& input = *model.surface;
        const bool squad = input.operation == "squad.place";
        const bool routed = input.operation == "scene.activate"
                            || input.operation == "slot.apply_auth"
                            || input.operation == "slot.set_channel";
        const bool implemented = squad || routed;
        Capability capability{};
        std::string capabilityId{};
        std::uint32_t capabilityIndex = 0;
        if (!begin_capability(input.id,
                              input.operation,
                              {},
                              format::SubjectKind::hostApi,
                              model.subjectIndex,
                              implemented ? format::kExposureMask : format::kInspectExposure,
                              0,
                              capability,
                              capabilityId,
                              capabilityIndex)) {
            return false;
        }
        const std::size_t firstGate = output_.gates.size();
        FailureReasons failures{};
        if (squad) {
            if (!add_gate("authored_mapping", true, {}, {}, failures)
                || !add_gate("host_role", true, {}, {}, failures)
                || !add_gate("route", true, {}, {}, failures)
                || !add_gate("runtime_adapter", true, {}, {}, failures)) {
                return false;
            }
        } else if (routed) {
            if (!add_gate("authored_mapping", true, {}, {}, failures)
                || !add_gate("meaning", true, {}, {}, failures)
                || !add_gate("panel_adapter", true, {}, {}, failures)
                || !add_gate("reader", true, {}, {}, failures)
                || !add_gate("route_contract", true, {}, {}, failures)
                || !add_gate("runtime_adapter", true, {}, {}, failures)
                || !add_gate("schema_encode", true, {}, {}, failures)) {
                return false;
            }
        } else if (!add_gate("host_role",
                             false,
                             "unsupported_host_role",
                             "An authoritative activity host",
                             failures)
                   || !add_gate("live_effect",
                                false,
                                "live_effect_unverified",
                                "One visible end-to-end effect",
                                failures)
                   || !add_gate("meaning",
                                input.operation == "sleep_until",
                                "meaning_unverified",
                                "A verified retail action mapping",
                                failures)
                   || !add_gate("runtime_adapter",
                                false,
                                "runtime_adapter_missing",
                                "The script VM adapter",
                                failures)) {
            return false;
        }
        const std::size_t firstRefusal = output_.refusals.size();
        if (!implemented
            && !add_refusal(capabilityId, kScript, kRefused, failures, capabilityIndex)) {
            return false;
        }
        return finish_capability(capability, firstGate, firstRefusal);
    }

    /** @return True after host subjects and operations are emitted in byte-sorted order. */
    [[nodiscard]] bool build_host_capabilities() {
        std::vector<std::string_view> subjects{};
        subjects.reserve(inputs_.hostSurfaces.size());
        for (const HostSurface& row : inputs_.hostSurfaces) {
            subjects.push_back(row.id);
        }
        std::sort(subjects.begin(), subjects.end(), byte_less);
        subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
        if (subjects.size() > kMaximumHostSurfaceCount) {
            return false;
        }
        output_.hostSubjects.resize(subjects.size());
        for (std::size_t index = 0; index < subjects.size(); ++index) {
            if (!intern(subjects[index], output_.hostSubjects[index].id)) {
                return false;
            }
        }

        std::vector<HostModel> models{};
        models.reserve(inputs_.hostSurfaces.size());
        for (const HostSurface& row : inputs_.hostSurfaces) {
            const auto found =
                std::lower_bound(subjects.begin(), subjects.end(), row.id, byte_less);
            if (found == subjects.end() || *found != row.id) {
                return false;
            }
            models.push_back({&row, static_cast<std::uint32_t>(found - subjects.begin())});
        }
        std::sort(models.begin(),
                  models.end(),
                  [](const HostModel& left, const HostModel& right) noexcept {
                      if (left.subjectIndex != right.subjectIndex) {
                          return left.subjectIndex < right.subjectIndex;
                      }
                      if (left.surface->operation != right.surface->operation) {
                          return byte_less(left.surface->operation, right.surface->operation);
                      }
                      return byte_less(left.surface->id, right.surface->id);
                  });

        std::size_t nextModel = 0;
        for (std::size_t subjectIndex = 0; subjectIndex < subjects.size(); ++subjectIndex) {
            const std::size_t firstCapability = output_.capabilities.size();
            while (nextModel < models.size() && models[nextModel].subjectIndex == subjectIndex) {
                if (!build_host_capability(models[nextModel])) {
                    return false;
                }
                ++nextModel;
            }
            if (!make_range(firstCapability,
                            output_.capabilities.size() - firstCapability,
                            output_.hostSubjects[subjectIndex].capabilities)) {
                return false;
            }
        }
        return nextModel == models.size();
    }

    const Inputs& inputs_;
    std::span<const internal::RouteEvidence> routes_{};
    Snapshot output_{};
    std::unordered_map<std::string, std::uint32_t, StringHash, StringEqual> stringIndexes_{};
};

} // namespace

std::string_view Snapshot::value(Text text) const noexcept {
    return text.stringIndex < strings.size() ? std::string_view(strings[text.stringIndex])
                                             : std::string_view{};
}

std::span<const HostSurface> host_surfaces() noexcept {
    return kHostSurfaces;
}

/**
 * Builds a complete bounded policy snapshot without changing output on failure.
 * @param inputs Final parent rows and evidence borrowed for the duration of this call.
 * @param output Receives the complete owned snapshot only after success.
 * @return True when validation and every policy projection step succeed.
 */
bool build(const Inputs& inputs, Snapshot& output) noexcept {
    try {
        if (!internal::valid_inputs(inputs)) {
            return false;
        }
        std::vector<internal::RouteEvidence> routes;
        if (!internal::build_routes(inputs, routes)) {
            return false;
        }
        Builder builder(inputs, routes);
        if (!builder.run()) {
            return false;
        }
        output = builder.take();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::policy_inventory
