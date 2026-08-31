#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../core/settings/settings.h"
#include "../activity/defaults/activity_defaults_validation.h"
#include "../build_data/records/rewards/reward_persistence.h"
#include "../build_data/runtime.h"
#include "../progression/seasonal_experience.h"
#include "../record_claims/record_claims.h"
#include "equipment/configured_equipment_identity.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::storage {

State g_state;
SRWLOCK g_stateLock{SRWLOCK_INIT};

} // namespace runtime::storage

namespace {

/** Network-order IPv4 loopback returned by the in-process SignOn route. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** Default one-hour lifetime for generated SignOn session tokens. */
constexpr std::uint32_t kDefaultTokenLifetimeSeconds = 3600;
/** Family 5 uses the largest signed 64-bit value as its process-global object key. */
constexpr std::uint64_t kGlobalFamily5Soid =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/** Global unlock-value slot named by the installed build's season constants. */
constexpr std::uint16_t kActiveSeasonValueSlot = 607;
/** One-based season number carried by the Season of Arrivals definition. */
constexpr std::int32_t kSeasonOfArrivalsNumber = 11;

/**
 * Makes one process-owned global value authoritative without disturbing authored overrides.
 * @return False only when a new row is needed and the bounded family-5 list is full.
 */
[[nodiscard]] bool
upsert_family5_value(Family5State& family, std::uint16_t slot, std::int32_t value) noexcept {
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == slot) {
            family.values[index].value = value;
            return true;
        }
    }
    if (family.valueCount >= family.values.size()) {
        return false;
    }
    family.values[family.valueCount++] = UnlockValueOverride{slot, value};
    return true;
}

/**
 * Fills fixed secret storage with Windows system randomness.
 * @tparam Size Required secret byte count.
 * @param output Secret storage to overwrite.
 * @return True when Windows generates every byte.
 */
template <std::size_t Size>
[[nodiscard]] bool randomize(std::array<std::byte, Size>& output) noexcept {
    return BCryptGenRandom(nullptr,
                           reinterpret_cast<PUCHAR>(output.data()),
                           static_cast<ULONG>(output.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG)
           >= 0;
}

/** Seeds canonical character row generations before installed build data is needed. */
[[nodiscard]] bool seed_inventory_runtime_fields(AccountState& accountState) noexcept {
    if (!account::valid_authored(accountState)) {
        return false;
    }
    for (std::size_t characterIndex = 0; characterIndex < accountState.characterCount;
         ++characterIndex) {
        CharacterState& character = accountState.characters[characterIndex];
        std::uint32_t next = 0;
        for (std::optional<account::inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()) {
                item->mutationSerial = static_cast<std::int32_t>(next++);
            }
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            character.inventory.values[index].mutationSerial = static_cast<std::int32_t>(next++);
        }
        for (std::size_t index = 0; index < character.stacks.count; ++index) {
            character.stacks.values[index].mutationSerial = static_cast<std::int32_t>(next++);
        }
        character.nextInventorySerial = next;
    }
    return account::valid(accountState);
}

/**
 * Canonicalizes only profile rows which the installed socket UI materializes as action sources.
 * @param accountState Account canonicalized in place.
 * @return True when every profile row canonicalizes.
 */
[[nodiscard]] bool canonicalize_profile_item_identities(AccountState& accountState) noexcept {
    if (!account::valid(accountState)) {
        return false;
    }
    if (accountState.profileItemCount == 0) {
        // Nothing to canonicalize, so the socket relation is not needed. Demanding it here would
        // refuse the first account snapshot of an account that owns no profile stack at all, and
        // an empty account family never becomes active.
        return true;
    }
    if (!build_data::socket_plug_rules_ready()) {
        return false;
    }
    std::array<bool, account::inventory::kProfileItemCapacity> actionSources{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const account::inventory::ProfileItem& profileItem = accountState.profileItems[index];
        build_data::items::Definition item{};
        build_data::items::details::Definition detail{};
        build_data::inventory::buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_hash(profileItem.definitionHash, item)
            || item.definitionHash != profileItem.definitionHash
            || !build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || detail.instancedDefinitionState
                   != build_data::items::details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile) {
            return false;
        }
        actionSources[index] =
            build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
        if (actionSources[index]
            && ++actionSourceCount > account::inventory::kProfileActionSourceCapacity) {
            return false;
        }
    }

    // Currency, material, and consumable rows are native non-instanced stacks.  Clear any stale
    // runtime key before allocating action-source identities so it cannot reserve the namespace.
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        if (!actionSources[index]) {
            accountState.profileItems[index].instanceSoid = 0;
        }
    }

    std::uint64_t nextProfileSoid = account::inventory::kFirstProfileItemInstanceSoid;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        account::inventory::ProfileItem& item = accountState.profileItems[index];
        if (!actionSources[index] || item.instanceSoid != 0) {
            continue;
        }
        while (runtime::detail::account_owns_soid(accountState, nextProfileSoid)) {
            if (nextProfileSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            ++nextProfileSoid;
        }
        item.instanceSoid = nextProfileSoid;
        if (nextProfileSoid != (std::numeric_limits<std::uint64_t>::max)()) {
            ++nextProfileSoid;
        }
    }
    return account::valid(accountState);
}

} // namespace

