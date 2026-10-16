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

        /** @brief Reviews an occurrence before changing the detached value. */
        template <class T>
        void Repair(T &value, const T removed, const std::optional<T> replacement, const Assets::AssetId asset, const std::uint64_t object,
                    const NavigationReferenceField field, std::vector<NavigationReferenceImpact> &impacts, bool &repairable) {
            if (value != removed)
                return;
            impacts.push_back({asset, object, field, removed.Value(), replacement ? std::optional{replacement->Value()} : std::nullopt});
            if (replacement)
                value = *replacement;
            else
                repairable = false;
        }

        /** @brief Applies puts first so explicit replacement identities are validated against the final catalog. */
        void ApplyPuts(NavigationDefinitionInput &input, const std::span<const NavigationDefinitionEdit> edits) {
            for (const auto &edit : edits)
                std::visit([&](const auto &operation) {
                    using T = std::decay_t<decltype(operation)>;
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
            std::erase_if(values, [&](const auto &value) {
                return value.id == id;
            });
            return true;
        }

        /** @brief Projects definition-local references and removes descriptors only in detached storage. */
        Result<void> ApplyRemovals(NavigationDefinitionInput &input, const Assets::AssetId asset,
                                   const std::span<const NavigationDefinitionEdit> edits, std::vector<NavigationReferenceImpact> &impacts,
                                   bool &repairable) {
            for (const auto &edit : edits) {
                bool valid = true;
                std::visit([&](const auto &operation) {
                    using T = std::decay_t<decltype(operation)>;
                    if constexpr (std::is_same_v<T, RemoveNavigationProfile>)
                        valid = Remove(input.profiles, operation.id, operation.replacement);
                    else if constexpr (std::is_same_v<T, RemoveNavigationFilter>)
                        valid = Remove(input.filters, operation.id, operation.replacement);
                    else if constexpr (std::is_same_v<T, RemoveNavigationArea>) {
                        valid = Remove(input.areas, operation.id, operation.replacement);
                        for (auto &filter : input.filters)
                            for (auto &cost : filter.costOverrides) {
                                if (cost.area == operation.id && filter.source.kind == NavigationDescriptorSourceKind::Package) {
                                    impacts.push_back({asset, filter.id.Value(), NavigationReferenceField::FilterAreaCost,
                                                       operation.id.Value(), std::nullopt});
                                    repairable = false;  // Project gestures cannot rewrite contributed package authority.
                                } else
                                    Repair(cost.area, operation.id, operation.replacement, asset, filter.id.Value(),
                                           NavigationReferenceField::FilterAreaCost, impacts, repairable);
                            }
                    }
                }, edit);
                if (!valid)
                    return Result<void>::Failure(Reject(NavigationSettingsFailure::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Preserves package provenance and rejects unreviewed project identity replacement. */
        Result<void> ValidateEditable(const NavigationDefinitionInput &input, const std::span<const NavigationDefinitionEdit> edits) {
            // Package-contributed descriptors are not project-editable and retain their exact provenance.
            for (const auto &edit : edits) {
                bool permitted = true;
                std::visit([&](const auto &operation) {
                    using T = std::decay_t<decltype(operation)>;
                    if constexpr (std::is_same_v<T, PutNavigationArea>) {
                        const auto prior = std::ranges::find(input.areas, operation.value.id, &NavigationAreaDescriptor::id);
                        permitted = operation.value.source.kind == NavigationDescriptorSourceKind::Project &&
                                    (prior == input.areas.end() || prior->source == operation.value.source);
                    } else if constexpr (std::is_same_v<T, PutNavigationFilter>) {
                        const auto prior = std::ranges::find(input.filters, operation.value.id, &NavigationQueryFilterDescriptor::id);
                        permitted = operation.value.source.kind == NavigationDescriptorSourceKind::Project &&
                                    (prior == input.filters.end() || prior->source == operation.value.source);
                    } else if constexpr (std::is_same_v<T, RemoveNavigationArea>) {
                        const auto prior = std::ranges::find(input.areas, operation.id, &NavigationAreaDescriptor::id);
                        permitted = prior != input.areas.end() && prior->source.kind == NavigationDescriptorSourceKind::Project;
                    } else if constexpr (std::is_same_v<T, RemoveNavigationFilter>) {
                        const auto prior = std::ranges::find(input.filters, operation.id, &NavigationQueryFilterDescriptor::id);
                        permitted = prior != input.filters.end() && prior->source.kind == NavigationDescriptorSourceKind::Project;
                    }
                }, edit);
                if (!permitted)
                    return Result<void>::Failure(Reject(NavigationSettingsFailure::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves a surface through the exact captured Scene, not ordering or the current UI selection. */
        bool SelectedSurface(const SceneTransactionSnapshot &scene, const Navigation::SurfaceId id, const Assets::AssetId definition) {
            return std::ranges::any_of(scene.objects, [&](const auto &object) {
                const auto &surface = object.components.navigationSurface;
                return surface && surface->id == id && surface->definition == definition;
            });
        }

        /** @brief Determines agent catalog ownership; ambiguity cannot authorize a cross-asset repair. */
        Result<bool> SelectedAgent(const Runtime::NavigationAgentComponent &agent, const Assets::AssetId selected,
                                   const std::span<const NavigationDefinitionSnapshot> definitions) {
            std::optional<Assets::AssetId> owner;
            for (const auto &definition : definitions) {
                const auto &source = *definition.definition;
                if (std::ranges::find(source.Profiles(), agent.profile, &NavigationAgentProfileDescriptor::id) == source.Profiles().end() ||
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

        /** @brief Repairs one detached object's exact navigation fields while retaining all unrelated authored content. */
        Result<void> RepairObject(SceneSource::SceneObjectSnapshot &object, const SceneTransactionSnapshot &scene,
                                  const Assets::AssetId definition, const std::span<const NavigationDefinitionSnapshot> definitions,
                                  const std::span<const NavigationDefinitionEdit> edits, std::vector<NavigationReferenceImpact> &impacts,
                                  bool &repairable) {
            auto &c = object.components;
            for (const auto &edit : edits) {
                if (const auto *remove = std::get_if<RemoveNavigationProfile>(&edit)) {
                    if (c.navigationSurface && c.navigationSurface->definition == definition)
                        for (auto &id : c.navigationSurface->profiles)
                            Repair(id, remove->id, remove->replacement, scene.session.owner, object.id.value,
                                   NavigationReferenceField::SurfaceProfile, impacts, repairable);
                    if (c.navigationLink && SelectedSurface(scene, c.navigationLink->start.surface, definition) !=
                                                SelectedSurface(scene, c.navigationLink->end.surface, definition))
                        return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
                    if (c.navigationLink && SelectedSurface(scene, c.navigationLink->start.surface, definition))
                        for (auto &id : c.navigationLink->profiles)
                            Repair(id, remove->id, remove->replacement, scene.session.owner, object.id.value,
                                   NavigationReferenceField::LinkProfile, impacts, repairable);
                }
                if (const auto *remove = std::get_if<RemoveNavigationArea>(&edit);
                    remove && c.navigationModifier && c.navigationModifier->area &&
                    SelectedSurface(scene, c.navigationModifier->surface, definition))
                    Repair(*c.navigationModifier->area, remove->id, remove->replacement, scene.session.owner, object.id.value,
                           NavigationReferenceField::ModifierArea, impacts, repairable);
                if (!c.navigationAgent)
                    continue;
                const auto *profile = std::get_if<RemoveNavigationProfile>(&edit);
                const auto *filter = std::get_if<RemoveNavigationFilter>(&edit);
                if ((!profile || c.navigationAgent->profile != profile->id) && (!filter || c.navigationAgent->filter != filter->id))
                    continue;
                auto selected = SelectedAgent(*c.navigationAgent, definition, definitions);
                if (selected.HasError())
                    return Result<void>::Failure(selected.ErrorValue());
                if (!selected.Value())
                    continue;
                if (profile)
                    Repair(c.navigationAgent->profile, profile->id, profile->replacement, scene.session.owner, object.id.value,
                           NavigationReferenceField::AgentProfile, impacts, repairable);
                if (filter)
                    Repair(c.navigationAgent->filter, filter->id, filter->replacement, scene.session.owner, object.id.value,
                           NavigationReferenceField::AgentFilter, impacts, repairable);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationSettingsWorkflow::NavigationSettingsWorkflow */
    NavigationSettingsWorkflow::NavigationSettingsWorkflow(const Assets::AssetRegistry &registry,
                                                           const std::span<NavigationDefinitionDocument *const> definitions,
                                                           const std::span<SceneDocumentTransactions *const> scenes)
        : registry_(registry) {
        for (auto *owner : definitions)
            definitions_.push_back(owner ? owner->lifetime_ : std::weak_ptr<NavigationDefinitionDocument::Lifetime>{});
        for (auto *owner : scenes)
            scenes_.push_back(owner ? owner->lifetime_ : std::weak_ptr<SceneDocumentTransactions::Lifetime>{});
    }

    /** @copydoc NavigationSettingsWorkflow::CaptureInventory */
    Result<void> NavigationSettingsWorkflow::CaptureInventory(NavigationSettingsReview &review, const Assets::AssetId definition,
                                                              const bool destructive) const {
        const auto registry = registry_.Snapshot();
        review.registryRevision_ = registry.Revision();
        if (!registry.Revision().value)
            return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
        std::set<Assets::AssetId> assets;
        for (const auto &handle : definitions_) {
            const auto lifetime = handle.lock();
            const auto *owner = lifetime ? lifetime->owner : nullptr;
            if (!owner)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            auto captured = owner->Snapshot();
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            if (!assets.insert(captured.Value().asset).second)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
            const auto *record = registry.Find(captured.Value().asset);
            if (!record || record->type.Value() != Assets::NavMeshAssetTypeName)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::IncompleteInventory));
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
            auto *owner = lifetime ? lifetime->owner : nullptr;
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
                                                                         const std::span<const NavigationDefinitionEdit> edits) const try {
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
                if (auto repaired =
                        RepairObject(after, scene.snapshot, definition, review.definitions_, edits, review.impacts_, review.repairable_);
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

}  // namespace Horo::Editor
