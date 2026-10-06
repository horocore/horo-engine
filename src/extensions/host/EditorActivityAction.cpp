#include "EditorActivitySessionState.h"

namespace Horo::Extensions {
    namespace {
        std::uint8_t ActionCancelled(const void *context) {
            return static_cast<std::uint8_t>(static_cast<const CancellationToken *>(context)->IsCancellationRequested());
        }

        HoroExtensionStatus PublishActionResult(void *context, const HoroEditorActivitySnapshot *snapshot) noexcept {
            if (!context || !snapshot)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            try {
                auto &result = *static_cast<EditorActivitySession::ActionResult *>(context);
                auto copied = Detail::CopyActivityForm(*snapshot, result.drawerId, result.titleKey);
                if (copied.HasError())
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                const EditorActivityPresentation presentation{(snapshot->presentationFlags & HORO_EDITOR_ACTIVITY_HIDDEN) == 0,
                                                              (snapshot->presentationFlags & HORO_EDITOR_ACTIVITY_DISABLED) == 0,
                                                              snapshot->badgeCount};
                std::lock_guard lock{result.mutex};
                if (result.form || snapshot->revision <= result.revision)
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                result.form = std::move(copied).Value();
                result.presentation = presentation;
                result.revision = snapshot->revision;
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        /** @brief Publishes only completed owned results at the owner boundary, then releases the completed job handoff. */
        void PublishCompletedAction(EditorActivitySession &session) {
            if (session.result && session.result->ready.load(std::memory_order_acquire)) {
                const auto completed = session.result;
                std::lock_guard lock{completed->mutex};
                if (session.result->form && session.result->revision > session.revision) {
                    if (const auto host = session.host.lock();
                        host &&
                        host->Registry()
                            .PublishForm(session.drawer.surface.provider, session.drawer.surface.id, *session.result->form)
                            .HasValue() &&
                        host->Registry()
                            .PublishActivity(session.activity.surface.provider, session.activity.surface.id, session.result->presentation)
                            .HasValue()) {
                        session.form = std::move(*session.result->form);
                        session.presentation = session.result->presentation;
                        session.revision = session.result->revision;
                    }
                }
                session.job.reset();
                session.result.reset();
            }
        }

        /** @brief Executes one provider callback with a retirement lease owned by the scheduled closure. */
        Result<void> ExecuteAction(const std::shared_ptr<EditorActivitySession::ActionResult> &result,
                                   const EditorActivitySession::PendingAction &action, const HoroEditorActivityActionFunc invoke,
                                   void *moduleContext, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested()) {
                result->ready.store(true, std::memory_order_release);
                return JobCancelled();
            }
            HoroEditorActivityAction input{sizeof(HoroEditorActivityAction),
                                           {action.node.data(), static_cast<std::uint32_t>(action.node.size())},
                                           {action.action.data(), static_cast<std::uint32_t>(action.action.size())},
                                           action.revision,
                                           {&cancellation, ActionCancelled}};
            const HoroEditorActivitySnapshotSink sink{result.get(), PublishActionResult};
            HoroExtensionStatus status = HORO_EXTENSION_ERROR_INIT_FAILED;
            try {
                status = invoke(moduleContext, &input, &sink);
            } catch (...) {
                status = HORO_EXTENSION_ERROR_INIT_FAILED;
            }
            if (status != HORO_EXTENSION_SUCCESS || cancellation.IsCancellationRequested()) {
                std::lock_guard lock{result->mutex};
                result->form.reset();
            }
            result->ready.store(true, std::memory_order_release);
            if (cancellation.IsCancellationRequested() || status == HORO_EXTENSION_ERROR_CANCELLED)
                return JobCancelled();
            if (status != HORO_EXTENSION_SUCCESS)
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
            return Result<void>::Success();
        }

        void PumpAction(EditorActivitySession &session, const JobSystem &jobs, const std::shared_ptr<EditorActivitySession> &owner) {
            if (session.revoked || !session.committed)
                return;
            PublishCompletedAction(session);
            if (!session.pendingAction || session.job)
                return;
            const auto retirement = session.retirement.lock();
            const auto lease = retirement ? retirement->Acquire(session.activity.surface.provider.moduleId, ExtensionLeaseKind::Job,
                                                                session.activity.surface.id, owner)
                                          : nullptr;
            if (!lease) {
                session.pendingAction.reset();
                return;
            }
            auto action = std::move(*session.pendingAction);
            session.pendingAction.reset();
            auto result = std::make_shared<EditorActivitySession::ActionResult>();
            result->drawerId = session.drawer.surface.id;
            result->titleKey = session.drawer.surface.labelLocalizationKey;
            result->revision = session.revision;
            const auto invoke = session.invoke;
            void *const moduleContext = session.moduleContext;
            auto accepted = jobs.SubmitResult({}, [lease, result, action = std::move(action), invoke,
                                                   moduleContext](const CancellationToken &cancellation) {
                return ExecuteAction(result, action, invoke, moduleContext, cancellation);
            });
            if (accepted.HasValue()) {
                session.job.emplace(std::move(accepted).Value());
                session.result = std::move(result);
            }
        }
    }  // namespace

    /** @copydoc PumpEditorActivityAction */
    void PumpEditorActivityAction(EditorActivitySession &session, const JobSystem &jobs,
                                  const std::shared_ptr<EditorActivitySession> &owner) {
        PumpAction(session, jobs, owner);
    }
}  // namespace Horo::Extensions