/**
 * Loads build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @return True when the cached data passes its checks and every secret gets random bytes.
 */
bool initialize(void* module, const AccountState& initialAccount) noexcept {
    return initialize(module, initialAccount, activity::defaults::authored());
}

/**
 * Loads build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
bool initialize(void* module,
                const AccountState& initialAccount,
                const activity::defaults::ActivityDefaults& activityDefaults) noexcept {
    AccountState runtimeAccount = initialAccount;
    if (!seed_inventory_runtime_fields(runtimeAccount)
        || !activity::defaults::valid(activityDefaults)) {
        return false;
    }
    if (!build_data::initialize(module, runtime::equipment::configured_hash(runtimeAccount))) {
        return false;
    }
    if (build_data::records::rewards::initialize(module)) {
        (void)build_data::records::rewards::load_and_publish();
    }
    (void)record_claims::initialize(module);
    (void)progression::seasonal_experience::initialize(module);
    // A cache hit already has the complete plug relation, so publish canonical profile identities
    // in the first State image.  On a first cache build, snapshot preparation repeats this step
    // after package extraction has published the relation.
    if (build_data::socket_plug_rules_ready()
        && !canonicalize_profile_item_identities(runtimeAccount)) {
        build_data::shutdown();
        return false;
    }
    State initialized{};
    if (!randomize(initialized.signOn.encryptionKey)
        || !randomize(initialized.signOn.authenticationKey)
        || !randomize(initialized.signOn.sessionToken) || !randomize(initialized.bap.nonce)
        || !randomize(initialized.bap.sessionKey) || !randomize(initialized.bap.envelopeIv)) {
        SecureZeroMemory(&initialized, sizeof initialized);
        build_data::shutdown();
        return false;
    }
    initialized.signOn.relayAddress = kLoopbackAddress;
    // The published relay port is the one the listener binds, so both move with one setting.
    initialized.signOn.relayPort = core::settings::get().server.bapPort;
    initialized.signOn.tokenLifetimeSeconds = kDefaultTokenLifetimeSeconds;
    initialized.account = runtimeAccount;
    initialized.activity.defaults = activityDefaults;
    initialized.investment.family5.objectSoid = kGlobalFamily5Soid;
    // Only the override lists come from settings. Identity and gate stay owned by State.
    const Family5State& authored = core::settings::get().initialFamily5;
    initialized.investment.family5.flags = authored.flags;
    initialized.investment.family5.flagCount = authored.flagCount;
    initialized.investment.family5.values = authored.values;
    initialized.investment.family5.valueCount = authored.valueCount;
    // Family 5 addresses the Client's global unlock-value space directly. Slot 607 selects the
    // season definition; account objective rows use a separate mapped index space and cannot.
    if (!upsert_family5_value(
            initialized.investment.family5, kActiveSeasonValueSlot, kSeasonOfArrivalsNumber)) {
        SecureZeroMemory(&initialized, sizeof initialized);
        build_data::shutdown();
        return false;
    }
    // The arm is account-wide and rides the first ws-503, which goes out before any pick. Nothing
    // is selected at boot, so it is armed when any authored character carries the bypass. The
    // per-character objB byte is the other half, and it still decides which character it opens.
    for (std::size_t index = 0; index < runtimeAccount.characterCount; ++index) {
        if (runtimeAccount.characters[index].contentBypass) {
            initialized.investment.family5.contentGateArm = true;
            break;
        }
    }

    // Publish one complete State only after every generated secret is valid.
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state = initialized;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&initialized, sizeof initialized);
    return true;
}

/** Securely erases State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&runtime::storage::g_state, sizeof runtime::storage::g_state);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    progression::seasonal_experience::shutdown();
    build_data::shutdown();
}

/** @return Immutable generated SignOn session fields. */
const SignOnState& sign_on() noexcept {
    return runtime::storage::g_state.signOn;
}

