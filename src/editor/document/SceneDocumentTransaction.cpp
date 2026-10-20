#include "Horo/Editor/SceneDocumentTransaction.h"

#include "Horo/Scene/SceneSource.h"
#include "editor/document/SceneDocumentInternal.h"
#include "editor/document/SceneDocumentTransactionCapture.h"

#include <limits>

namespace Horo::Editor {
    using namespace SceneDocumentDetail;
    using namespace SceneTransactionCapture;

    namespace {
        /** @brief Build a stable typed rejection without translating a lower-layer error. */
        Error Rejected(const SceneTransactionFailure reason) {
            return MakeError(SceneTransactionError(reason));
        }

    }  // namespace

    struct SceneDocumentTransactions::State final {
        std::unique_ptr<SceneDocument> ownedDocument;
        std::unique_ptr<EditorHistory> ownedHistory;
        SceneDocument *document{};
        EditorHistory *history{};
        SceneTransactionSession session;
        std::uint64_t replacementEpoch{};
        std::thread::id ownerThread{std::this_thread::get_id()};
    };

    /** @copydoc SceneDocumentTransactions::SceneDocumentTransactions */
    SceneDocumentTransactions::SceneDocumentTransactions() noexcept = default;

    /** @copydoc SceneDocumentTransactions::~SceneDocumentTransactions */
    SceneDocumentTransactions::~SceneDocumentTransactions() {
        Shutdown();
    }

    /** @copydoc SceneDocumentTransactions::SceneDocumentTransactions */
    SceneDocumentTransactions::SceneDocumentTransactions(SceneDocumentTransactions &&other) noexcept
        : lifetime_(std::move(other.lifetime_)), state_(std::move(other.state_)) {
        if (lifetime_)
            lifetime_->owner = this;
    }

    /** @copydoc SceneDocumentTransactions::operator= */
    SceneDocumentTransactions &SceneDocumentTransactions::operator=(SceneDocumentTransactions &&other) noexcept {
        if (this != &other) {
            Shutdown();
            lifetime_ = std::move(other.lifetime_);
            state_ = std::move(other.state_);
            if (lifetime_)
                lifetime_->owner = this;
        }
        return *this;
    }

