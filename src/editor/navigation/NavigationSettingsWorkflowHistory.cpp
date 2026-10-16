#include "Horo/Editor/NavigationSettingsWorkflow.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Navigation;

        /** @brief Preserves typed history admission failures at the owner boundary. */
        Error Reject(const NavigationSettingsFailure reason) {
            return MakeError(NavigationSettingsError(reason));
        }
    }  // namespace

    struct NavigationSettingsWorkflow::ScenePreparation final {
        std::vector<std::shared_ptr<SceneDocumentTransactions::Prepared>> scenes;
        std::vector<SceneDocumentTransactions *> owners;
        std::vector<SceneFence> nextScenes;
        Group group;
    };

    /** @copydoc NavigationSettingsWorkflow::FindDefinition */
    Result<NavigationDefinitionDocument *> NavigationSettingsWorkflow::FindDefinition(const NavigationSettingsReview &review) const {
        NavigationDefinitionDocument *definition{};
        for (const auto &handle : definitions_) {
            const auto lifetime = handle.lock();
            auto *owner = lifetime ? lifetime->owner : nullptr;
            if (!owner)
                return Result<NavigationDefinitionDocument *>::Failure(Reject(NavigationSettingsFailure::Closed));
            const auto expected = std::ranges::find(review.definitions_, owner->asset_, &NavigationDefinitionSnapshot::asset);
            if (expected == review.definitions_.end())
                return Result<NavigationDefinitionDocument *>::Failure(Reject(NavigationSettingsFailure::Stale));
            if (auto valid = owner->Admit(*expected); valid.HasError())
                return Result<NavigationDefinitionDocument *>::Failure(valid.ErrorValue());
            if (!expectedDefinitions_.empty()) {
                const auto prior = std::ranges::find(expectedDefinitions_, owner->asset_, &NavigationDefinitionSnapshot::asset);
                if (prior == expectedDefinitions_.end())
                    return Result<NavigationDefinitionDocument *>::Failure(Reject(NavigationSettingsFailure::Stale));
                if (auto valid = owner->Admit(*prior); valid.HasError())
                    return Result<NavigationDefinitionDocument *>::Failure(valid.ErrorValue());
            }
            if (owner->asset_ == review.definition_.asset)
                definition = owner;
        }
        if (!definition)
            return Result<NavigationDefinitionDocument *>::Failure(Reject(NavigationSettingsFailure::Stale));
        return Result<NavigationDefinitionDocument *>::Success(definition);
    }

    /** @copydoc NavigationSettingsWorkflow::PrepareSceneCommit */
    Result<std::unique_ptr<NavigationSettingsWorkflow::ScenePreparation>> NavigationSettingsWorkflow::PrepareSceneCommit(
        const NavigationSettingsReview &review, const bool canEdit, const CancellationToken &cancellation, const bool coordinated) const {
        using Return = Result<std::unique_ptr<ScenePreparation>>;
        auto stage = std::make_unique<ScenePreparation>();
        for (std::size_t index = 0; index < review.scenes_.size(); ++index) {
            const auto &scene = review.scenes_[index];
            const auto lifetime = scene.owner.lock();
            auto *owner = lifetime ? lifetime->owner : nullptr;
            if (!owner)
                return Return::Failure(Reject(NavigationSettingsFailure::Closed));
            const SceneTransactionContext context{scene.snapshot.session, scene.snapshot.revision, scene.snapshot.state, canEdit,
                                                  cancellation};
            if (!expectedScenes_.empty() &&
                (index >= expectedScenes_.size() || expectedScenes_[index].context.session != context.session ||
                 expectedScenes_[index].context.revision != context.revision || expectedScenes_[index].context.state != context.state))
                return Return::Failure(Reject(NavigationSettingsFailure::Stale));
            auto prepared = owner->Prepare(context, scene.changes, {}, coordinated);
            if (prepared.HasError())
                return Return::Failure(prepared.ErrorValue());
            stage->nextScenes.emplace_back(scene.owner, owner->PreparedContext(*prepared.Value()));
            stage->owners.push_back(owner);
            stage->scenes.push_back(std::move(prepared).Value());
            if (!scene.changes.empty())
                stage->group.changedScenes.push_back(index);
        }
        return Return::Success(std::move(stage));
    }

    /** @copydoc NavigationSettingsWorkflow::Commit */
    Result<void> NavigationSettingsWorkflow::Commit(const NavigationSettingsReview &review, const bool canEdit,
                                                    const CancellationToken &cancellation) {
        try {
            if (review.workflow_ != this || !review.repairable_ || !canEdit || cancellation.IsCancellationRequested())
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Referenced));
            if (groups_.size() >= 64)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Capacity));
            if (registry_.Snapshot().Revision() != review.registryRevision_)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Stale));
            auto selected = FindDefinition(review);
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            auto *definition = selected.Value();
            const bool coordinated = std::ranges::any_of(review.scenes_, [](const auto &scene) {
                return !scene.changes.empty();
            });
            auto preparedDefinitionResult = definition->Prepare(review.definition_, review.candidate_, coordinated);
            if (preparedDefinitionResult.HasError())
                return Result<void>::Failure(preparedDefinitionResult.ErrorValue());
            auto preparedDefinition = std::move(preparedDefinitionResult).Value();
            auto reservedScenes = PrepareSceneCommit(review, canEdit, cancellation, coordinated);
            if (reservedScenes.HasError())
                return Result<void>::Failure(reservedScenes.ErrorValue());
            auto stage = std::move(reservedScenes).Value();
            stage->group.definition = definition->lifetime_;
            auto nextDefinitions = review.definitions_;
            auto target = std::ranges::find(nextDefinitions, definition->asset_, &NavigationDefinitionSnapshot::asset);
            target->definition = preparedDefinition.next.source;
            target->state = preparedDefinition.next.state;
            ++target->revision;
            auto history = groups_;
            history.resize(cursor_);
            history.push_back(std::move(stage->group));
            // All histories, immutable roots and result buffers now exist. No callbacks/notifications occur during final swaps.
            if (registry_.Snapshot().Revision() != review.registryRevision_ || cancellation.IsCancellationRequested())
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Stale));
            for (std::size_t index = 0; index < stage->scenes.size(); ++index)
                if (auto valid = stage->owners[index]->ValidatePrepared(*stage->scenes[index]); valid.HasError())
                    return valid;
            if (auto admitted = FindDefinition(review); admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            PublishCommit(*definition, preparedDefinition, *stage, nextDefinitions, history, review.registryRevision_);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(Reject(NavigationSettingsFailure::Capacity));
        }
    }

    /** @copydoc NavigationSettingsWorkflow::PublishCommit */
    void NavigationSettingsWorkflow::PublishCommit(NavigationDefinitionDocument &definition,
                                                   NavigationDefinitionDocument::Prepared &preparedDefinition, ScenePreparation &stage,
                                                   std::vector<NavigationDefinitionSnapshot> &nextDefinitions, std::vector<Group> &history,
                                                   const Assets::AssetRegistryRevision registry) noexcept {
        for (std::size_t index = 0; index < stage.scenes.size(); ++index)
            static_cast<void>(stage.owners[index]->PublishPrepared(*stage.scenes[index]));
        definition.Publish(preparedDefinition);
        groups_.swap(history);
        expectedDefinitions_.swap(nextDefinitions);
        expectedScenes_.swap(stage.nextScenes);
        expectedRegistry_ = registry;
        cursor_ = groups_.size();
    }

    /** @copydoc NavigationSettingsWorkflow::PrepareSceneReplay */
    Result<std::unique_ptr<NavigationSettingsWorkflow::ScenePreparation>> NavigationSettingsWorkflow::PrepareSceneReplay(
        const Group &group, const CancellationToken &cancellation, const bool redo) const {
        using Return = Result<std::unique_ptr<ScenePreparation>>;
        auto stage = std::make_unique<ScenePreparation>();
        for (std::size_t index = 0; index < expectedScenes_.size(); ++index) {
            const auto &fence = expectedScenes_[index];
            const auto lease = fence.owner.lock();
            auto *owner = lease ? lease->owner : nullptr;
            if (!owner)
                return Return::Failure(Reject(NavigationSettingsFailure::Closed));
            auto context = fence.context;
            context.cancellation = cancellation;
            const bool changed = std::ranges::find(group.changedScenes, index) != group.changedScenes.end();
            auto prepared = changed ? owner->PrepareReplay(context, redo) : owner->Prepare(context, {});
            if (prepared.HasError())
                return Return::Failure(prepared.ErrorValue());
            stage->nextScenes.emplace_back(fence.owner, owner->PreparedContext(*prepared.Value()));
            stage->owners.push_back(owner);
            stage->scenes.push_back(std::move(prepared).Value());
        }
        return Return::Success(std::move(stage));
    }

    /** @copydoc NavigationSettingsWorkflow::ValidateReplayDefinitions */
    Result<void> NavigationSettingsWorkflow::ValidateReplayDefinitions() const {
        for (const auto &handle : definitions_) {
            const auto lease = handle.lock();
            const auto *owner = lease ? lease->owner : nullptr;
            if (!owner)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Closed));
            const auto expected = std::ranges::find(expectedDefinitions_, owner->asset_, &NavigationDefinitionSnapshot::asset);
            if (expected == expectedDefinitions_.end())
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Stale));
            if (auto valid = owner->Admit(*expected); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationSettingsWorkflow::Replay */
    Result<void> NavigationSettingsWorkflow::Replay(const bool redo, const CancellationToken &cancellation) {
        try {
            if ((redo ? cursor_ == groups_.size() : cursor_ == 0) || cancellation.IsCancellationRequested() ||
                registry_.Snapshot().Revision() != expectedRegistry_)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Stale));
            const auto &group = groups_[redo ? cursor_ : cursor_ - 1];
            const auto lifetime = group.definition.lock();
            auto *definition = lifetime ? lifetime->owner : nullptr;
            if (!definition)
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Closed));
            if (auto valid = ValidateReplayDefinitions(); valid.HasError())
                return valid;
            const auto expected = std::ranges::find(expectedDefinitions_, definition->asset_, &NavigationDefinitionSnapshot::asset);
            auto preparedDefinitionResult = definition->PrepareReplay(*expected, redo);
            if (preparedDefinitionResult.HasError())
                return Result<void>::Failure(preparedDefinitionResult.ErrorValue());
            auto preparedDefinition = std::move(preparedDefinitionResult).Value();
            auto reservedScenes = PrepareSceneReplay(group, cancellation, redo);
            if (reservedScenes.HasError())
                return Result<void>::Failure(reservedScenes.ErrorValue());
            auto stage = std::move(reservedScenes).Value();
            auto nextDefinitions = expectedDefinitions_;
            auto target = std::ranges::find(nextDefinitions, definition->asset_, &NavigationDefinitionSnapshot::asset);
            target->definition = preparedDefinition.next.source;
            target->state = preparedDefinition.next.state;
            ++target->revision;
            if (registry_.Snapshot().Revision() != expectedRegistry_ || cancellation.IsCancellationRequested())
                return Result<void>::Failure(Reject(NavigationSettingsFailure::Stale));
            for (std::size_t index = 0; index < stage->scenes.size(); ++index)
                if (auto valid = stage->owners[index]->ValidatePrepared(*stage->scenes[index]); valid.HasError())
                    return valid;
            if (auto valid = ValidateReplayDefinitions(); valid.HasError())
                return valid;
            for (std::size_t index = 0; index < stage->scenes.size(); ++index)
                static_cast<void>(stage->owners[index]->PublishPrepared(*stage->scenes[index]));
            definition->Publish(preparedDefinition);
            expectedDefinitions_.swap(nextDefinitions);
            expectedScenes_.swap(stage->nextScenes);
            if (redo)
                ++cursor_;
            else
                --cursor_;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(Reject(NavigationSettingsFailure::Capacity));
        }
    }

}  // namespace Horo::Editor
