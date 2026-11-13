#include "EditorPlaySaveHostInternal.h"
#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Application::Internal {
    namespace {
        /** @brief Owner-only request preparation; session clears it before any worker dispatch.
         * Borrowed host state remains retained by the existing Save session retirement contract.
         */
        class PhysicsCapturePreparation final : public Runtime::IRuntimeSaveCapturePreparation {
        public:
            using PrepareFunction = std::function<Result<void>()>;
            using Observe = std::function<Result<Runtime::RuntimeSaveCapturePreparationEvidence>()>;

            PhysicsCapturePreparation(PrepareFunction prepare, Observe observe)
                : prepare_(std::move(prepare)), observe_(std::move(observe)) {}

            Result<Runtime::RuntimeSaveCapturePreparationEvidence> Prepare(
                const Runtime::RuntimeSaveCapturePreparationContext &context) override {
                using Outcome = Result<Runtime::RuntimeSaveCapturePreparationEvidence>;
                if (context.cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(Runtime::SaveErrors::OperationCancelled));
                auto observed = observe_();
                if (observed.HasError())
                    return Outcome::Failure(observed.ErrorValue());
                if (!observed.Value().ready)
                    return observed;
                if (auto prepared = prepare_(); prepared.HasError())
                    return Outcome::Failure(prepared.ErrorValue());
                return observe_();  // Re-read actual host authority after cold preparation; never echo expected values.
            }

        private:
            PrepareFunction prepare_;
            Observe observe_;
        };
    }  // namespace

    /** @copydoc EditorPlaySaveHost::State::RegisterPhysicsCapture */
    Result<void> EditorPlaySaveHost::State::RegisterPhysicsCapture() {
        if (!schema->physics)
            return Result<void>::Success();
        if (!physics || *schema->physics != Physics::Detail::PhysicsStateSaveDescriptor())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
        const auto added = registry.Register(*schema->physics, std::make_shared<PlaySavePhysicsCapture>(physics));
        return added.HasError() ? Result<void>::Failure(added.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::ReleasePhysicsCapture */
    Result<void> EditorPlaySaveHost::State::ReleasePhysicsCapture() {
        if (!physics)
            return Result<void>::Success();
        if (const auto removed = registry.Unregister(Physics::Detail::PhysicsStateSaveParticipant()); removed.HasError())
            return Result<void>::Failure(removed.ErrorValue());
        physics->CloseCapture();
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::CapturePreparation */
    std::shared_ptr<Runtime::IRuntimeSaveCapturePreparation> EditorPlaySaveHost::State::CapturePreparation() {
        if (!physics)
            return {};
        return std::make_shared<PhysicsCapturePreparation>([this]() -> Result<void> {
            if (retiring || suspending || std::this_thread::get_id() != owner)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
            auto current = CurrentIdentities();
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            return physics->PrepareCapture(current.Value());
        }, [this]() -> Result<Runtime::RuntimeSaveCapturePreparationEvidence> {
            using Outcome = Result<Runtime::RuntimeSaveCapturePreparationEvidence>;
            if (retiring || suspending || std::this_thread::get_id() != owner)
                return Outcome::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
            auto ready = physics->CaptureReady();
            if (ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            return Outcome::Success({authority->Generation(), Provenance(), ready.Value()});
        });
    }
}  // namespace Horo::Application::Internal