/** Ensures every native profile action source has one unique runtime item-instance key. */
bool ensure_profile_item_identities() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    const bool ready = canonicalize_profile_item_identities(candidate);
    if (ready) {
        runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ready;
}

/**
 * Publishes the bootstrap content-id token read from the installed client.
 * @param token Exactly 16 native bytes.
 * @return True when the complete token is kept for this process.
 */
bool publish_bootstrap_token(std::span<const std::byte> token) noexcept {
    SignOnState& signOn = runtime::storage::g_state.signOn;
    if (token.size() != signOn.bootstrapToken.size()) {
        return false;
    }
    std::copy(token.begin(), token.end(), signOn.bootstrapToken.begin());
    signOn.bootstrapTokenPresent = true;
    return true;
}

/** @return Immutable generated BAP session fields. */
const BapState& bap() noexcept {
    return runtime::storage::g_state.bap;
}

/** @return A copy of the evaluated content state, read under the lock. */
InvestmentState investment_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    InvestmentState snapshot = runtime::storage::g_state.investment;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    (void)progression::seasonal_experience::apply_artifact_state(snapshot.family5);
    return snapshot;
}

bool refresh_artifact_progression() noexcept {
    // Artifact progress is projected into each Family-4 account image at encode time.
    return true;
}

bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                 PendingArtifactPurchase& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (!account::valid(account)) {
        return false;
    }
    std::size_t selected = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selected = index;
            break;
        }
    }
    if (selected >= account.characterCount
        || !progression::seasonal_experience::prepare_artifact_mod_unlock(
            saleIndex, mutation.beforeMask, mutation.afterMask)) {
        mutation = {};
        return false;
    }
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[selected].soid;
    mutation.characterIndex = selected;
    mutation.saleIndex = saleIndex;
    mutation.prepared = true;
    return true;
}

bool commit_artifact_mod_unlock(PendingArtifactPurchase& mutation) noexcept {
    const PendingArtifactPurchase prepared = mutation;
    mutation = {};
    const AccountState account = account_snapshot();
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.beforeMask == prepared.afterMask
        || prepared.characterIndex >= account.characterCount
        || account.primarySoid != prepared.accountSoid
        || account.characters[prepared.characterIndex].soid != prepared.characterSoid
        || !account.characters[prepared.characterIndex].selected) {
        return false;
    }
    return progression::seasonal_experience::replace_artifact_mod_mask(
        prepared.beforeMask, prepared.afterMask);
}

