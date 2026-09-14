#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"

#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &code, std::string message = {}) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] bool IsValidProvenance(const PersistentEntityProvenance &provenance) noexcept {
            return std::visit([]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, AuthoredPersistentEntityProvenance>)
                    return value.scene.IsValid() && value.object.IsValid();
                else
                    return value.definition.IsValid() && (!value.prefabInstance.has_value() || value.prefabInstance->IsValid());
            }, provenance);
        }

        [[nodiscard]] bool IsValidDisposition(const PersistentEntityDisposition disposition) noexcept {
            return disposition == PersistentEntityDisposition::Live || disposition == PersistentEntityDisposition::Tombstone;
        }

        [[nodiscard]] bool IsValidRecord(const PersistentEntityRecord &record) noexcept {
            return record.identity.IsValid() && record.generation.IsValid() && IsValidProvenance(record.provenance) &&
                   IsValidDisposition(record.disposition);
        }

        [[nodiscard]] bool IsValidBindingShape(const PersistentEntityRuntimeBinding &binding, const SceneRuntimeId runtime) noexcept {
            return binding.identity.IsValid() && binding.generation.IsValid() && binding.runtime.IsValid() &&
                   binding.runtime.runtime == runtime;
        }

        [[nodiscard]] std::string IdentityText(const PersistentEntityId &identity) {
            return identity.ToString();
        }
    }  // namespace

    PersistentEntityIdentityMap::PersistentEntityIdentityMap(SceneRuntimeId runtime, std::vector<PersistentEntityRecord> records,
                                                             std::vector<PersistentEntityRuntimeBinding> bindings,
                                                             std::vector<RuntimeIndexEntry> runtimeIndex,
                                                             std::vector<AuthoredIndexEntry> authoredIndex) noexcept
        : runtime_(runtime), records_(std::move(records)), bindings_(std::move(bindings)), runtimeIndex_(std::move(runtimeIndex)),
          authoredIndex_(std::move(authoredIndex)) {}

    /** @copydoc PersistentEntityIdentityMap::ValidateRecords */
    Result<void> PersistentEntityIdentityMap::ValidateRecords(const std::vector<PersistentEntityRecord> &records,
                                                              std::vector<AuthoredIndexEntry> &authoredIndex) {
        for (std::size_t index = 0; index < records.size(); ++index) {
            const auto &record = records[index];
            if (!IsValidRecord(record))
                return Fail<void>(SceneErrors::PersistentIdentityInvalid,
                                  "A persistent entity record has invalid identity, generation, provenance, or state.");
            if (index > 0 && records[index - 1].identity == record.identity)
                return Fail<void>(SceneErrors::PersistentIdentityDuplicate,
                                  std::format("Persistent entity '{}' occurs more than once.", IdentityText(record.identity)));
            if (const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance); authored != nullptr)
                authoredIndex.emplace_back(authored->scene, authored->object, index);
        }
        std::ranges::sort(authoredIndex);
        const auto duplicate =
            std::ranges::adjacent_find(authoredIndex, [](const AuthoredIndexEntry &left, const AuthoredIndexEntry &right) {
            return left.scene == right.scene && left.object == right.object;
        });
        if (duplicate == authoredIndex.end())
            return Result<void>::Success();
        return Fail<void>(SceneErrors::PersistentIdentityDuplicate,
                          std::format("Authored Scene object {}:{} maps to multiple persistent entities.", duplicate->scene.value,
                                      duplicate->object.value));
    }

    /** @copydoc PersistentEntityIdentityMap::ValidateBinding */
    Result<void> PersistentEntityIdentityMap::ValidateBinding(const SceneRuntimeId runtime,
                                                              const std::vector<PersistentEntityRecord> &records,
                                                              const PersistentEntityRuntimeBinding &binding) {
        if (!IsValidBindingShape(binding, runtime))
            return Fail<void>(SceneErrors::PersistentIdentityBindingInvalid,
                              "A persistent entity binding targets an invalid or foreign Scene entity.");
        const auto record = std::ranges::lower_bound(records, binding.identity, {}, &PersistentEntityRecord::identity);
        if (record == records.end() || record->identity != binding.identity)
            return Fail<void>(SceneErrors::PersistentIdentityBindingInvalid, "A runtime binding has no persistent entity record.");
        if (record->generation != binding.generation)
            return Fail<void>(SceneErrors::PersistentIdentityStale,
                              std::format("Persistent entity '{}' binding uses a stale generation.", IdentityText(binding.identity)));
        if (record->disposition == PersistentEntityDisposition::Tombstone)
            return Fail<void>(SceneErrors::PersistentIdentityTombstoned,
                              std::format("Tombstoned persistent entity '{}' cannot receive a runtime binding.",
                                          IdentityText(binding.identity)));
        return Result<void>::Success();
    }

    /** @copydoc PersistentEntityIdentityMap::ValidateBindings */
    Result<void> PersistentEntityIdentityMap::ValidateBindings(const SceneRuntimeId runtime,
                                                               const std::vector<PersistentEntityRecord> &records,
                                                               const std::vector<PersistentEntityRuntimeBinding> &bindings,
                                                               std::vector<RuntimeIndexEntry> &runtimeIndex) {
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto &binding = bindings[index];
            if (index > 0 && bindings[index - 1].identity == binding.identity)
                return Fail<void>(SceneErrors::PersistentIdentityDuplicate,
                                  std::format("Persistent entity '{}' has multiple runtime bindings.", IdentityText(binding.identity)));
            if (auto validated = ValidateBinding(runtime, records, binding); validated.HasError())
                return validated;
            runtimeIndex.emplace_back(binding.runtime, binding.identity);
        }
        std::ranges::sort(runtimeIndex, {}, &RuntimeIndexEntry::runtime);
        if (std::ranges::adjacent_find(runtimeIndex, {}, &RuntimeIndexEntry::runtime) != runtimeIndex.end())
            return Fail<void>(SceneErrors::PersistentIdentityDuplicate, "One runtime entity is bound to multiple persistent identities.");
        for (const auto &record : records) {
            const auto binding = std::ranges::lower_bound(bindings, record.identity, {}, &PersistentEntityRuntimeBinding::identity);
            const bool isBound = binding != bindings.end() && binding->identity == record.identity;
            if (record.disposition == PersistentEntityDisposition::Live && !isBound)
                return Fail<void>(SceneErrors::PersistentIdentityBindingMissing,
                                  std::format("Live persistent entity '{}' has no runtime binding.", IdentityText(record.identity)));
        }
        return Result<void>::Success();
    }

    /** @copydoc PersistentEntityIdentityMap::Create */
    Result<PersistentEntityIdentityMap> PersistentEntityIdentityMap::Create(const SceneRuntimeId runtime,
                                                                            const std::span<const PersistentEntityRecord> records,
                                                                            const std::span<const PersistentEntityRuntimeBinding> bindings,
                                                                            const std::size_t maximumEntries) {
        if (!runtime.IsValid() || maximumEntries == 0 || maximumEntries > MaximumPersistentEntityCount || records.size() > maximumEntries ||
            bindings.size() > maximumEntries)
            return Fail<PersistentEntityIdentityMap>(SceneErrors::PersistentIdentityInvalid,
                                                     "Persistent entity identity bounds or runtime are invalid.");
        try {
            std::vector<PersistentEntityRecord> canonicalRecords{records.begin(), records.end()};
            std::ranges::sort(canonicalRecords, {}, &PersistentEntityRecord::identity);
            std::vector<AuthoredIndexEntry> authoredIndex;
            authoredIndex.reserve(canonicalRecords.size());
            if (auto recordsValidated = ValidateRecords(canonicalRecords, authoredIndex); recordsValidated.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(recordsValidated.ErrorValue());

            std::vector<PersistentEntityRuntimeBinding> canonicalBindings{bindings.begin(), bindings.end()};
            std::ranges::sort(canonicalBindings, {}, &PersistentEntityRuntimeBinding::identity);
            std::vector<RuntimeIndexEntry> runtimeIndex;
            runtimeIndex.reserve(canonicalBindings.size());
            if (auto bindingsValidated = ValidateBindings(runtime, canonicalRecords, canonicalBindings, runtimeIndex);
                bindingsValidated.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(bindingsValidated.ErrorValue());
            return Result<PersistentEntityIdentityMap>::Success(
                PersistentEntityIdentityMap{runtime, std::move(canonicalRecords), std::move(canonicalBindings), std::move(runtimeIndex),
                                            std::move(authoredIndex)});
        } catch (const std::bad_alloc &) {
            return Fail<PersistentEntityIdentityMap>(SceneErrors::PersistentIdentityAllocationFailed);
        }
    }

    /** @copydoc PersistentEntityIdentityMap::Records */
    std::span<const PersistentEntityRecord> PersistentEntityIdentityMap::Records() const noexcept {
        return records_;
    }

    /** @copydoc PersistentEntityIdentityMap::Resolve */
    Result<EntityRef> PersistentEntityIdentityMap::Resolve(const PersistentEntityId identity,
                                                           const PersistentEntityGeneration generation) const {
        if (!identity.IsValid() || !generation.IsValid())
            return Fail<EntityRef>(SceneErrors::PersistentIdentityInvalid);
        const auto record = std::ranges::lower_bound(records_, identity, {}, &PersistentEntityRecord::identity);
        if (record == records_.end() || record->identity != identity)
            return Fail<EntityRef>(SceneErrors::PersistentIdentityUnknown,
                                   std::format("Persistent entity '{}' is not present.", IdentityText(identity)));
        if (record->generation != generation)
            return Fail<EntityRef>(SceneErrors::PersistentIdentityStale,
                                   std::format("Persistent entity '{}' generation is stale.", IdentityText(identity)));
        if (record->disposition == PersistentEntityDisposition::Tombstone)
            return Fail<EntityRef>(SceneErrors::PersistentIdentityTombstoned,
                                   std::format("Persistent entity '{}' is tombstoned.", IdentityText(identity)));
        const auto binding = std::ranges::lower_bound(bindings_, identity, {}, &PersistentEntityRuntimeBinding::identity);
        if (binding == bindings_.end() || binding->identity != identity)
            return Fail<EntityRef>(SceneErrors::PersistentIdentityBindingMissing);
        return Result<EntityRef>::Success(binding->runtime);
    }

    /** @copydoc PersistentEntityIdentityMap::Find */
    Result<PersistentEntityId> PersistentEntityIdentityMap::Find(const EntityRef runtime) const {
        if (!runtime.IsValid() || runtime.runtime != runtime_)
            return Fail<PersistentEntityId>(SceneErrors::PersistentIdentityBindingInvalid);
        const auto found = std::ranges::lower_bound(runtimeIndex_, runtime, {}, &RuntimeIndexEntry::runtime);
        if (found == runtimeIndex_.end() || found->runtime != runtime)
            return Fail<PersistentEntityId>(SceneErrors::PersistentIdentityUnknown);
        return Result<PersistentEntityId>::Success(found->identity);
    }

    /** @copydoc PersistentEntityIdentityMap::FindAuthored */
    const PersistentEntityRecord *PersistentEntityIdentityMap::FindAuthored(const SceneDefinitionId scene,
                                                                            const SceneObjectId object) const noexcept {
        if (!scene.IsValid() || !object.IsValid())
            return nullptr;
        const auto found = std::ranges::lower_bound(authoredIndex_, AuthoredIndexEntry{scene, object, 0});
        if (found == authoredIndex_.end() || found->scene != scene || found->object != object)
            return nullptr;
        return &records_[found->recordIndex];
    }
}  // namespace Horo::Runtime
