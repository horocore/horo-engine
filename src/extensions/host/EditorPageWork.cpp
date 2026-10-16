#include "EditorPageHostInternal.h"

#include <algorithm>
#include <ranges>
#include <type_traits>

namespace Horo::Extensions::EditorPageDetail {
    namespace {
        /** @brief Records a provider failure while preserving its full typed error at the presentation boundary. */
        void Failed(EditorPageSession &session, Error failure) {
            session.failure = std::move(failure);
            session.state = EditorPageState::Failed;
        }

        /** @brief Copies all provider findings only if every binding/code/message is admitted; no partial diagnostics publish. */
        bool ValidFindings(const EditorPageSession &session, const std::vector<EditorPageFinding> &findings) {
            if (findings.size() > 32)
                return false;
            return std::ranges::all_of(findings, [&](const auto &f) {
                if (f.severity > EditorUiValidationSeverity::Error || f.message.kind != EditorUiTextKind::LocalizationKey ||
                    !session.context.Allows(EditorSurfaceLocalizationKey{f.message.value}) || !session.context.Allows(f.code))
                    return false;
                return !f.binding.IsValid() || std::ranges::find(session.definition.fields, f.binding, &EditorPageField::binding) !=
                                                   session.definition.fields.end();
            });
        }

        /** @brief Executes one provider call with no borrowed session/host pointer and publishes the handoff exactly once. */
        Result<void> Invoke(IEditorPageOperations &provider, const std::shared_ptr<EditorPageWork> &work, const CancellationToken &cancel) {
            try {
                if (cancel.IsCancellationRequested())
                    work->result.emplace(Result<std::vector<EditorPageFinding>>::Failure(JobCancelled().ErrorValue()));
                else
                    work->result.emplace(provider.Invoke(work->operation, work->draft, work->action, cancel));
            } catch (...) {
                work->result.emplace(Result<std::vector<EditorPageFinding>>::Failure(MakeError(ExtensionErrors::ContributionRejected)));
            }
            work->ready.store(true, std::memory_order_release);
            return Result<void>::Success();
        }

        /** @brief Retires a queued-cancelled or scheduler-failed callback that never entered provider code or published a result. */
        void RetireUnpublished(EditorPageHostState &host, EditorPageSession &session) {
            if (!session.job)
                return;
            const auto snapshot = session.job->Snapshot();
            if (!snapshot || snapshot->state == JobState::Queued || snapshot->state == JobState::Running ||
                (session.work && session.work->ready.load(std::memory_order_acquire)))
                return;
            host.projectionDirty = true;
            session.job.reset();
            session.work.reset();
            if (session.state != EditorPageState::Cancelled)
                Failed(session, snapshot->error.value_or(MakeError(ExtensionErrors::ContributionRejected)));
        }

        /** @brief Prepares all active/draft containers before durable I/O, then performs only noexcept publication after success. */
        void CommitSettings(EditorPageHostState &host, EditorPageSession &session, const EditorPageDraft &draft) {
            auto candidate = Candidate(session, draft, session.active->Revision() + 1);
            if (candidate.HasError()) {
                Failed(session, candidate.ErrorValue());
                return;
            }
            auto cleanDraft = draft;
            std::optional<ConfigurationSnapshot> prepared{std::move(candidate).Value()};
            static_assert(std::is_nothrow_swappable_v<decltype(prepared)>);
            static_assert(std::is_nothrow_move_assignable_v<EditorPageDraft>);
            if (!session.definition.fields.empty()) {
                const auto scope = session.definition.fields.front().setting.scope;
                if (session.contextRevision == 0 || host.store.ContextRevision(scope) != session.contextRevision) {
                    Failed(session, MakeError(ExtensionErrors::CapabilityRevoked));
                    return;
                }
                try {
                    if (auto written = host.store.Write(session.context.Provider(), session.context.Surface().id, scope,
                                                        session.contextRevision, *prepared);
                        written.HasError()) {
                        session.state = EditorPageState::RecoveryRequired;
                        session.failure = std::move(written).ErrorValue();
                        return;
                    }
                } catch (...) {
                    session.state = EditorPageState::RecoveryRequired;
                    session.failure = MakeError(ExtensionErrors::ContributionRejected);
                    return;
                }
            }
            session.active.swap(prepared);
            session.draft = std::move(cleanDraft);
            session.dirty = false;
            session.failure.reset();
            session.state = EditorPageState::Editing;
        }
    }  // namespace

