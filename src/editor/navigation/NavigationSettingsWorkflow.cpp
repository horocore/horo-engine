#include "Horo/Editor/NavigationSettingsWorkflow.h"

#include "Horo/Assets/NavMeshAssetType.h"

#include <algorithm>
#include <new>
#include <set>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        using namespace Navigation;

        /** @brief Returns a stable settings failure. */
        Error Reject(const NavigationSettingsFailure reason) {
            return MakeError(NavigationSettingsError(reason));
        }

        /** @brief Updates one complete descriptor under its stable identity. */
        template <class T> void Put(std::vector<T> &values, const T &value) {
            const auto found = std::ranges::find(values, value.id, &T::id);
            if (found == values.end())
                values.push_back(value);
            else
                *found = value;
        }

        /** @brief Stack-only report sink; the caller retains every owner and preparation buffer. */
        struct ReferenceReview final {
            Assets::AssetId asset;
            std::uint64_t object{};
            std::vector<NavigationReferenceImpact> &impacts;
            bool &repairable;
        };

        /** @brief Reviews an occurrence before changing the detached value. */
        template <class T>
        void Repair(T &value, const T removed, const std::optional<T> replacement, const NavigationReferenceField field,
                    const ReferenceReview &review) {
            if (value != removed)
                return;
            review.impacts.emplace_back(review.asset, review.object, field, removed.Value(),
                                        replacement ? std::optional{replacement->Value()} : std::nullopt);
            if (replacement)
                value = *replacement;
            else
                review.repairable = false;
        }

        /** @brief Applies puts first so explicit replacement identities are validated against the final catalog. */
        void ApplyPuts(NavigationDefinitionInput &input, const std::span<const NavigationDefinitionEdit> edits) {
            for (const auto &edit : edits)
                std::visit([&input]<typename T>(const T &operation) {
                    if constexpr (std::is_same_v<T, PutNavigationProfile>)
                        Put(input.profiles, operation.value);
                    else if constexpr (std::is_same_v<T, PutNavigationArea>)
                        Put(input.areas, operation.value);
                    else if constexpr (std::is_same_v<T, PutNavigationFilter>)
                        Put(input.filters, operation.value);
                }, edit);
        }

        /** @brief Validates explicit deletion/replacement identities before removing a descriptor. */
        template <class T, class Id> bool Remove(std::vector<T> &values, const Id id, const std::optional<Id> replacement) {
            if (!id.IsValid() || std::ranges::find(values, id, &T::id) == values.end() ||
                (replacement && (*replacement == id || std::ranges::find(values, *replacement, &T::id) == values.end())))
                return false;
            std::erase_if(values, [id](const auto &value) {
                return value.id == id;
            });
            return true;
        }

        /** @brief Repairs one filter's local cost references while preserving package-contributed authority. */
        void RepairFilterCosts(NavigationQueryFilterDescriptor &filter, const RemoveNavigationArea &operation, const Assets::AssetId asset,
                               std::vector<NavigationReferenceImpact> &impacts, bool &repairable) {
            const ReferenceReview review{asset, filter.id.Value(), impacts, repairable};
            for (auto &cost : filter.costOverrides) {
                if (cost.area == operation.id && filter.source.kind == NavigationDescriptorSourceKind::Package) {
                    impacts.emplace_back(asset, filter.id.Value(), NavigationReferenceField::FilterAreaCost, operation.id.Value(),
                                         std::nullopt);
                    repairable = false;
                } else
                    Repair(cost.area, operation.id, operation.replacement, NavigationReferenceField::FilterAreaCost, review);
            }
        }

        /** @brief Projects definition-local references and removes descriptors only in detached storage. */
        Result<void> ApplyRemovals(NavigationDefinitionInput &input, const Assets::AssetId asset,
                                   const std::span<const NavigationDefinitionEdit> edits, std::vector<NavigationReferenceImpact> &impacts,
                                   bool &repairable) {
            for (const auto &edit : edits) {
                bool valid = true;
                std::visit([&input, asset, &impacts, &repairable, &valid]<typename T>(const T &operation) {
                    if constexpr (std::is_same_v<T, RemoveNavigationProfile>)
                        valid = Remove(input.profiles, operation.id, operation.replacement);
                    else if constexpr (std::is_same_v<T, RemoveNavigationFilter>)
                        valid = Remove(input.filters, operation.id, operation.replacement);
                    else if constexpr (std::is_same_v<T, RemoveNavigationArea>) {
                        valid = Remove(input.areas, operation.id, operation.replacement);
                        for (auto &filter : input.filters)
                            RepairFilterCosts(filter, operation, asset, impacts, repairable);
                    }
                }, edit);
                if (!valid)
                    return Result<void>::Failure(Reject(NavigationSettingsFailure::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Preserves package provenance and rejects unreviewed project identity replacement. */
        Result<void> ValidateEditable(const NavigationDefinitionInput &input, const std::span<const NavigationDefinitionEdit> edits) {
            using enum NavigationDescriptorSourceKind;
            // Package-contributed descriptors are not project-editable and retain their exact provenance.
            for (const auto &edit : edits) {
                bool permitted = true;
                std::visit([&input, &permitted]<typename T>(const T &operation) {
                    if constexpr (std::is_same_v<T, PutNavigationArea>) {
                        const auto prior = std::ranges::find(input.areas, operation.value.id, &NavigationAreaDescriptor::id);
                        permitted = operation.value.source.kind == Project &&
                                    (prior == input.areas.end() || prior->source == operation.value.source);
                    } else if constexpr (std::is_same_v<T, PutNavigationFilter>) {
                        const auto prior = std::ranges::find(input.filters, operation.value.id, &NavigationQueryFilterDescriptor::id);
                        permitted = operation.value.source.kind == Project &&
                                    (prior == input.filters.end() || prior->source == operation.value.source);
                    } else if constexpr (std::is_same_v<T, RemoveNavigationArea>) {
                        const auto prior = std::ranges::find(input.areas, operation.id, &NavigationAreaDescriptor::id);
                        permitted = prior != input.areas.end() && prior->source.kind == Project;
                    } else if constexpr (std::is_same_v<T, RemoveNavigationFilter>) {
                        const auto prior = std::ranges::find(input.filters, operation.id, &NavigationQueryFilterDescriptor::id);
                        permitted = prior != input.filters.end() && prior->source.kind == Project;
                    }
                }, edit);
                if (!permitted)
                    return Result<void>::Failure(Reject(NavigationSettingsFailure::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves a surface through the exact captured Scene, not ordering or the current UI selection. */
        bool SelectedSurface(const SceneTransactionSnapshot &scene, const Navigation::SurfaceId id, const Assets::AssetId definition) {
            return std::ranges::any_of(scene.objects, [id, definition](const auto &object) {
                const auto &surface = object.components.navigationSurface;
                return surface && surface->id == id && surface->definition == definition;
            });
        }

        /** @brief Determines agent catalog ownership; ambiguity cannot authorize a cross-asset repair. */
        Result<bool> SelectedAgent(const Runtime::NavigationAgentComponent &agent, const Assets::AssetId selected,
                                   const std::span<const NavigationDefinitionSnapshot> definitions) {
            std::optional<Assets::AssetId> owner;
            for (const auto &definition : definitions) {
                if (const auto &source = *definition.definition;
                    std::ranges::find(source.Profiles(), agent.profile, &NavigationAgentProfileDescriptor::id) == source.Profiles().end() ||
                    source.Registry().ResolveFilter(agent.filter).HasError())
                    continue;
                if (owner)
                    return Result<bool>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
                owner = definition.asset;
            }
            if (!owner)
                return Result<bool>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            return Result<bool>::Success(*owner == selected);
        }

        /** @brief Repairs surface and link profiles in the same original order, retaining cross-definition ambiguity checks. */
        Result<void> RepairProfiles(SceneSource::SceneObjectComponentSet &components, const SceneTransactionSnapshot &scene,
                                    const Assets::AssetId definition, const RemoveNavigationProfile &remove,
                                    const ReferenceReview &review) {
            if (components.navigationSurface && components.navigationSurface->definition == definition)
                for (auto &id : components.navigationSurface->profiles)
                    Repair(id, remove.id, remove.replacement, NavigationReferenceField::SurfaceProfile, review);
            if (!components.navigationLink)
                return Result<void>::Success();
            const auto &link = *components.navigationLink;
            const bool start = SelectedSurface(scene, link.start.surface, definition);
            if (const bool end = SelectedSurface(scene, link.end.surface, definition); start != end)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            if (start)
                for (auto &id : components.navigationLink->profiles)
                    Repair(id, remove.id, remove.replacement, NavigationReferenceField::LinkProfile, review);
            return Result<void>::Success();
        }

        /** @brief Repairs an agent only after resolving its exact unambiguous captured definition owner. */
        Result<void> RepairAgent(Runtime::NavigationAgentComponent &agent, const NavigationDefinitionEdit &edit,
                                 const Assets::AssetId definition, const std::span<const NavigationDefinitionSnapshot> definitions,
                                 const ReferenceReview &review) {
            const auto *profile = std::get_if<RemoveNavigationProfile>(&edit);
            const auto *filter = std::get_if<RemoveNavigationFilter>(&edit);
            if ((!profile || agent.profile != profile->id) && (!filter || agent.filter != filter->id))
                return Result<void>::Success();
            const auto selected = SelectedAgent(agent, definition, definitions);
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            if (!selected.Value())
                return Result<void>::Success();
            if (profile)
                Repair(agent.profile, profile->id, profile->replacement, NavigationReferenceField::AgentProfile, review);
            if (filter)
                Repair(agent.filter, filter->id, filter->replacement, NavigationReferenceField::AgentFilter, review);
            return Result<void>::Success();
        }

        /** @brief Repairs one detached object's exact navigation fields while retaining all unrelated authored content. */
        Result<void> RepairObject(SceneSource::SceneObjectSnapshot &object, const SceneTransactionSnapshot &scene,
                                  const Assets::AssetId definition, const std::span<const NavigationDefinitionSnapshot> definitions,
                                  const std::span<const NavigationDefinitionEdit> edits, std::vector<NavigationReferenceImpact> &impacts,
                                  bool &repairable) {
            auto &components = object.components;
            const ReferenceReview review{scene.session.owner, object.id.value, impacts, repairable};
            for (const auto &edit : edits) {
                if (const auto *remove = std::get_if<RemoveNavigationProfile>(&edit))
                    if (auto valid = RepairProfiles(components, scene, definition, *remove, review); valid.HasError())
                        return valid;
                if (const auto *remove = std::get_if<RemoveNavigationArea>(&edit);
                    remove && components.navigationModifier && components.navigationModifier->area &&
                    SelectedSurface(scene, components.navigationModifier->surface, definition))
                    Repair(*components.navigationModifier->area, remove->id, remove->replacement, NavigationReferenceField::ModifierArea,
                           review);
                if (components.navigationAgent)
                    if (auto valid = RepairAgent(*components.navigationAgent, edit, definition, definitions, review); valid.HasError())
                        return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationSettingsWorkflow::NavigationSettingsWorkflow */
    NavigationSettingsWorkflow::NavigationSettingsWorkflow(const Assets::AssetRegistry &registry,
                                                           const std::span<NavigationDefinitionDocument *const> definitions,
                                                           const std::span<SceneDocumentTransactions *const> scenes)
        : registry_(registry) {
        for (const auto *owner : definitions)
            definitions_.push_back(owner ? owner->lifetime_ : std::weak_ptr<NavigationDefinitionDocument::Lifetime>{});
        for (const auto *owner : scenes)
            scenes_.push_back(owner ? owner->lifetime_ : std::weak_ptr<SceneDocumentTransactions::Lifetime>{});
    }

    /** @copydoc NavigationSettingsWorkflow::CaptureInventory */
    Result<void> NavigationSettingsWorkflow::CaptureInventory(NavigationSettingsReview &review, const Assets::AssetId definition,
                                                              const bool destructive) const {
        using enum NavigationSettingsFailure;
        const auto registry = registry_.Snapshot();
        review.registryRevision_ = registry.Revision();
        if (!registry.Revision().value)
            return Result<void>::Failure(Reject(IncompleteInventory));
        std::set<Assets::AssetId> assets;
        for (const auto &handle : definitions_) {
            const auto lifetime = handle.lock();
            const auto *owner = lifetime ? lifetime->owner : nullptr;
            if (!owner)
                return Result<void>::Failure(Reject(IncompleteInventory));
            auto captured = owner->Snapshot();
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            if (!assets.insert(captured.Value().asset).second)
                return Result<void>::Failure(Reject(IncompleteInventory));
            if (const auto *record = registry.Find(captured.Value().asset); !record || record->type.Value() != Assets::NavMeshAssetTypeName)
                return Result<void>::Failure(Reject(IncompleteInventory));
            if (captured.Value().asset == definition)
                review.definition_ = captured.Value();
            review.definitions_.push_back(std::move(captured).Value());
        }
        return CaptureSceneOwners(review, registry, destructive);
    }

    /** @copydoc NavigationSettingsWorkflow::CaptureSceneOwners */
    Result<void> NavigationSettingsWorkflow::CaptureSceneOwners(NavigationSettingsReview &review,
                                                                const Assets::AssetRegistrySnapshot &registry,
                                                                const bool destructive) const {
        std::set<Assets::AssetId> assets;
        for (const auto &definition : review.definitions_)
            assets.insert(definition.asset);
        for (const auto &handle : scenes_) {
            const auto lifetime = handle.lock();
            const auto *owner = lifetime ? lifetime->owner : nullptr;
            if (!owner)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            auto captured = owner->Snapshot();
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            const auto *record = registry.Find(captured.Value().session.owner);
            if (!record || record->type.Value() != "core.scene" || !assets.insert(record->id).second)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            if (destructive && !captured.Value().prefabInstances.empty()) {
                review.unsupportedOwners_.push_back({record->id, NavigationUnsupportedOwnerKind::ScenePrefabInstances});
                review.repairable_ = false;
            }
            review.scenes_.push_back({handle, std::move(captured).Value(), {}});
        }
        // Prefab source documents may contain the same typed navigation fields. An unopened prefab owner is never treated as zero
        // references.
        for (const auto &record : registry.Records())
            if (destructive && record.type.Value() == "core.prefab") {
                review.unsupportedOwners_.push_back({record.id, NavigationUnsupportedOwnerKind::PrefabSource});
                review.repairable_ = false;
            }
        for (const auto &record : registry.Records())
            if ((record.type.Value() == "core.scene" || record.type.Value() == Assets::NavMeshAssetTypeName) && !assets.contains(record.id))
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
        if (!review.definition_.definition)
            return Result<void>::Failure(Reject(NavigationSettingsFailure::Invalid));
        return Result<void>::Success();
    }

    /** @copydoc NavigationSettingsWorkflow::Preview */
    Result<NavigationSettingsReview> NavigationSettingsWorkflow::Preview(const Assets::AssetId definition,
                                                                         const std::span<const NavigationDefinitionEdit> edits) const {
        try {
            using Return = Result<NavigationSettingsReview>;
            if (edits.empty() || edits.size() > 64 || definitions_.size() > 256 || scenes_.size() > 256)
                return Return::Failure(Reject(NavigationSettingsFailure::Capacity));
            const bool destructive = std::ranges::any_of(edits, [](const auto &edit) {
                return std::holds_alternative<RemoveNavigationProfile>(edit) || std::holds_alternative<RemoveNavigationArea>(edit) ||
                       std::holds_alternative<RemoveNavigationFilter>(edit);
            });
            NavigationSettingsReview review;
            review.workflow_ = this;
            if (auto inventory = CaptureInventory(review, definition, destructive); inventory.HasError())
                return Return::Failure(inventory.ErrorValue());
            review.candidate_ = CopyNavigationDefinition(*review.definition_.definition);
            if (auto permitted = ValidateEditable(review.candidate_, edits); permitted.HasError())
                return Return::Failure(permitted.ErrorValue());
            ApplyPuts(review.candidate_, edits);
            if (auto removed = ApplyRemovals(review.candidate_, definition, edits, review.impacts_, review.repairable_); removed.HasError())
                return Return::Failure(removed.ErrorValue());
            for (auto &scene : review.scenes_)
                for (const auto &before : scene.snapshot.objects) {
                    auto after = before;
                    if (auto repaired = RepairObject(after, scene.snapshot, definition, review.definitions_, edits, review.impacts_,
                                                     review.repairable_);
                        repaired.HasError())
                        return Return::Failure(repaired.ErrorValue());
                    if (review.impacts_.size() > 16'384)
                        return Return::Failure(Reject(NavigationSettingsFailure::Capacity));
                    if (after.components == before.components)
                        continue;
                    auto fingerprint = FingerprintSceneObjectContent(before, 16 * 1024 * 1024);
                    if (fingerprint.HasError())
                        return Return::Failure(fingerprint.ErrorValue());
                    scene.changes.push_back({before.id, fingerprint.Value(), before.generated, std::move(after), {}});
                }
            if (review.repairable_) {
                auto valid = NavigationDefinition::Create(review.candidate_);
                if (valid.HasError())
                    return Return::Failure(valid.ErrorValue());
            }
            return Return::Success(std::move(review));
        } catch (const std::bad_alloc &) {
            return Result<NavigationSettingsReview>::Failure(Reject(NavigationSettingsFailure::Capacity));
        }
    }

}  // namespace Horo::Editor