bool reset_artifact(std::int32_t glimmerCost, ArtifactResetResult& result) noexcept {
    result = {};
    constexpr std::uint32_t kGlimmerHash = 3159615086U;
    constexpr std::array<std::uint32_t, 25> kArtifactModHashes{
        715026181U,  715026182U,  715026183U,  715026176U,  715026177U,
        3213968582U, 3213968581U, 3213968580U, 3213968579U, 3213968578U,
        3465659109U, 3465659110U, 3465659111U, 3465659104U, 3465659105U,
        3175764264U, 3175764267U, 3175764266U, 3175764269U, 3175764268U,
        4186620519U, 4186620516U, 4186620517U, 4186620514U, 4186620515U};
    if (glimmerCost <= 0) {
        return false;
    }

    const std::uint32_t previousMods = progression::seasonal_experience::artifact_mod_mask();
    const AccountState before = account_snapshot();
    if (previousMods == 0 || !account::valid(before)
        || !runtime::detail::valid_profile_inventory(before)) {
        return false;
    }

    std::int32_t remainingCost = glimmerCost;
    auto compacted = before.profileItems;
    std::size_t compactedCount = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        auto item = before.profileItems[index];
        if (item.definitionHash == kGlimmerHash && remainingCost > 0) {
            const std::int32_t spent = (std::min)(item.quantity, remainingCost);
            item.quantity -= spent;
            remainingCost -= spent;
        }
        const bool artifactMod =
            std::find(kArtifactModHashes.begin(), kArtifactModHashes.end(), item.definitionHash)
            != kArtifactModHashes.end();
        if (item.quantity > 0 && !artifactMod) {
            compacted[compactedCount++] = item;
        }
    }
    if (remainingCost != 0) {
        return false;
    }
    std::fill(compacted.begin() + static_cast<std::ptrdiff_t>(compactedCount),
              compacted.end(),
              account::inventory::ProfileItem{});

    std::int32_t serial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        serial = (std::max)(serial, before.profileItems[index].mutationSerial);
    }
    const auto same = [](const auto& left, const auto& right) noexcept {
        return left.instanceSoid == right.instanceSoid
               && left.definitionHash == right.definitionHash && left.quantity == right.quantity
               && left.mutationSerial == right.mutationSerial;
    };
    std::size_t changedRows = 0;
    for (std::size_t index = 0; index < compactedCount; ++index) {
        changedRows += static_cast<std::size_t>(index >= before.profileItemCount
                                                || !same(compacted[index],
                                                         before.profileItems[index]));
    }
    if (changedRows > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()
                                               - serial)) {
        return false;
    }
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same(compacted[index], before.profileItems[index])) {
            compacted[index].mutationSerial = ++serial;
        }
    }

    AccountState candidate = before;
    candidate.profileItems = compacted;
    candidate.profileItemCount = compactedCount;
    const auto clearArtifactSockets = [&](account::inventory::Item& item) noexcept {
        if (item.sockets.policy != account::inventory::SocketPolicy::authored) {
            return true;
        }
        build_data::items::Definition base{};
        build_data::items::details::Definition detail{};
        if (!build_data::find_item_definition_hash(item.definitionHash, base)
            || !build_data::find_configured_item_detail(base.definitionIndex, detail)
            || detail.definitionIndex != base.definitionIndex
            || item.sockets.plugCount != detail.ordinarySocketCount) {
            return false;
        }
        for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
            const auto& plug = item.sockets.plugs[lane];
            if (!plug.has_value()
                || std::find(kArtifactModHashes.begin(), kArtifactModHashes.end(), *plug)
                       == kArtifactModHashes.end()) {
                continue;
            }
            const std::uint16_t initial = detail.initialPlugIndices[lane];
            if (initial == build_data::items::details::kUnavailableItemIndex) {
                item.sockets.plugs[lane].reset();
                continue;
            }
            build_data::items::Definition replacement{};
            if (!build_data::find_item_definition_index(initial, replacement)) {
                return false;
            }
            item.sockets.plugs[lane] = replacement.definitionHash;
        }
        return true;
    };
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        auto& character = candidate.characters[characterIndex];
        for (auto& item : character.equipment.slots) {
            if (item.has_value() && !clearArtifactSockets(*item)) {
                return false;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            if (!clearArtifactSockets(character.inventory.values[itemIndex])) {
                return false;
            }
        }
    }
    ArtifactResetResult changed{};
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        if (!before.characters[characterIndex].selected) {
            continue;
        }
        const auto recordChanged = [&changed](const account::inventory::Item& prior,
                                              const account::inventory::Item& current) noexcept {
            if (prior.sockets.policy == current.sockets.policy
                && prior.sockets.plugCount == current.sockets.plugCount
                && prior.sockets.plugs == current.sockets.plugs) {
                return true;
            }
            if (changed.instanceCount >= changed.instanceSoids.size()) {
                return false;
            }
            changed.instanceSoids[changed.instanceCount++] = current.instanceSoid;
            return true;
        };
        const auto& prior = before.characters[characterIndex];
        const auto& current = candidate.characters[characterIndex];
        for (std::size_t slot = 0; slot < current.equipment.slots.size(); ++slot) {
            if (current.equipment.slots[slot].has_value()
                && !recordChanged(*prior.equipment.slots[slot], *current.equipment.slots[slot])) {
                return false;
            }
        }
        for (std::size_t index = 0; index < current.inventory.count; ++index) {
            if (!recordChanged(prior.inventory.values[index], current.inventory.values[index])) {
                return false;
            }
        }
    }
    if (!account::valid(candidate) || !runtime::detail::valid_profile_inventory(candidate)
        || !progression::seasonal_experience::replace_artifact_mod_mask(previousMods, 0)) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    bool current = runtime::detail::same_profile_inventory(
        runtime::storage::g_state.account, before.profileItems, before.profileItemCount)
                   && runtime::storage::g_state.account.characterCount == before.characterCount;
    for (std::size_t index = 0; current && index < before.characterCount; ++index) {
        current = runtime::detail::same_character(runtime::storage::g_state.account.characters[index],
                                                  before.characters[index]);
    }
    if (current) {
        runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    if (!current) {
        (void)progression::seasonal_experience::replace_artifact_mod_mask(0, previousMods);
    } else {
        result = changed;
    }
    return current;
}

} // namespace sunrise::state
