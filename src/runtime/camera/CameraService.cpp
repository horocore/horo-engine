#include "Horo/Runtime/Camera/CameraService.h"

#include "Horo/Runtime/Camera/CameraErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Runtime {
    namespace {
        /** @brief Converts a valid view orientation to the shared scene quaternion convention. */
        Result<Math::Quaternion> CameraRotation(const Render::RenderCameraView &view) {
            const auto matrix = Math::TryLookAt(view.position, view.target, view.up);
            if (matrix.HasError())
                return Result<Math::Quaternion>::Failure(matrix.ErrorValue());
            const auto inverse = Math::TryInverseAffine(matrix.Value());
            if (inverse.HasError())
                return Result<Math::Quaternion>::Failure(inverse.ErrorValue());
            const auto pose = Math::TryDecomposeAffineTRS(inverse.Value());
            if (pose.HasError())
                return Result<Math::Quaternion>::Failure(pose.ErrorValue());
            return Result<Math::Quaternion>::Success(pose.Value().rotation);
        }

        /** @brief Interpolates view orientation while retaining the exact failing math operation. */
        Result<Math::Quaternion> BlendRotation(const Render::RenderCameraView &from, const Render::RenderCameraView &to,
                                               const float weight) {
            const auto source = CameraRotation(from);
            if (source.HasError())
                return Result<Math::Quaternion>::Failure(source.ErrorValue());
            const auto destination = CameraRotation(to);
            if (destination.HasError())
                return Result<Math::Quaternion>::Failure(destination.ErrorValue());
            return Math::TrySlerp(source.Value(), destination.Value(), weight);
        }

        /** @brief Exposure of the extra view changes history identity; ordinary interpolation of weight does not. */
        bool SameCrossFadeSource(const std::optional<CameraCrossFadeView> &previous,
                                 const std::optional<CameraCrossFadeView> &next) noexcept {
            if (!previous || !next)
                return previous.has_value() == next.has_value();
            return previous->sourceCamera == next->sourceCamera;
        }
    }  // namespace

    /** @copydoc CameraService::Create */
    Result<CameraService> CameraService::Create(const CameraViewContext &context, const CameraTransitionSupport transitions) {
        return Create(context, transitions, std::nullopt);
    }

    /** @copydoc CameraService::Create(const CameraViewContext &, CameraTransitionSupport, std::optional<CameraCrossFadeAdmission>) */
    Result<CameraService> CameraService::Create(const CameraViewContext &context, const CameraTransitionSupport transitions,
                                                const std::optional<CameraCrossFadeAdmission> crossFade) {
        if (!context.view.IsValid() || !context.scene.IsValid() || context.domain >= CameraViewDomain::Count ||
            transitions >= CameraTransitionSupport::Count)
            return Result<CameraService>::Failure(MakeError(CameraErrors::InvalidContext));
        if (crossFade && (crossFade->context != context || crossFade->generation == 0))
            return Result<CameraService>::Failure(MakeError(CameraErrors::InvalidContext));
        return Result<CameraService>::Success(CameraService{context, transitions, crossFade});
    }

    CameraService::CameraService(CameraService &&other) noexcept
        : context_(other.context_), transitions_(other.transitions_), crossFade_(other.crossFade_), slots_(std::move(other.slots_)),
          committed_(std::move(other.committed_)), admissionOpen_(other.admissionOpen_) {
        other.Shutdown();
    }

    /** @copydoc CameraService::Acquire */
    Result<CameraOverrideLease> CameraService::Acquire(const CameraOverrideRequest &request) {
        if (!admissionOpen_)
            return Result<CameraOverrideLease>::Failure(MakeError(CameraErrors::Closed));
        if (request.context != context_ || !request.claim.IsValid())
            return Result<CameraOverrideLease>::Failure(MakeError(CameraErrors::InvalidContext));
        if (std::ranges::any_of(slots_, [&](const Slot &slot) {
            return slot.request && slot.request->claim == request.claim;
        }))
            return Result<CameraOverrideLease>::Failure(MakeError(CameraErrors::DuplicateClaim));
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto &slot = slots_[index];
            if (slot.request || slot.generation == 0)
                continue;
            slot.request = request;
            return Result<CameraOverrideLease>::Success({context_, request.claim, static_cast<std::uint32_t>(index), slot.generation});
        }
        return Result<CameraOverrideLease>::Failure(MakeError(CameraErrors::CapacityExceeded));
    }

    Result<std::size_t> CameraService::Resolve(const CameraOverrideLease &lease) const {
        if (!admissionOpen_)
            return Result<std::size_t>::Failure(MakeError(CameraErrors::Closed));
        if (lease.context != context_ || lease.slot >= slots_.size())
            return Result<std::size_t>::Failure(MakeError(CameraErrors::InvalidLease));
        if (const auto &slot = slots_[lease.slot];
            !slot.request || slot.generation != lease.generation || slot.request->claim != lease.claim)
            return Result<std::size_t>::Failure(MakeError(CameraErrors::InvalidLease));
        return Result<std::size_t>::Success(lease.slot);
    }

    bool CameraService::ValidProposal(const CameraProposal &proposal, const bool isOverride) const noexcept {
        if (proposal.sceneRevision == 0 || proposal.transitionResolution >= CameraTransitionResolution::Count ||
            !proposal.values.IsValid() || Math::TryLookAt(proposal.values.position, proposal.values.target, proposal.values.up).HasError())
            return false;
        if (!proposal.camera.IsValid())
            return proposal.camera == EntityRef{} && !isOverride && context_.domain == CameraViewDomain::EditorPreview;
        return proposal.camera.runtime == context_.scene;
    }

    /** @copydoc CameraService::Submit */
    Result<void> CameraService::Submit(const CameraOverrideLease &lease, const CameraProposal &proposal) {
        const auto index = Resolve(lease);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (!ValidProposal(proposal, true))
            return Result<void>::Failure(MakeError(CameraErrors::InvalidTarget));
        slots_[index.Value()].proposal.emplace(proposal, std::nullopt);
        return Result<void>::Success();
    }

    /** @copydoc CameraService::ValidateTransitionInputs */
    Result<void> CameraService::ValidateTransitionInputs(const CameraProposal &from, const CameraProposal &to, const float weight) const {
        if (!ValidProposal(from, true) || !ValidProposal(to, true) || !std::isfinite(weight) || weight < 0.0F || weight > 1.0F)
            return Result<void>::Failure(MakeError(CameraErrors::InvalidTarget));
        if (from.sceneRevision != to.sceneRevision)
            return Result<void>::Failure(MakeError(CameraErrors::StaleFrame));
        return Result<void>::Success();
    }

    /** @copydoc CameraService::ValidateBlend */
    Result<void> CameraService::ValidateBlend(const CameraProposal &from, const CameraProposal &to, const float weight) const {
        if (transitions_ != CameraTransitionSupport::SingleViewBlend)
            return Result<void>::Failure(MakeError(CameraErrors::UnsupportedTransition));
        if (const auto valid = ValidateTransitionInputs(from, to, weight); valid.HasError())
            return valid;
        if (from.values.projection.kind != to.values.projection.kind)
            return Result<void>::Failure(MakeError(CameraErrors::IncompatibleTransition));
        return Result<void>::Success();
    }

    /** @copydoc CameraService::SubmitBlend */
    Result<void> CameraService::SubmitBlend(const CameraOverrideLease &lease, const CameraProposal &from, const CameraProposal &to,
                                            const float weight) {
        const auto index = Resolve(lease);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (const auto valid = ValidateBlend(from, to, weight); valid.HasError())
            return valid;
        const auto rotation = BlendRotation(from.values, to.values, weight);
        if (rotation.HasError())
            return Result<void>::Failure(WithCause(MakeError(CameraErrors::IncompatibleTransition), rotation.ErrorValue()));
        auto blended = to;
        blended.transitionResolution = CameraTransitionResolution::Requested;
        auto &values = blended.values;
        values.position = from.values.position * (1.0F - weight) + to.values.position * weight;
        values.target = values.position + rotation.Value().Rotate({0.0F, 0.0F, -1.0F});
        values.up = rotation.Value().Rotate({0.0F, 1.0F, 0.0F});
        const auto &a = from.values.projection;
        const auto &b = to.values.projection;
        if (b.kind == Render::RenderProjectionKind::Perspective)
            values.projection.verticalFovRadians = std::lerp(a.verticalFovRadians, b.verticalFovRadians, weight);
        else
            values.projection.orthographicHeight = std::lerp(a.orthographicHeight, b.orthographicHeight, weight);
        values.projection.nearPlane = std::lerp(a.nearPlane, b.nearPlane, weight);
        values.projection.farPlane = std::lerp(a.farPlane, b.farPlane, weight);
        if (!ValidProposal(blended, true))
            return Result<void>::Failure(MakeError(CameraErrors::IncompatibleTransition));
        slots_[index.Value()].proposal.emplace(blended, std::nullopt);
        return Result<void>::Success();
    }

    /** @copydoc CameraService::CheckCrossFadeAdmission */
    Result<void> CameraService::CheckCrossFadeAdmission() const {
        if (!admissionOpen_)
            return Result<void>::Failure(MakeError(CameraErrors::Closed));
        if (!crossFade_)
            return Result<void>::Failure(MakeError(CameraErrors::UnsupportedTransition));
        if (crossFade_->availableViews < 2)
            return Result<void>::Failure(MakeError(CameraErrors::TransitionBudgetExceeded));
        return Result<void>::Success();
    }

    /** @copydoc CameraService::SubmitCrossFade */
    Result<void> CameraService::SubmitCrossFade(const CameraOverrideLease &lease, const CameraProposal &from, const CameraProposal &to,
                                                const float weight) {
        const auto index = Resolve(lease);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (const auto admitted = CheckCrossFadeAdmission(); admitted.HasError())
            return admitted;
        if (const auto valid = ValidateTransitionInputs(from, to, weight); valid.HasError())
            return valid;
        const CameraCrossFadeView view{from.camera, from.values, weight, crossFade_->generation};
        slots_[index.Value()].proposal.emplace(to, view);
        return Result<void>::Success();
    }

    /** @copydoc CameraService::Withdraw */
    Result<void> CameraService::Withdraw(const CameraOverrideLease &lease) {
        const auto index = Resolve(lease);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        slots_[index.Value()].proposal.reset();
        return Result<void>::Success();
    }

    /** @copydoc CameraService::Release */
    Result<void> CameraService::Release(const CameraOverrideLease &lease) {
        const auto index = Resolve(lease);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &slot = slots_[index.Value()];
        slot.request.reset();
        slot.proposal.reset();
        slot.generation = slot.generation == std::numeric_limits<std::uint32_t>::max() ? 0 : slot.generation + 1;
        return Result<void>::Success();
    }

    const CameraService::Slot *CameraService::WinningProposal() const noexcept {
        const Slot *winner = nullptr;
        for (const auto &slot : slots_) {
            if (!slot.request || !slot.proposal)
                continue;
            if (!winner || slot.request->priority > winner->request->priority ||
                (slot.request->priority == winner->request->priority && slot.request->claim < winner->request->claim))
                winner = &slot;
        }
        return winner;
    }

    Result<void> CameraService::ValidateFrame(const std::uint64_t renderedFrame) const {
        if (!admissionOpen_)
            return Result<void>::Failure(MakeError(CameraErrors::Closed));
        if (renderedFrame == 0 || (committed_ && renderedFrame < committed_->renderedFrame))
            return Result<void>::Failure(MakeError(CameraErrors::InvalidFrame));
        return Result<void>::Success();
    }

    bool CameraService::ChangesSelection(const CameraProposal &proposal, const std::optional<CameraClaimId> &claim,
                                         const std::optional<CameraCrossFadeView> &crossFade) const noexcept {
        return !committed_ || committed_->overrideOwner != claim || committed_->selected.camera != proposal.camera ||
               committed_->selected.discontinuityRevision != proposal.discontinuityRevision ||
               committed_->selected.transitionResolution != proposal.transitionResolution ||
               !SameCrossFadeSource(committed_->crossFade, crossFade);
    }

    bool CameraService::CanAdvanceEpoch(const bool changed) const noexcept {
        return !changed || !committed_ || committed_->selectionEpoch != std::numeric_limits<std::uint64_t>::max();
    }

    bool CameraService::HasCommittedFrame(const std::uint64_t renderedFrame) const noexcept {
        return committed_ && renderedFrame == committed_->renderedFrame;
    }

    bool CameraService::EligibleBase(const std::optional<CameraProposal> &base) const noexcept {
        return base && ValidProposal(*base, false);
    }

    /** @copydoc CameraService::ValidateSceneFrame */
    Result<void> CameraService::ValidateSceneFrame(const std::uint64_t renderedFrame, const RuntimeSceneView scene) const {
        if (const auto valid = ValidateFrame(renderedFrame); valid.HasError())
            return valid;
        if (!scene.IsCurrent() || scene.RuntimeId() != context_.scene)
            return Result<void>::Failure(MakeError(CameraErrors::InvalidContext));
        if (HasCommittedFrame(renderedFrame) && committed_->selected.sceneRevision != scene.StructuralRevision())
            return Result<void>::Failure(MakeError(CameraErrors::StaleFrame));
        return Result<void>::Success();
    }

    /** @copydoc CameraService::Commit */
    Result<CameraSelectionSnapshot> CameraService::Commit(const std::uint64_t renderedFrame, const RuntimeSceneView scene,
                                                          const std::optional<CameraProposal> &base) {
        if (const auto valid = ValidateSceneFrame(renderedFrame, scene); valid.HasError())
            return Result<CameraSelectionSnapshot>::Failure(valid.ErrorValue());
        const auto sceneRevision = scene.StructuralRevision();
        if (HasCommittedFrame(renderedFrame))
            return Result<CameraSelectionSnapshot>::Success(*committed_);
        const Slot *winner = WinningProposal();
        if (!winner && !EligibleBase(base))
            return Result<CameraSelectionSnapshot>::Failure(MakeError(CameraErrors::CameraUnavailable));
        const auto claim = winner ? std::optional{winner->request->claim} : std::nullopt;
        const auto &selected = winner ? winner->proposal->selected : *base;
        if (selected.sceneRevision != sceneRevision)
            return Result<CameraSelectionSnapshot>::Failure(MakeError(CameraErrors::StaleFrame));
        const auto crossFade = winner ? winner->proposal->crossFade : std::nullopt;
        const bool changed = ChangesSelection(selected, claim, crossFade);
        if (!CanAdvanceEpoch(changed))
            return Result<CameraSelectionSnapshot>::Failure(MakeError(CameraErrors::InvalidFrame));
        const auto epoch = committed_ ? committed_->selectionEpoch + static_cast<std::uint64_t>(changed) : 1;
        committed_.emplace(context_, renderedFrame, epoch, selected, claim, changed, crossFade);
        return Result<CameraSelectionSnapshot>::Success(*committed_);
    }

    /** @copydoc CameraService::Shutdown */
    void CameraService::Shutdown() noexcept {
        admissionOpen_ = false;
        for (auto &slot : slots_) {
            slot.request.reset();
            slot.proposal.reset();
        }
    }
}  // namespace Horo::Runtime
