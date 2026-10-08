#include "Horo/Runtime/Camera/CameraService.h"

#include "Horo/Runtime/Camera/CameraErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime {
    /** @copydoc CameraService::Create */
    Result<CameraService> CameraService::Create(const CameraViewContext &context) {
        if (!context.view.IsValid() || !context.scene.IsValid() || context.domain >= CameraViewDomain::Count)
            return Result<CameraService>::Failure(MakeError(CameraErrors::InvalidContext));
        return Result<CameraService>::Success(CameraService{context});
    }

    CameraService::CameraService(CameraService &&other) noexcept
        : context_(other.context_), slots_(std::move(other.slots_)), committed_(std::move(other.committed_)),
          admissionOpen_(other.admissionOpen_) {
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
        if (!proposal.values.IsValid() || Math::TryLookAt(proposal.values.position, proposal.values.target, proposal.values.up).HasError())
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
        slots_[index.Value()].proposal = proposal;
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

    bool CameraService::ChangesSelection(const CameraProposal &proposal, const std::optional<CameraClaimId> &claim) const noexcept {
        return !committed_ || committed_->overrideOwner != claim || committed_->selected.camera != proposal.camera ||
               committed_->selected.discontinuityRevision != proposal.discontinuityRevision;
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

    /** @copydoc CameraService::Commit */
    Result<CameraSelectionSnapshot> CameraService::Commit(const std::uint64_t renderedFrame, const std::optional<CameraProposal> &base) {
        if (const auto valid = ValidateFrame(renderedFrame); valid.HasError())
            return Result<CameraSelectionSnapshot>::Failure(valid.ErrorValue());
        if (HasCommittedFrame(renderedFrame))
            return Result<CameraSelectionSnapshot>::Success(*committed_);
        const Slot *winner = WinningProposal();
        if (!winner && !EligibleBase(base))
            return Result<CameraSelectionSnapshot>::Failure(MakeError(CameraErrors::CameraUnavailable));
        const auto claim = winner ? std::optional{winner->request->claim} : std::nullopt;
        const auto &selected = winner ? *winner->proposal : *base;
        const bool changed = ChangesSelection(selected, claim);
        if (!CanAdvanceEpoch(changed))
            return Result<CameraSelectionSnapshot>::Failure(MakeError(CameraErrors::InvalidFrame));
        const auto epoch = committed_ ? committed_->selectionEpoch + static_cast<std::uint64_t>(changed) : 1;
        committed_.emplace(context_, renderedFrame, epoch, selected, claim, changed);
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