    /** @copydoc Schedule */
    Result<void> Schedule(EditorPageHostState &host, EditorPageSession &session, EditorPageOperation operation, EditorUiActionId action) {
        if (session.job || session.revoked || !session.operations)
            return Failure();
        const auto retirement = session.retirement.lock();
        const auto code = retirement ? retirement->Acquire(session.context.Provider().moduleId, ExtensionLeaseKind::Job,
                                                           session.context.Surface().id, session.operations)
                                     : nullptr;
        if (!code)
            return Failure(ExtensionErrors::CapabilityRevoked);
        auto acquired = session.context.AcquireCapabilityUse(
            session.context.Surface().kind == EditorSurfaceKind::SettingsPage ? EditorSettingsPageCapability : EditorModalPageCapability);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        auto use = std::make_shared<ExtensionCapabilityUseLease>(std::move(acquired).Value());
        auto work = std::make_shared<EditorPageWork>();
        work->operation = operation;
        work->action = std::move(action);
        work->draft = operation == EditorPageOperation::Cancel ? *session.cancelledDraft : session.draft;
        // code owns the provider object; the raw borrow cannot outlive that attributed executable lease.
        auto *const provider = session.operations.get();
        auto accepted = host.jobs.SubmitResult({}, [code, use, work, provider](const CancellationToken &cancel) {
            return Invoke(*provider, work, cancel);
        });
        if (accepted.HasError())
            return Result<void>::Failure(accepted.ErrorValue());
        host.projectionDirty = true;
        session.work = std::move(work);
        session.job.emplace(std::move(accepted).Value());
        if (operation != EditorPageOperation::Cancel)
            session.state = operation == EditorPageOperation::Validate ? EditorPageState::Validating : EditorPageState::Submitting;
        return Result<void>::Success();
    }

    /** @copydoc Complete */
    void Complete(EditorPageHostState &host, EditorPageSession &session) {
        if (!session.work)
            return;
        if (!session.work->ready.load(std::memory_order_acquire)) {
            RetireUnpublished(host, session);
            return;
        }
        host.projectionDirty = true;
        const auto work = session.work;
        session.job.reset();
        session.work.reset();
        if (session.revoked || !session.context.IsUsable() || session.state == EditorPageState::Cancelled ||
            work->draft.revision != session.revision)
            return;
        if (auto use = CheckPermission(host, session); use.HasError()) {
            Failed(session, use.ErrorValue());
            return;
        }
        if (!work->result || work->result->HasError()) {
            Failed(session, work->result ? work->result->ErrorValue() : MakeError(ExtensionErrors::ContributionRejected));
            return;
        }
        if (!ValidFindings(session, work->result->Value())) {
            Failed(session, MakeError(ExtensionErrors::EditorUiFormInvalid));
            return;
        }
        session.findings = std::move(*work->result).Value();
        if (std::ranges::any_of(session.findings, [](const auto &f) {
            return f.severity == EditorUiValidationSeverity::Error;
        })) {
            session.state = EditorPageState::Failed;
            return;
        }
        if (work->operation == EditorPageOperation::Validate) {
            if (session.context.Surface().kind == EditorSurfaceKind::SettingsPage)
                CommitSettings(host, session, work->draft);
            else if (auto submitted = Schedule(host, session, EditorPageOperation::Submit, work->action); submitted.HasError())
                Failed(session, submitted.ErrorValue());
        } else if (work->operation == EditorPageOperation::Submit) {
            session.state = EditorPageState::Completed;
            session.dirty = false;
            session.failure.reset();
        }
    }
}  // namespace Horo::Extensions::EditorPageDetail

namespace Horo::Extensions {
    /** @copydoc EditorPageHost::Update */
    void EditorPageHost::Update() {
        if (!state_->OnThread() || state_->shutdown)
            return;
        for (const auto &weak : state_->sessions)
            if (const auto session = weak.lock(); session && !session->revoked) {
                if (!session->context.IsUsable() ||
                    !state_->registry.IsAvailable(session->context.Provider(), session->context.Surface().id)) {
                    state_->projectionDirty = true;
                    session->Revoke();
                    continue;
                }
                if (session->context.Surface().kind == EditorSurfaceKind::ModalPage) {
                    const auto parent =
                        EditorPageDetail::Find(*state_, session->context.Provider(), session->context.Surface().placement.target);
                    if (!parent || (parent->state != EditorPageState::Editing && parent->state != EditorPageState::Failed))
                        static_cast<void>(Cancel(session->context.Provider(), session->context.Surface().id));
                }
                EditorPageDetail::Complete(*state_, *session);
                if (!session->job && session->cancelledDraft) {
                    auto scheduled = EditorPageDetail::Schedule(*state_, *session, EditorPageOperation::Cancel, {});
                    session->cancelledDraft.reset();
                    if (scheduled.HasError())
                        session->failure = scheduled.ErrorValue();
                }
            }
        EditorPageDetail::Prepare(*state_);
    }
}  // namespace Horo::Extensions
