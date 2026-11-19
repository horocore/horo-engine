#include "NetworkWorldBindingState.h"

#include <new>

namespace Horo::Network {
    /** @copydoc NetworkWorldBindingController::Impl::AuthoredSent */
    bool NetworkWorldBindingController::Impl::AuthoredSent(const Peer &peer, NetworkSessionGeneration session) const noexcept {
        if (!setup.policy.authoredScene)
            return true;
        return session.IsValid() &&
               (peer.authoredPublished == session || (peer.authoredSent.session == session && peer.authoredSent.complete));
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueAuthoredUpdate */
    Result<void> NetworkWorldBindingController::Impl::QueueAuthoredUpdate(Peer &peer, std::uint64_t tick) const {
        if (!setup.policy.authoredScene || !peer.sendsWorld || peer.frameBytes != 0 || !peer.sent)
            return Result<void>::Success();
        const auto session = peer.sent->worldSession;
        const auto *inventory = worlds.AuthorityAuthored(session);
        if (!inventory || AuthoredSent(peer, session))
            return Result<void>::Success();
        const std::size_t first = peer.authoredSent.session == session ? peer.authoredSent.next : 0;
        const auto &identity = setup.policy.authoredScene->control;
        const auto codec = setup.codecs->FindCodec(identity.protocol, identity.id);
        if (!codec)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        const auto available = (codec->maximumPayloadBytes - MinimumNetworkAuthoredScenePageBytes) / NetworkAuthoredSceneRowBytes;
        // The codec limits payload bytes. The transport also counts the enclosing Control header.
        // Try progressively smaller pages against the same encoder used by the authenticated lane.
        for (std::size_t capacity = available; capacity != 0; capacity /= 2) {
            auto page = MakeNetworkAuthoredScenePage(*inventory, first, capacity);
            if (page.HasError())
                return Result<void>::Failure(page.ErrorValue());
            auto queued = QueueAuthoredPage(peer, page.Value(), tick);
            const auto &capacityError = NetworkErrors::MessageEnvelopeCapacityExceeded;
            if (queued.HasValue() || queued.ErrorValue().domain.Value() != capacityError.domain.Value() ||
                queued.ErrorValue().code.Value() != capacityError.code.Value())
                return queued;
        }
        return Result<void>::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueAuthoredPage */
    Result<void> NetworkWorldBindingController::Impl::QueueAuthoredPage(Peer &peer, const NetworkAuthoredScenePage &page,
                                                                        std::uint64_t tick) const {
        const auto payload = EncodeNetworkAuthoredScenePage(page);
        if (payload.HasError())
            return Result<void>::Failure(payload.ErrorValue());
        try {
            peer.extendedFrame.resize(payload.Value().size() + peer.frame.size());
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
        const auto &identity = setup.policy.authoredScene->control;
        const MessageEnvelopePayloadView message{identity.protocol, identity.id, identity.schema, identity.version, {}, {}, 0,
                                                 payload.Value()};
        if (auto queued = QueueFrame(peer, message, tick, peer.extendedFrame); queued.HasError())
            return queued;
        const auto next = page.firstIndex + page.objects.size();
        peer.authoredPending = Peer::AuthoredProgress{page.session, next, next == page.totalObjects};
        return Result<void>::Success();
    }
}  // namespace Horo::Network
