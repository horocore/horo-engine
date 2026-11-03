#include "Horo/Prefab/PrefabPersistentState.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <set>

namespace Horo::Prefab {
    namespace {
        /** @brief Keeps compatibility diagnostics in the owning prefab domain without
         * collapsing the actionable cause. */
        Result<void> Invalid(std::string message) {
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, std::move(message)));
        }

        /** @brief Validates durable links as exact world-qualified incarnations, never
         * runtime slots. */
        bool ValidReference(const PrefabPersistentEntityReference &reference, Runtime::SaveWorldId world) noexcept {
            return reference.target.world == world && reference.target.entity.IsValid() && reference.generation.IsValid();
        }

        /** @brief Checks mandatory recipe identity fields independently from optional placement/version admission. */
        bool HasCreationIdentity(const PrefabPersistentInstance &instance) noexcept {
            return instance.identity.IsValid() && !instance.creator.empty() &&
                   instance.creator.size() <= Gameplay::MaximumBehaviorTypeIdBytes && IsValidUtf8ScalarSequence(instance.creator) &&
                   instance.source.asset.IsValid() && instance.source.target.IsValid() &&
                   instance.source.artifactDigest != Sha256Digest{} && instance.rootSourceRevision.contentDigest != Sha256Digest{};
        }

        /** @brief Matches one actual saved member's provenance to the exact occurrence and source asset. */
        bool HasMemberIdentity(const PrefabPersistentInstance &instance, const Runtime::PersistentEntityRecord &record) noexcept {
            const auto *origin = std::get_if<Runtime::SpawnedPersistentEntityProvenance>(&record.provenance);
            return origin && origin->prefabInstance && *origin->prefabInstance == instance.identity &&
                   origin->definition.Bytes() == instance.source.asset.Bytes() && record.identity.IsValid() && record.generation.IsValid();
        }

        /** @brief Rejects absent creation authority while preserving actual cooked/root
         * revision distinction. */
        Result<void> Creation(const PrefabPersistentInstance &instance) {
            if (!HasCreationIdentity(instance) || instance.placement.TryToMatrix().HasError() || instance.members.empty())
                return Invalid("Durable prefab creation requires exact instance, cooked "
                               "artifact, root revision and placement evidence.");
            const auto version = Application::ParseHoroVersion(Application::FormatHoroVersion(instance.rootSourceRevision.projectVersion));
            if (version.HasError() || version.Value() != instance.rootSourceRevision.projectVersion)
                return Invalid("Durable prefab root revision has an invalid canonical "
                               "project version.");
            if (instance.disposition > Runtime::PersistentEntityDisposition::Tombstone)
                return Invalid("Durable prefab disposition is invalid.");
            return Result<void>::Success();
        }

        /** @brief Confirms every member retains the exact prefab occurrence as its
         * creation provenance. */
        Result<void> Member(const PrefabPersistentInstance &instance, const PrefabPersistentMember &member) {
            const auto &record = member.record;
            if (!HasMemberIdentity(instance, record))
                return Invalid("Durable prefab member identity does not match its exact "
                               "creation recipe.");
            if (!member.source.sourceAsset.IsValid() || member.source.sourceDigest == Sha256Digest{} ||
                record.disposition > Runtime::PersistentEntityDisposition::Tombstone)
                return Invalid("Durable prefab member lacks valid original cooked source "
                               "evidence or disposition.");
            if (instance.disposition == Runtime::PersistentEntityDisposition::Tombstone &&
                record.disposition != Runtime::PersistentEntityDisposition::Tombstone)
                return Invalid("A destroyed prefab occurrence cannot retain a live member.");
            return Result<void>::Success();
        }

        /** @brief Persistent attachment keys are per entity, preserving legal duplicate IDs on different objects. */
        Result<void> Behaviors(const PrefabPersistentMember &member, std::size_t maximum) {
            if (member.behaviors.size() > maximum)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::set<Gameplay::BehaviorInstanceId> source;
            std::set<Gameplay::BehaviorInstanceId> runtime;
            for (const auto &behavior : member.behaviors) {
                if (behavior.type.Value().empty() || !behavior.source.value || !behavior.runtime.value || !behavior.schemaVersion)
                    return Invalid("Durable prefab behavior identity or schema is invalid.");
                if (!source.insert(behavior.source).second || !runtime.insert(behavior.runtime).second)
                    return Invalid("Durable prefab repeats a source or runtime attachment identity within one entity.");
            }
            return Result<void>::Success();
        }

        /** @brief Enforces complete stable addresses and shared identity uniqueness
         * before parent ordering. */
        Result<void> Members(const PrefabPersistentInstance &instance, std::set<Runtime::PersistentEntityId> &identities,
                             std::size_t maximumBehaviors) {
            std::set<PrefabObjectAddress> addresses;
            for (const auto &member : instance.members) {
                if (auto valid = Member(instance, member); valid.HasError())
                    return valid;
                if (auto valid = Behaviors(member, maximumBehaviors); valid.HasError())
                    return valid;
                if (!identities.insert(member.record.identity).second || !addresses.insert(member.source.sourceObject).second)
                    return Invalid("Durable prefab recipes contain duplicate entity identity "
                                   "or source object address.");
            }
            return Result<void>::Success();
        }

        /** @brief Persisted intent remains the same typed delta algebra and may not
         * hide unresolved source/schema changes. */
        Result<void> Overrides(const PrefabPersistentInstance &instance, const PrefabLimitProfile &profile) {
            if (instance.overrides.sourcePrefab != instance.source.asset ||
                instance.overrides.authoredAgainst != instance.rootSourceRevision)
                return Invalid("Durable prefab override source does not match its creation revision.");
            if (instance.overrides.HasUnresolved())
                return Result<void>::Failure(MakeError(PrefabErrors::OverrideConflict));
            return ValidatePrefabValueOverrideSet(instance.overrides, profile);
        }

        /** @brief Validates each external binding once without treating an absent
         * inventory target as deletion authority. */
        Result<void> Links(const PrefabPersistentInstance &instance, Runtime::SaveWorldId world) {
            if (instance.parent && !ValidReference(*instance.parent, world))
                return Invalid("Durable prefab parent belongs to a different world or "
                               "invalid incarnation.");
            std::set<PrefabPropertyId> slots;
            for (const auto &binding : instance.bindings)
                if (!binding.slot.Value() || !ValidReference(binding.target, world) || !slots.insert(binding.slot).second)
                    return Invalid("Durable prefab external binding is invalid or repeated.");
            return Result<void>::Success();
        }
    }  // namespace

    namespace {
        /** @brief Checks closed initialization value shape; actual declared per-field bounds remain the real cooked projector's authority.
         */
        bool ValidInitializationValue(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([]<typename Value>(const Value &input) {
                if constexpr (std::is_same_v<Value, std::monostate>)
                    return false;
                else if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(input);
                else if constexpr (std::is_same_v<Value, std::string>)
                    return input.size() <= 256 && IsValidUtf8ScalarSequence(input);
                else if constexpr (std::is_same_v<Value, Math::Vec2> || std::is_same_v<Value, Math::Vec3>)
                    return Math::IsFinite(input);
                else if constexpr (std::is_same_v<Value, Math::Quaternion>)
                    return input.TryNormalized().HasValue();
                else
                    return true;
            }, value);
        }

        /** @brief Duplicate initialization interfaces are contradictions, never first/last record wins. */
        Result<void> Initializations(const PrefabPersistentInstance &instance) {
            std::set<PrefabInitializationId> interfaces;
            for (const auto &value : instance.initialization)
                if (!value.id.value || !ValidInitializationValue(value.value) || !interfaces.insert(value.id).second)
                    return Invalid("Durable prefab initialization contains an invalid or duplicate typed interface.");
            return Result<void>::Success();
        }

        /** @brief Retains exact creation ancestry so restoration cannot reset runtime recursion/depth policy. */
        Result<void> Lineage(const PrefabPersistentInstance &instance, const PrefabLimitProfile &profile) {
            if (instance.creationLineage.empty() || instance.creationLineage.size() > profile.Policy().maximumRuntimeSpawnDepth ||
                instance.creationLineage.back() != instance.source.asset)
                return Invalid("Durable prefab creation lineage is absent, over budget or does not end at its own asset.");
            std::set<Assets::AssetId> unique;
            for (const auto asset : instance.creationLineage)
                if (!asset.IsValid() || !unique.insert(asset).second)
                    return Invalid("Durable prefab creation lineage contains an invalid or repeated asset.");
            return Result<void>::Success();
        }

        /** @brief Validates one whole recipe against the same aggregate inventory
         * charge. */
        Result<void> Instance(const PrefabPersistentInstance &instance, Runtime::SaveWorldId world, const PrefabPersistenceLimits &limits,
                              const PrefabLimitProfile &profile, std::set<Runtime::PersistentEntityId> &identities) {
            if (auto valid = Creation(instance); valid.HasError())
                return valid;
            if (instance.members.size() > limits.maximumMembers - identities.size() ||
                instance.members.size() > profile.Policy().maximumObjectCount ||
                instance.initialization.size() > MaximumPrefabInitializationValues ||
                instance.bindings.size() > profile.Policy().maximumInstanceBindings)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            if (auto valid = Members(instance, identities, profile.Policy().maximumComponentsPerObject); valid.HasError())
                return valid;
            if (auto valid = Overrides(instance, profile); valid.HasError())
                return valid;
            if (auto valid = Initializations(instance); valid.HasError())
                return valid;
            if (auto valid = Lineage(instance, profile); valid.HasError())
                return valid;
            return Links(instance, world);
        }

        /** @brief Checks trusted policy before allocating any canonical ordering
         * scratch. */
        bool ValidBounds(const PrefabPersistentState &state, const PrefabPersistenceLimits &limits) noexcept {
            return state.world.IsValid() && IsValidPrefabPersistenceLimits(limits);
        }

        /** @brief Orders stable recipe identities before reporting any semantic
         * failure. */
        Result<void> Validate(const PrefabPersistentState &state, const PrefabPersistenceLimits &limits,
                              const PrefabLimitProfile &profile) {
            if (!ValidBounds(state, limits))
                return Invalid("Durable prefab capture bounds or world identity are invalid.");
            if (state.instances.size() > limits.maximumInstances)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::vector<const PrefabPersistentInstance *> ordered;
            ordered.reserve(state.instances.size());
            for (const auto &instance : state.instances)
                ordered.push_back(&instance);
            std::ranges::sort(ordered, {}, [](const auto *instance) {
                return instance->identity;
            });
            std::set<Runtime::PersistentEntityId> identities;
            Runtime::SavePrefabInstanceId previous;
            for (const auto *instance : ordered) {
                if (instance->identity == previous)
                    return Invalid("Durable prefab instance identity is invalid or duplicated.");
                previous = instance->identity;
                if (auto valid = Instance(*instance, state.world, limits, profile, identities); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePrefabPersistentState */
    Result<void> ValidatePrefabPersistentState(const PrefabPersistentState &state, const PrefabPersistenceLimits &limits,
                                               const PrefabLimitProfile &profile) {
        try {
            return Validate(state, limits, profile);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Prefab
