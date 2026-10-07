#include "EditorActivitySessionState.h"

#include <exception>

// These C endpoints have C name linkage but are not part of the exported SDK.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif
extern "C" {
/** @brief C ABI cancellation adapter; the token borrow ends when the provider returns. */
std::uint8_t HoroActivityActionCancelled(const void *context) noexcept {
    return static_cast<std::uint8_t>(static_cast<const Horo::CancellationToken *>(context)->IsCancellationRequested());
}

/** @brief C ABI sink copies provider bytes into the strongly typed owned worker result. */
HoroExtensionStatus HoroPublishActivityActionResult(void *context, const HoroEditorActivitySnapshot *snapshot) noexcept {
    if (!context || !snapshot)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    try {
        auto &result = *static_cast<Horo::Extensions::EditorActivitySession::ActionResult *>(context);
        auto copied = Horo::Extensions::Detail::CopyActivityForm(*snapshot, result.drawerId, result.titleKey);
        if (copied.HasError())
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        const Horo::Extensions::EditorActivityPresentation presentation{(snapshot->presentationFlags & HORO_EDITOR_ACTIVITY_HIDDEN) == 0,
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
}  // extern "C"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

namespace Horo::Extensions {
    namespace {
        static_assert(std::is_convertible_v<decltype(&HoroPublishActivityActionResult), decltype(HoroEditorActivitySnapshotSink::publish)>);
        static_assert(
            std::is_convertible_v<decltype(&HoroActivityActionCancelled), decltype(HoroExtensionCancellation::isCancellationRequested)>);

        /** @brief Publishes only completed owned results at the owner boundary, then releases the completed job handoff. */
        void PublishCompletedAction(EditorActivitySession &session) {
            if (session.actions.result && session.actions.result->ready.load(std::memory_order_acquire)) {
                const auto completed = session.actions.result;
                std::lock_guard lock{completed->mutex};
                if (session.actions.result->form && session.actions.result->revision > session.revision) {
                    if (const auto host = session.host.lock();
                        host &&
                        host->Registry()
                            .PublishForm(session.drawer.surface.provider, session.drawer.surface.id, *session.actions.result->form)
                            .HasValue() &&
                        host->Registry()
                            .PublishActivity(session.activity.surface.provider, session.activity.surface.id,
                                             session.actions.result->presentation)
                            .HasValue()) {
                        session.form = std::move(*session.actions.result->form);
                        session.presentation = session.actions.result->presentation;
                        session.revision = session.actions.result->revision;
                    }
                }
                session.actions.job.reset();
                session.actions.result.reset();
            }
        }

        /** @brief Executes one provider callback with a retirement lease owned by the scheduled closure. */
        Result<void> ExecuteAction(const std::shared_ptr<EditorActivitySession::ActionResult> &result,
                                   const EditorActivitySession::PendingAction &action, const EditorActivityProviderAction &provider,
                                   const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested()) {
                result->ready.store(true, std::memory_order_release);
                return JobCancelled();
            }
            HoroEditorActivityAction input{sizeof(HoroEditorActivityAction),
                                           {action.node.data(), static_cast<std::uint32_t>(action.node.size())},
                                           {action.action.data(), static_cast<std::uint32_t>(action.action.size())},
                                           action.revision,
                                           {&cancellation, HoroActivityActionCancelled}};
            const auto sink = MakeEditorActivityResultSink(*result);
            const HoroExtensionStatus status = provider.Invoke(input, sink);
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
            if (!session.actions.pending || session.actions.job)
                return;
            const auto retirement = session.retirement.lock();
            const auto lease = retirement ? retirement->Acquire(session.activity.surface.provider.moduleId, ExtensionLeaseKind::Job,
                                                                session.activity.surface.id, owner)
                                          : nullptr;
            if (!lease) {
                session.actions.pending.reset();
                return;
            }
            auto action = std::move(*session.actions.pending);
            session.actions.pending.reset();
            auto result = std::make_shared<EditorActivitySession::ActionResult>();
            result->drawerId = session.drawer.surface.id;
            result->titleKey = session.drawer.surface.labelLocalizationKey;
            result->revision = session.revision;
            const auto provider = session.actions.provider;
            auto accepted =
                jobs.SubmitResult({}, [lease, result, action = std::move(action), provider](const CancellationToken &cancellation) {
                return ExecuteAction(result, action, provider, cancellation);
            });
            if (accepted.HasValue()) {
                session.actions.job.emplace(std::move(accepted).Value());
                session.actions.result = std::move(result);
            }
        }
    }  // namespace

    /** @copydoc MakeEditorActivityResultSink */
    HoroEditorActivitySnapshotSink MakeEditorActivityResultSink(EditorActivitySession::ActionResult &result) noexcept {
        return {&result, HoroPublishActivityActionResult};
    }

    /** @copydoc EditorActivityProviderAction::Invoke */
    HoroExtensionStatus EditorActivityProviderAction::Invoke(const HoroEditorActivityAction &action,
                                                             const HoroEditorActivitySnapshotSink &sink) const noexcept {
        if (!invoke_)
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        try {
            return invoke_(context_, &action, &sink);
        } catch (const std::exception &) {
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        } catch (...) {
            // Contract-violating non-standard throws must not cross the foreign invocation boundary either.
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        }
    }

    /** @copydoc PumpEditorActivityAction */
    void PumpEditorActivityAction(EditorActivitySession &session, const JobSystem &jobs,
                                  const std::shared_ptr<EditorActivitySession> &owner) {
        PumpAction(session, jobs, owner);
    }
}  // namespace Horo::Extensions
