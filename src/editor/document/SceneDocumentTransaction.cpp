#include "Horo/Editor/SceneDocumentTransaction.h"

#include "Horo/Scene/SceneSource.h"
#include "editor/document/SceneDocumentInternal.h"

#include <array>
#include <limits>

namespace Horo::Editor {
    using namespace SceneDocumentDetail;

    namespace {
        /** @brief Build a stable typed rejection without translating a lower-layer error. */
        Error Rejected(const SceneTransactionFailure reason) {
            return MakeError(SceneTransactionError(reason));
        }

        /** @brief Compare exact semantic content and its independent generation ownership. */
        Result<void> Matches(const SceneObjectSnapshot &object, const Sha256Digest &content,
                             const std::optional<SceneSource::SceneGeneratedOwnership> &ownership) {
            if (object.generated != ownership)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            const auto fingerprint = FingerprintSceneObjectContent(object, kMaximumHistoryBytes);
            if (fingerprint.HasError())
                return Result<void>::Failure(fingerprint.ErrorValue());
            if (fingerprint.Value() != content)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            return Result<void>::Success();
        }

        /** @brief Charge fixed and nested authored storage against a finite envelope before cloning roots. */
        bool ChargeObjects(const std::span<const SceneObjectSnapshot> objects, const std::size_t limit, std::size_t &bytes) {
            for (const auto &object : objects) {
                const auto nested = EstimateSceneObjectOwnedMemoryBytes(object);
                if (nested > limit || sizeof(object) > limit - nested || bytes > limit - nested - sizeof(object))
                    return false;
                bytes += nested + sizeof(object);
            }
            return true;
        }

        /** @brief Prove a generated fragment's authored/retirement reservation before copying it. */
        bool FitsReservation(const SceneObjectReplacement &change, const SceneObjectSnapshot *before) {
            if (!(change.desired && change.desired->generated) && !(before && before->generated))
                return true;
            const std::size_t afterBytes =
                change.desired ? sizeof(SceneObjectSnapshot) + EstimateSceneObjectOwnedMemoryBytes(*change.desired) : 0;
            const std::size_t beforeBytes = before ? sizeof(SceneObjectSnapshot) + EstimateSceneObjectOwnedMemoryBytes(*before) : 0;
            return afterBytes <= change.resources.residentBytes && beforeBytes <= change.resources.retirementBytes &&
                   afterBytes <= change.resources.preparationBytes && beforeBytes <= change.resources.preparationBytes - afterBytes;
        }

        /** @brief Derive committed indices from surviving authored order before applying detached changes. */
        void ResolveReplacementIndices(SceneObjectsReplacedDelta &delta, const std::size_t objectCount) {
            const auto removed = static_cast<std::size_t>(std::ranges::count_if(delta.objects, [](const auto &change) {
                return change.before && !change.after;
            }));
            std::size_t appended = objectCount - removed;
            for (auto &change : delta.objects) {
                if (!change.after)
                    continue;
                if (!change.before) {
                    change.after->index = appended++;
                    continue;
                }
                const auto removedBefore = std::ranges::count_if(delta.objects, [&change](const auto &other) {
                    return other.before && !other.after && other.before->index < change.before->index;
                });
                change.after->index = change.before->index - static_cast<std::size_t>(removedBefore);
            }
        }

        /** @brief Capture one validated before/after pair; absence denotes an exact semantic no-op. */
        Result<std::optional<SceneObjectReplacementDelta>> CaptureReplacement(const std::vector<SceneObjectSnapshot> &objects,
                                                                              const SceneObjectReplacement &change) {
            using Return = Result<std::optional<SceneObjectReplacementDelta>>;
            const auto current = FindObject(objects, change.object);
            if (!FitsReservation(change, current == objects.end() ? nullptr : std::to_address(current)))
                return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
            SceneObjectReplacementDelta captured{change.object, {}, {}};
            if (current == objects.end()) {
                if (change.priorContent != Sha256Digest{} || change.expectedOwnership || !change.desired)
                    return Return::Failure(Rejected(SceneTransactionFailure::Stale));
            } else {
                if (IsEffectivelyLocked(objects, change.object))
                    return Return::Failure(LockedObjectError());
                if (const auto matches = Matches(*current, change.priorContent, change.expectedOwnership); matches.HasError())
                    return Return::Failure(matches.ErrorValue());
                captured.before = IndexedSceneObject{*current, static_cast<std::size_t>(current - objects.begin())};
            }
            if (change.desired) {
                if (change.desired->id != change.object)
                    return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
                // Existing objects keep their index; ResolveReplacementIndices assigns final positions after removals.
                captured.after = IndexedSceneObject{*change.desired, captured.before ? captured.before->index : objects.size()};
            }
            if (captured.before && captured.after && captured.before->object.generated == captured.after->object.generated) {
                const auto desired = FingerprintSceneObjectContent(captured.after->object, kMaximumHistoryBytes);
                if (desired.HasError())
                    return Return::Failure(desired.ErrorValue());
                if (desired.Value() == change.priorContent)
                    return Return::Success(std::nullopt);
            }
            return Return::Success(std::move(captured));
        }

