#include "NetworkWorldBindingState.h"

#include <algorithm>

namespace Horo::Network {
    /** @copydoc NetworkWorldBindingController::PrepareTravel */
    Result<void> NetworkWorldBindingController::PrepareTravel(const std::span<const NetworkModeWorld> worlds) {
        if (auto checked = Reconcile(impl_->nowTick); checked.HasError())
            return checked;
        for (const auto &peer : impl_->peers) {
            if (peer.lifecycle && peer.sent && peer.sent->kind == NetworkWorldBindingKind::PrepareTravel &&
                impl_->nowTick < peer.preparedDeadline)
                return Result<void>::Failure(
                    MakeError(NetworkErrors::NetworkModeUnavailable, "A cancelled remote prepare retains its finite expiry fence."));
        }
        return impl_->worlds.PrepareTravel(worlds, impl_->nowTick, impl_->setup.maximumPendingTicks);
    }

    /** @copydoc NetworkWorldBindingController::TravelPending */
    bool NetworkWorldBindingController::TravelPending() const noexcept {
        return impl_->worlds.TravelPending();
    }

    /** @copydoc NetworkWorldBindingController::TravelStatus */
    Result<void> NetworkWorldBindingController::TravelStatus() const {
        return impl_->worlds.TravelStatus();
    }

    /** @copydoc NetworkWorldBindingController::TravelReady */
    bool NetworkWorldBindingController::TravelReady() const noexcept {
        if (impl_->stopped || !impl_->worlds.TravelReady())
            return false;
        const auto *prepared = impl_->worlds.PreparedServerBinding();
        if (!prepared)
            return true;  // A remote client still requires the matching authenticated CommitTravel.
        return std::ranges::none_of(impl_->peers, [prepared](const Impl::Peer &peer) {
            return peer.lifecycle && peer.sendsWorld && (peer.frameBytes != 0 || peer.sent != *prepared);
        });
    }

    /** @copydoc NetworkWorldBindingController::Commit */
    Result<void> NetworkWorldBindingController::Commit(const Runtime::RuntimePhase phase) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldPhaseInvalid));
        const auto *prepared = impl_->worlds.PreparedServerBinding();
        std::optional<NetworkWorldBindingRecord> reservedCommit;
        if (prepared) {
            if (!TravelReady())
                return Result<void>::Success();
            auto commit = *prepared;
            commit.kind = NetworkWorldBindingKind::CommitTravel;
            reservedCommit = commit;
            // All peers' bounded slots are vacant and PREPARE was accepted by native send.
            // Reserve/encode every COMMIT before the local aggregate can become visible.
            for (auto &peer : impl_->peers) {
                if (!peer.lifecycle || !peer.sendsWorld)
                    continue;
                if (auto queued = impl_->Queue(peer, commit, impl_->nowTick); queued.HasError()) {
                    impl_->ReleaseCommitReservations(commit);
                    impl_->worlds.CancelTravel();
                    return queued;
                }
            }
        }
        auto committed = impl_->worlds.Commit(phase);
        if (committed.HasError()) {
            if (reservedCommit)
                impl_->ReleaseCommitReservations(*reservedCommit);
            impl_->worlds.CancelTravel();
        }
        // No network send occurs here. After local publication each exact reserved
        // sequence remains owned until accepted, deadline, or typed peer failure.
        return committed;
    }

    void NetworkWorldBindingController::Impl::ReleaseCommitReservations(const NetworkWorldBindingRecord &commit) noexcept {
        for (auto &peer : peers) {
            if (peer.sendsWorld && peer.pending == commit) {
                peer.pending.reset();
                peer.frameBytes = 0;
            }
        }
    }

    /** @copydoc NetworkWorldBindingController::CancelTravel */
    Result<void> NetworkWorldBindingController::CancelTravel() {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        impl_->worlds.CancelTravel();
        // Only native-accepted frames consume a sequence. Discarding an unsent
        // PREPARE preserves its next sequence; a remote prepared candidate expires.
        for (auto &peer : impl_->peers) {
            if (peer.pending && peer.pending->kind == NetworkWorldBindingKind::PrepareTravel) {
                peer.pending.reset();
                peer.frameBytes = 0;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Network
