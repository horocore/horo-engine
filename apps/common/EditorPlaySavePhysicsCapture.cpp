#include "EditorPlaySavePhysicsServices.h"
#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Application::Internal {
    /** @copydoc PlaySavePhysicsServices::CaptureReady */
    Result<bool> PlaySavePhysicsServices::CaptureReady() const {
        if (owner_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        const auto scene = scenes_.ActiveScene();
        if (!scene || !scene->IsCurrent())
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        auto target = native_.PublishedStateTarget();
        if (target.HasError())
            return Result<bool>::Failure(target.ErrorValue());
        if (target.Value().Scene() != scene->RuntimeId())
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        return target.Value().CaptureReady();
    }

    /** @copydoc PlaySavePhysicsServices::ValidateCaptureScene */
    Result<void> PlaySavePhysicsServices::ValidateCaptureScene(const Runtime::CanonicalCaptureContext &context) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        const auto scene = scenes_.ActiveScene();
        if (!scene || !scene->IsCurrent() || scene->RuntimeId().value != context.provenance.sceneIncarnation ||
            scene->StructuralRevision() != context.provenance.sceneRevision)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsServices::EnsureCapture */
    Result<void> PlaySavePhysicsServices::EnsureCapture(const Runtime::CanonicalCaptureContext &context) {
        if (auto current = ValidateCaptureScene(context); current.HasError())
            return current;
        if (provenance_ && *provenance_ == context.provenance && snapshot_.IsValid())
            return Result<void>::Success();
        if (!capture_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
        auto snapshot = capture_->Capture();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (snapshot.Value()->Bytes().size() > policy_.maximumWireBytes)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
        snapshot_ = std::move(snapshot).Value();
        provenance_ = context.provenance;
        return Result<void>::Success();
    }

    /** @copydoc PlaySavePhysicsServices::CaptureCoverage */
    Result<PlaySavePhysicsCaptureCoverage> PlaySavePhysicsServices::CaptureCoverage(const Runtime::CanonicalCaptureContext &context) {
        if (auto captured = EnsureCapture(context); captured.HasError())
            return Result<PlaySavePhysicsCaptureCoverage>::Failure(captured.ErrorValue());
        PlaySavePhysicsCaptureCoverage result;
        result.owner_ = shared_from_this();
        result.provenance_ = context.provenance;
        return Result<PlaySavePhysicsCaptureCoverage>::Success(std::move(result));
    }

    /** @copydoc PlaySavePhysicsServices::MatchesCapture */
    bool PlaySavePhysicsServices::MatchesCapture(Runtime::RuntimeSceneView scene,
                                                 const Runtime::RuntimeSaveCaptureProvenance &provenance) const noexcept {
        if (owner_ != std::this_thread::get_id())
            return false;
        return scene.IsCurrent() && provenance_ && *provenance_ == provenance && snapshot_.IsValid() &&
               scene.RuntimeId().value == provenance.sceneIncarnation && scene.StructuralRevision() == provenance.sceneRevision;
    }

    /** @copydoc PlaySavePhysicsCaptureCoverage::Matches */
    bool PlaySavePhysicsCaptureCoverage::Matches(Runtime::RuntimeSceneView scene) const noexcept {
        const auto owner = owner_.lock();
        return owner && owner->MatchesCapture(scene, provenance_);
    }

    /** @copydoc PlaySavePhysicsServices::Capture */
    Result<Runtime::CanonicalCaptureDisposition> PlaySavePhysicsServices::Capture(const Runtime::CanonicalCaptureContext &context,
                                                                                  Runtime::ICanonicalCaptureSink &sink) {
        using Outcome = Result<Runtime::CanonicalCaptureDisposition>;
        if (auto captured = EnsureCapture(context); captured.HasError())
            return Outcome::Failure(captured.ErrorValue());
        if (auto copied = sink.WriteCopied(Physics::Detail::PhysicsStateSaveRecord(), snapshot_->Bytes()); copied.HasError())
            return Outcome::Failure(copied.ErrorValue());
        snapshot_ = {};
        provenance_.reset();
        return Outcome::Success(Runtime::CanonicalCaptureDisposition::Captured);
    }
}  // namespace Horo::Application::Internal