        /** @brief Validate all submitted identities before no-op filtering and capture the complete detached delta. */
        Result<SceneObjectsReplacedDelta> CaptureReplacements(const std::vector<SceneObjectSnapshot> &objects,
                                                              const std::span<const SceneObjectReplacement> changes) {
            SceneObjectsReplacedDelta delta;
            delta.objects.reserve(changes.size());
            std::vector<SceneObjectId> submitted;
            submitted.reserve(changes.size());
            for (const auto &change : changes) {
                if (!change.object.IsValid() || std::ranges::find(submitted, change.object) != submitted.end())
                    return Result<SceneObjectsReplacedDelta>::Failure(Rejected(SceneTransactionFailure::Invalid));
                submitted.push_back(change.object);
                auto captured = CaptureReplacement(objects, change);
                if (captured.HasError())
                    return Result<SceneObjectsReplacedDelta>::Failure(captured.ErrorValue());
                if (captured.Value())
                    delta.objects.push_back(std::move(*captured.Value()));
            }
            ResolveReplacementIndices(delta, objects.size());
            return Result<SceneObjectsReplacedDelta>::Success(std::move(delta));
        }

        /** @brief Guard exact generated ownership/content before historical cleanup or replacement. */
        Result<void> ValidateReplay(const std::vector<SceneObjectSnapshot> &objects, const SceneCommandDelta &delta, const bool redo) {
            const auto *batch = std::get_if<SceneObjectsReplacedDelta>(&delta);
            if (!batch)
                return Result<void>::Success();
            for (const auto &change : batch->objects) {
                const auto &expected = redo ? change.before : change.after;
                const auto current = FindObject(objects, change.object);
                if (!expected) {
                    if (current != objects.end())
                        return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
                    continue;
                }
                if (current == objects.end())
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
                const auto fingerprint = FingerprintSceneObjectContent(expected->object, kMaximumHistoryBytes);
                if (fingerprint.HasError())
                    return Result<void>::Failure(fingerprint.ErrorValue());
                if (auto matched = Matches(*current, fingerprint.Value(), expected->object.generated); matched.HasError())
                    return matched;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SceneTransactionError */
    const ErrorCodeDescriptor &SceneTransactionError(const SceneTransactionFailure reason) noexcept {
        using enum ErrorSeverity;
        static const ErrorDomainId domain{"horo.editor.scene_transaction"};
        static const std::array<ErrorCodeDescriptor, 7> errors{
            {{domain, ErrorCode{"scene_transaction.closed"}, Error, "Scene transaction admission is closed.",
              "Open a fresh document transaction port.", false, true},
             {domain, ErrorCode{"scene_transaction.wrong_thread"}, Error, "Scene transaction requires its owner thread.",
              "Return to the document owner phase.", false, false},
             {domain, ErrorCode{"scene_transaction.stale"}, Error, "Scene transaction evidence is stale or conflicts.",
              "Capture current document and ownership evidence.", false, true},
             {domain, ErrorCode{"scene_transaction.unauthorized"}, Error, "Scene editing permission was not granted.",
              "Obtain explicit document edit authority.", false, true},
             {domain, ErrorCode{"scene_transaction.cancelled"}, Error, "Scene transaction was cancelled.",
              "Preserve the prior authored state.", false, false},
             {domain, ErrorCode{"scene_transaction.invalid"}, Error, "Scene transaction is malformed.",
              "Supply complete finite typed replacements.", false, true},
             {domain, ErrorCode{"scene_transaction.capacity"}, Error, "Scene transaction exceeds its bounded reservation.",
              "Reduce the transaction or raise the host budget within safety bounds.", false, true}}};
        const auto index = static_cast<std::size_t>(reason);
        return errors[index < errors.size() ? index : static_cast<std::size_t>(SceneTransactionFailure::Invalid)];
    }

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
    SceneDocumentTransactions::~SceneDocumentTransactions() = default;
    /** @copydoc SceneDocumentTransactions::SceneDocumentTransactions */
    SceneDocumentTransactions::SceneDocumentTransactions(SceneDocumentTransactions &&) noexcept = default;
    /** @copydoc SceneDocumentTransactions::operator= */
    SceneDocumentTransactions &SceneDocumentTransactions::operator=(SceneDocumentTransactions &&) noexcept = default;

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

    /** @copydoc FingerprintSceneObjectContent */
    Result<Sha256Digest> FingerprintSceneObjectContent(const SceneObjectSnapshot &object, const std::size_t maximumBytes) {
        if (!maximumBytes || maximumBytes > 16 * 1024 * 1024 || EstimateSceneObjectOwnedMemoryBytes(object) > maximumBytes)
            return Result<Sha256Digest>::Failure(Rejected(SceneTransactionFailure::Capacity));
        auto authored = object;
        authored.generated.reset();
        const auto encoded = SceneSource::EncodeSceneSource({std::span{&authored, 1}, {}});
        if (encoded.size() > maximumBytes)
            return Result<Sha256Digest>::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (auto parsed = SceneSource::DecodeSceneSource(encoded); parsed.HasError())
            return Result<Sha256Digest>::Failure(parsed.ErrorValue());
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{encoded.data(), encoded.size()})));
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