    /** @copydoc SceneDocumentTransactions::Create */
    Result<SceneDocumentTransactions> SceneDocumentTransactions::Create(const SceneTransactionSession session,
                                                                        std::vector<SceneObjectSnapshot> objects,
                                                                        std::vector<ScenePrefabInstance> prefabInstances) {
        if (!session.owner.IsValid() || !session.generation)
            return Result<SceneDocumentTransactions>::Failure(Rejected(SceneTransactionFailure::Invalid));
        auto state = std::make_unique<State>();
        state->ownedDocument = std::make_unique<SceneDocument>();
        state->ownedHistory = std::make_unique<EditorHistory>();
        state->document = state->ownedDocument.get();
        state->history = state->ownedHistory.get();
        if (auto loaded = state->document->LoadSaved(std::move(objects), std::move(prefabInstances)); loaded.HasError())
            return Result<SceneDocumentTransactions>::Failure(loaded.ErrorValue());
        state->session = session;
        state->replacementEpoch = state->document->m_replacementEpoch;
        SceneDocumentTransactions result;
        result.state_ = std::move(state);
        result.lifetime_ = std::make_shared<Lifetime>(Lifetime{&result});
        return Result<SceneDocumentTransactions>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentTransactions::Bind */
    Result<SceneDocumentTransactions> SceneDocumentTransactions::Bind(SceneDocument &document, EditorHistory &history,
                                                                      const SceneTransactionSession session) {
        if (!session.owner.IsValid() || !session.generation)
            return Result<SceneDocumentTransactions>::Failure(Rejected(SceneTransactionFailure::Invalid));
        auto state = std::make_unique<State>();
        state->document = &document;
        state->history = &history;
        state->session = session;
        state->replacementEpoch = document.m_replacementEpoch;
        SceneDocumentTransactions result;
        result.state_ = std::move(state);
        result.lifetime_ = std::make_shared<Lifetime>(Lifetime{&result});
        return Result<SceneDocumentTransactions>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentTransactions::CheckAccess */
    Result<void> SceneDocumentTransactions::CheckAccess() const {
        using enum SceneTransactionFailure;
        if (!state_)
            return Result<void>::Failure(Rejected(Closed));
        if (state_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(Rejected(WrongThread));
        if (state_->document->m_replacementEpoch != state_->replacementEpoch)
            return Result<void>::Failure(Rejected(Stale));
        return Result<void>::Success();
    }

    /** @copydoc SceneDocumentTransactions::CheckCurrent */
    Result<void> SceneDocumentTransactions::CheckCurrent() const {
        return CheckAccess();
    }

    /** @copydoc SceneDocumentTransactions::Admit */
    Result<void> SceneDocumentTransactions::Admit(const SceneTransactionContext &context) const {
        using enum SceneTransactionFailure;
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(Rejected(Cancelled));
        if (!context.canEdit)
            return Result<void>::Failure(Rejected(Unauthorized));
        if (context.session != state_->session || context.revision != state_->document->Revision().value ||
            context.state != state_->document->State().value)
            return Result<void>::Failure(Rejected(Stale));
        return Result<void>::Success();
    }

    /** @copydoc SceneDocumentTransactions::IsDirty */
    Result<bool> SceneDocumentTransactions::IsDirty() const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        return Result<bool>::Success(state_->document->IsDirty());
    }

    /** @copydoc SceneDocumentTransactions::Snapshot */
    Result<SceneTransactionSnapshot> SceneDocumentTransactions::Snapshot(const SceneTransactionLimits limits) const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<SceneTransactionSnapshot>::Failure(access.ErrorValue());
        std::size_t bytes{};
        if (!limits.maximumSnapshotObjects || limits.maximumSnapshotObjects > 16'384 ||
            state_->document->Objects().size() > limits.maximumSnapshotObjects || !limits.maximumPreparedBytes ||
            limits.maximumPreparedBytes > 64 * 1024 * 1024 ||
            !ChargeObjects(state_->document->Objects(), limits.maximumPreparedBytes, bytes))
            return Result<SceneTransactionSnapshot>::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (const auto &instances = state_->document->m_prefabInstances;
            instances.size() > limits.maximumSnapshotObjects - state_->document->Objects().size() ||
            instances.size() > (limits.maximumPreparedBytes - bytes) / sizeof(ScenePrefabInstance))
            return Result<SceneTransactionSnapshot>::Failure(Rejected(SceneTransactionFailure::Capacity));
        auto value = state_->document->Snapshot();
        return Result<SceneTransactionSnapshot>::Success({state_->session, value.revision.value, value.state.value,
                                                          state_->document->IsDirty(), std::move(value.objects),
                                                          std::move(value.prefabInstances)});
    }

    /** @copydoc SceneDocumentTransactions::PreparationBudget */
    Result<std::size_t> SceneDocumentTransactions::PreparationBudget(const std::span<const SceneObjectReplacement> changes,
                                                                     const SceneTransactionLimits limits) const {
        using enum SceneTransactionFailure;
        if (!limits.maximumChanges || limits.maximumChanges > 4096 || changes.size() > limits.maximumChanges ||
            !limits.maximumSnapshotObjects || limits.maximumSnapshotObjects > 16'384 || !limits.maximumPreparedBytes ||
            limits.maximumPreparedBytes > 64 * 1024 * 1024)
            return Result<std::size_t>::Failure(Rejected(Capacity));
        std::size_t rootBytes{};
        if (state_->document->Objects().size() > limits.maximumSnapshotObjects ||
            !ChargeObjects(state_->document->Objects(), limits.maximumPreparedBytes / 3, rootBytes) ||
            state_->document->m_prefabInstances.size() > (limits.maximumPreparedBytes / 3 - rootBytes) / sizeof(ScenePrefabInstance))
            return Result<std::size_t>::Failure(Rejected(Capacity));
        rootBytes += state_->document->m_prefabInstances.size() * sizeof(ScenePrefabInstance);
        if (state_->history->m_impl->memoryBytes > (limits.maximumPreparedBytes - rootBytes * 3) / 2)
            return Result<std::size_t>::Failure(Rejected(Capacity));
        std::size_t incomingBytes{};
        const auto available = limits.maximumPreparedBytes - rootBytes * 3 - state_->history->m_impl->memoryBytes * 2;
        for (const auto &change : changes)
            if (change.desired && !ChargeObjects(std::span{&*change.desired, 1}, available / 4, incomingBytes))
                return Result<std::size_t>::Failure(Rejected(Capacity));
        return Result<std::size_t>::Success(available);
    }

    struct SceneDocumentTransactions::Prepared final {
        State *owner{};
        SceneTransactionContext context;
        std::vector<SceneObjectSnapshot> objects;
        std::optional<std::vector<ScenePrefabInstance>> instances;
        std::unique_ptr<SceneDocument> validated;
        std::unique_ptr<EditorHistory::Impl> history;
        bool consumed{};
        std::optional<DocumentStateId> replayState;

        /** @brief Builds the detached root and complete semantic history before any owner publication. */
        Result<void> StageRoot(const SceneDocumentTransactions &port, SceneObjectsReplacedDelta delta, const SceneTransactionLimits limits,
                               const std::size_t budget, const bool coordinated) {
            auto candidate = owner->document->m_objects;
            ReplaceObjects(candidate, delta, false);
            if (candidate.size() > limits.maximumSnapshotObjects ||
                owner->document->m_prefabInstances.size() > limits.maximumSnapshotObjects - candidate.size())
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            auto candidateInstances = owner->document->m_prefabInstances;
            if (delta.prefabCreation)
                ApplyPrefabTypedDelta(candidateInstances, *delta.prefabCreation);
            if (candidateInstances.size() > limits.maximumSnapshotObjects - candidate.size())
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            SceneDocument validatedRoot;
            if (auto valid = validatedRoot.LoadSaved(candidate, candidateInstances); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            const auto &document = *owner->document;
            if (document.m_revision.value == std::numeric_limits<std::uint64_t>::max() ||
                document.m_nextStateId == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            std::vector<SceneObjectId> affected;
            affected.reserve(delta.objects.size());
            for (const auto &change : delta.objects)
                affected.push_back(change.object);
            SceneCommandDelta semantic = std::move(delta);
            const auto bytes = EstimateMemoryBytes(semantic, affected.size());
            if (bytes > kMaximumHistoryBytes || bytes > budget)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            EditorHistory::Impl preparedHistory = *owner->history->m_impl;
            PushHistory(preparedHistory, HistoryRecord{document.m_state, DocumentStateId{document.m_nextStateId}, std::move(semantic),
                                                       std::move(affected), bytes});
            if (auto admitted = port.Admit(context); admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            preparedHistory.undo.back().coordinated = coordinated;
            if (const auto &replacement = std::get<SceneObjectsReplacedDelta>(preparedHistory.undo.back().delta);
                replacement.prefabCreation)
                preparedHistory.undo.back().affectedPrefabInstances.push_back(replacement.prefabCreation->instance.instanceId);
            objects = std::move(candidate);
            instances = std::move(candidateInstances);
            this->validated = std::make_unique<SceneDocument>(std::move(validatedRoot));
            this->history = std::make_unique<EditorHistory::Impl>(std::move(preparedHistory));
            return Result<void>::Success();
        }
    };

    /** @copydoc SceneDocumentTransactions::Prepare */
    Result<std::shared_ptr<SceneDocumentTransactions::Prepared>> SceneDocumentTransactions::Prepare(
        const SceneTransactionContext &context, const std::span<const SceneObjectReplacement> changes, const SceneTransactionLimits limits,
        const bool coordinated) const {
        using Return = Result<std::shared_ptr<Prepared>>;
        if (auto admitted = Admit(context); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        const auto budget = PreparationBudget(changes, limits);
        if (budget.HasError())
            return Return::Failure(budget.ErrorValue());
        auto prepared = std::make_shared<Prepared>();
        prepared->owner = state_.get();
        prepared->context = context;
        if (changes.empty())
            return Return::Success(std::move(prepared));
        auto captured = CaptureReplacements(state_->document->m_objects, changes);
        if (captured.HasError())
            return Return::Failure(captured.ErrorValue());
        auto delta = std::move(captured).Value();
        if (EstimateTypedDeltaMemoryBytes(delta) > limits.maximumPreparedBytes)
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (delta.objects.empty())
            return Return::Success(std::move(prepared));
        if (auto staged = prepared->StageRoot(*this, std::move(delta), limits, budget.Value(), coordinated); staged.HasError())
            return Return::Failure(staged.ErrorValue());
        return Return::Success(std::move(prepared));
    }

    /** @copydoc SceneDocumentTransactions::ValidatePrepared */
    Result<void> SceneDocumentTransactions::ValidatePrepared(const Prepared &prepared) const {
        if (prepared.owner != state_.get() || prepared.consumed)
            return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
        return Admit(prepared.context);
    }

    /** @copydoc SceneDocumentTransactions::PublishPrepared */
    SceneTransactionResult SceneDocumentTransactions::PublishPrepared(Prepared &prepared) noexcept {
        prepared.consumed = true;
        if (!prepared.history)
            return {prepared.context.revision, prepared.context.state, false};
        state_->history->m_impl->undo.swap(prepared.history->undo);
        state_->history->m_impl->redo.swap(prepared.history->redo);
        state_->history->m_impl->memoryBytes = prepared.history->memoryBytes;
        if (prepared.instances)
            state_->document->m_prefabInstances.swap(*prepared.instances);
        auto result = PublishRoot(prepared.objects, *prepared.validated);
        if (prepared.replayState) {
            state_->document->m_state = *prepared.replayState;
            result.state = prepared.replayState->value;
        }
        return result;
    }

    /** @copydoc SceneDocumentTransactions::PreparedContext */
    SceneTransactionContext SceneDocumentTransactions::PreparedContext(const Prepared &prepared) const noexcept {
        auto result = prepared.context;
        if (prepared.history) {
            ++result.revision;
            result.state = prepared.replayState ? prepared.replayState->value : state_->document->m_nextStateId;
        }
        return result;
    }

    /** @copydoc SceneDocumentTransactions::PrepareReplay */
    Result<std::shared_ptr<SceneDocumentTransactions::Prepared>> SceneDocumentTransactions::PrepareReplay(
        const SceneTransactionContext &context, const bool redo) {
        using Return = Result<std::shared_ptr<Prepared>>;
        if (auto admitted = Admit(context); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        const auto &source = redo ? state_->history->m_impl->redo : state_->history->m_impl->undo;
        if (source.empty() || state_->document->m_revision.value == UINT64_MAX || state_->document->m_nextStateId == UINT64_MAX)
            return Return::Failure(Rejected(SceneTransactionFailure::Stale));
        const auto &entry = source.back();
        if (auto valid = ValidateReplay(state_->document->m_objects, entry.delta, redo); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        auto prepared = std::make_shared<Prepared>();
        prepared->owner = state_.get();
        prepared->context = context;
        prepared->objects = state_->document->m_objects;
        auto instances = state_->document->m_prefabInstances;
        if (redo)
            ApplyDelta(prepared->objects, instances, entry.delta);
        else
            RevertDelta(prepared->objects, instances, entry.delta);
        prepared->validated = std::make_unique<SceneDocument>();
        if (auto valid = prepared->validated->LoadSaved(prepared->objects, instances); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        prepared->instances = std::move(instances);
        prepared->replayState = redo ? entry.afterState : entry.beforeState;
        prepared->history = std::make_unique<EditorHistory::Impl>(*state_->history->m_impl);
        auto &from = redo ? prepared->history->redo : prepared->history->undo;
        auto &to = redo ? prepared->history->undo : prepared->history->redo;
        to.push_back(std::move(from.back()));
        from.pop_back();
        return Return::Success(std::move(prepared));
    }

    /** @copydoc SceneDocumentTransactions::PreparePrefabReplacement */
    Result<SceneDocumentTransactions::PrefabReplacement> SceneDocumentTransactions::PreparePrefabReplacement(
        const SceneTransactionContext &context, const SceneObjectId root, const Prefab::PrefabAssetReference &source,
        const SceneTransactionLimits limits) const {
        using Return = Result<PrefabReplacement>;
        if (auto admitted = Admit(context); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        const auto budget = PreparationBudget({}, limits);
        if (budget.HasError())
            return Return::Failure(budget.ErrorValue());
        const auto &document = *state_->document;
        const auto selected = FindObject(document.m_objects, root);
        if (selected == document.m_objects.end() || !source.IsValid())
            return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        auto captured = CapturePrefabSubtree(document.m_objects, document.m_prefabInstances, root, limits.maximumChanges);
        if (captured.HasError())
            return Return::Failure(captured.ErrorValue());
        auto instance = AllocatePrefabInstanceId(document.m_nextPrefabInstanceId);
        if (instance.HasError())
            return Return::Failure(instance.ErrorValue());
        auto delta = std::move(captured).Value();
        delta.prefabCreation = CreatedPrefabInstanceDelta{{instance.Value(), source, selected->parent, selected->localTransform},
                                                          document.m_prefabInstances.size()};
        auto prepared = std::make_shared<Prepared>();
        prepared->owner = state_.get();
        prepared->context = context;
        if (auto staged = prepared->StageRoot(*this, std::move(delta), limits, budget.Value(), false); staged.HasError())
            return Return::Failure(staged.ErrorValue());
        PrefabReplacement result;
        result.prepared_ = std::move(prepared);
        result.instance_ = instance.Value();
        return Return::Success(std::move(result));
    }

    /** @copydoc SceneDocumentTransactions::CommitPrefabReplacement */
    Result<SceneTransactionResult> SceneDocumentTransactions::CommitPrefabReplacement(const PrefabReplacement &replacement) {
        if (!replacement.prepared_)
            return Result<SceneTransactionResult>::Failure(Rejected(SceneTransactionFailure::Invalid));
        if (auto valid = ValidatePrepared(*replacement.prepared_); valid.HasError())
            return Result<SceneTransactionResult>::Failure(valid.ErrorValue());
        return Result<SceneTransactionResult>::Success(PublishPrepared(*replacement.prepared_));
    }

    /** @copydoc SceneDocumentTransactions::Execute */
    Result<SceneTransactionResult> SceneDocumentTransactions::Execute(const SceneTransactionContext &context,
                                                                      const std::span<const SceneObjectReplacement> changes,
                                                                      const SceneTransactionLimits limits) {
        auto prepared = Prepare(context, changes, limits);
        if (prepared.HasError())
            return Result<SceneTransactionResult>::Failure(prepared.ErrorValue());
        if (auto valid = ValidatePrepared(*prepared.Value()); valid.HasError())
            return Result<SceneTransactionResult>::Failure(valid.ErrorValue());
        return Result<SceneTransactionResult>::Success(PublishPrepared(*prepared.Value()));
    }

    /** @copydoc SceneDocumentTransactions::PublishRoot */
    SceneTransactionResult SceneDocumentTransactions::PublishRoot(std::vector<SceneObjectSnapshot> &candidate,
                                                                  const SceneDocument &validated) noexcept {
        auto &owner = *state_->document;
        owner.m_objects.swap(candidate);
        owner.m_nextObjectId = std::max(owner.m_nextObjectId, validated.m_nextObjectId);
        owner.m_nextBehaviorInstanceId = std::max(owner.m_nextBehaviorInstanceId, validated.m_nextBehaviorInstanceId);
        owner.m_nextPrefabInstanceId = std::max(owner.m_nextPrefabInstanceId, validated.m_nextPrefabInstanceId);
        owner.m_nextNavigationSurfaceId = std::max(owner.m_nextNavigationSurfaceId, validated.m_nextNavigationSurfaceId);
        owner.m_nextNavigationRegionId = std::max(owner.m_nextNavigationRegionId, validated.m_nextNavigationRegionId);
        owner.m_nextNavigationModifierId = std::max(owner.m_nextNavigationModifierId, validated.m_nextNavigationModifierId);
        owner.m_nextNavigationLinkId = std::max(owner.m_nextNavigationLinkId, validated.m_nextNavigationLinkId);
        owner.m_nextAiAgentId = std::max(owner.m_nextAiAgentId, validated.m_nextAiAgentId);
        ++owner.m_revision.value;
        owner.m_state = DocumentStateId{owner.m_nextStateId++};
        return {owner.m_revision.value, owner.m_state.value, true};
    }

    /** @copydoc SceneDocumentHistoryReplay::Execute */
    Result<SceneCommandResult> SceneDocumentHistoryReplay::Execute(SceneDocument &document, EditorHistory &historyOwner,
                                                                   const bool redoDirection) {
        const auto &source = redoDirection ? historyOwner.m_impl->redo : historyOwner.m_impl->undo;
        if (source.empty())
            return Result<SceneCommandResult>::Failure(
                MakeError(redoDirection ? SceneDocumentErrors::NothingToRedo : SceneDocumentErrors::NothingToUndo));
        const auto &entry = source.back();
        if (entry.coordinated)
            return Result<SceneCommandResult>::Failure(Rejected(SceneTransactionFailure::Stale));
        if (auto valid = ValidateReplay(document.m_objects, entry.delta, redoDirection); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        if (document.m_revision.value == std::numeric_limits<std::uint64_t>::max())
            return Result<SceneCommandResult>::Failure(Rejected(SceneTransactionFailure::Capacity));
        auto objects = document.m_objects;
        auto instances = document.m_prefabInstances;
        if (redoDirection)
            ApplyDelta(objects, instances, entry.delta);
        else
            RevertDelta(objects, instances, entry.delta);
        SceneDocument validated;
        if (auto valid = validated.LoadSaved(objects, instances); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        const auto state = redoDirection ? entry.afterState : entry.beforeState;
        auto result = HistoryCommandResult(document, entry, redoDirection ? DocumentChangeKind::Redone : DocumentChangeKind::Undone);
        result.revision.value = document.m_revision.value + 1;
        result.state = state;
        EditorHistory::Impl history = *historyOwner.m_impl;
        auto &from = redoDirection ? history.redo : history.undo;
        auto &to = redoDirection ? history.undo : history.redo;
        to.push_back(std::move(from.back()));
        from.pop_back();
        document.m_objects.swap(objects);
        document.m_prefabInstances.swap(instances);
        historyOwner.m_impl->undo.swap(history.undo);
        historyOwner.m_impl->redo.swap(history.redo);
        ++document.m_revision.value;
        document.m_state = state;
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentTransactions::Replay */
    Result<SceneTransactionResult> SceneDocumentTransactions::Replay(const SceneTransactionContext &context, const bool redo) {
        if (auto admitted = Admit(context); admitted.HasError())
            return Result<SceneTransactionResult>::Failure(admitted.ErrorValue());
        SceneDocumentCommandExecutor executor{*state_->document, *state_->history};
        auto result = redo ? executor.Redo() : executor.Undo();
        if (result.HasError())
            return Result<SceneTransactionResult>::Failure(result.ErrorValue());
        return Result<SceneTransactionResult>::Success(
            {result.Value().revision.value, result.Value().state.value, result.Value().committed});
    }

    /** @copydoc SceneDocumentTransactions::MarkSaved */
    Result<void> SceneDocumentTransactions::MarkSaved(const SceneTransactionContext &context) {
        if (auto admitted = Admit(context); admitted.HasError())
            return admitted;
        return state_->document->MarkSaved({context.revision}, {context.state});
    }

    /** @copydoc SceneDocumentTransactions::Shutdown */
    void SceneDocumentTransactions::Shutdown() noexcept {
        if (lifetime_)
            lifetime_->owner = nullptr;
        lifetime_.reset();
        state_.reset();
    }
}  // namespace Horo::Editor