    /** @copydoc SceneDocumentTransactions::Execute */
    Result<SceneTransactionResult> SceneDocumentTransactions::Execute(const SceneTransactionContext &context,
                                                                      const std::span<const SceneObjectReplacement> changes,
                                                                      const SceneTransactionLimits limits) {
        if (auto admitted = Admit(context); admitted.HasError())
            return Result<SceneTransactionResult>::Failure(admitted.ErrorValue());
        const auto budget = PreparationBudget(changes, limits);
        if (budget.HasError())
            return Result<SceneTransactionResult>::Failure(budget.ErrorValue());
        if (changes.empty())
            return Result<SceneTransactionResult>::Success({context.revision, context.state, false});
        auto captured = CaptureReplacements(state_->document->m_objects, changes);
        if (captured.HasError())
            return Result<SceneTransactionResult>::Failure(captured.ErrorValue());
        auto delta = std::move(captured).Value();
        if (EstimateTypedDeltaMemoryBytes(delta) > limits.maximumPreparedBytes)
            return Result<SceneTransactionResult>::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (delta.objects.empty())
            return Result<SceneTransactionResult>::Success({context.revision, context.state, false});
        auto candidate = state_->document->m_objects;
        ReplaceObjects(candidate, delta, false);
        if (candidate.size() > limits.maximumSnapshotObjects ||
            state_->document->m_prefabInstances.size() > limits.maximumSnapshotObjects - candidate.size())
            return Result<SceneTransactionResult>::Failure(Rejected(SceneTransactionFailure::Capacity));
        SceneDocument validated;
        if (auto valid = validated.LoadSaved(candidate, state_->document->m_prefabInstances); valid.HasError())
            return Result<SceneTransactionResult>::Failure(valid.ErrorValue());
        const auto &document = *state_->document;
        if (document.m_revision.value == std::numeric_limits<std::uint64_t>::max() ||
            document.m_nextStateId == std::numeric_limits<std::uint64_t>::max())
            return Result<SceneTransactionResult>::Failure(Rejected(SceneTransactionFailure::Capacity));
        std::vector<SceneObjectId> affected;
        affected.reserve(delta.objects.size());
        for (const auto &change : delta.objects)
            affected.push_back(change.object);
        SceneCommandDelta semantic = std::move(delta);
        const auto bytes = EstimateMemoryBytes(semantic, affected.size());
        if (bytes > kMaximumHistoryBytes || bytes > budget.Value())
            return Result<SceneTransactionResult>::Failure(Rejected(SceneTransactionFailure::Capacity));
        EditorHistory::Impl history = *state_->history->m_impl;
        PushHistory(history, HistoryRecord{document.m_state, DocumentStateId{document.m_nextStateId}, std::move(semantic),
                                           std::move(affected), bytes});
        if (auto admitted = Admit(context); admitted.HasError())
            return Result<SceneTransactionResult>::Failure(admitted.ErrorValue());
        state_->history->m_impl->undo.swap(history.undo);
        state_->history->m_impl->redo.swap(history.redo);
        state_->history->m_impl->memoryBytes = history.memoryBytes;
        return Result<SceneTransactionResult>::Success(PublishRoot(candidate, validated));
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
        state_.reset();
    }
}  // namespace Horo::Editor
