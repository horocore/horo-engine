#include "NetworkWorldBindingState.h"

#include <algorithm>
#include <new>

namespace Horo::Network {
    /** @copydoc NetworkWorldBindingController::Create */
    Result<std::shared_ptr<NetworkWorldBindingController>> NetworkWorldBindingController::Create(NetworkModePlan plan,
                                                                                                 NetworkWorldBindingSetup setup) {
        if (const auto codec =
                setup.codecs ? setup.codecs->FindCodec(setup.policy.control.protocol, setup.policy.control.id) : std::nullopt;
            !codec || codec->schema != setup.policy.control.schema || !codec->supportedVersions.Contains(setup.policy.control.version) ||
            codec->maximumPayloadBytes != NetworkWorldBindingWireBytes || setup.schemaFingerprint == 0 || !setup.policy.feature.IsValid() ||
            !setup.policy.expectedServer.IsValid() || !setup.policy.expectedProvenance.IsValid() ||
            !setup.policy.authorityPublisher.IsValid() || setup.maximumPeers == 0 || setup.maximumPeers > 64 ||
            setup.maximumPendingTicks == 0 || plan.mode == NetworkProjectRole::Standalone)
            return Result<std::shared_ptr<NetworkWorldBindingController>>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        try {
            auto implementation = std::make_unique<Impl>(std::move(plan), std::move(setup));
            return Result<std::shared_ptr<NetworkWorldBindingController>>::Success(
                std::make_shared<NetworkWorldBindingController>(ConstructionKey{}, std::move(implementation)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<NetworkWorldBindingController>>::Failure(
                MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
    }

    /** @copydoc NetworkWorldBindingController::NetworkWorldBindingController */
    NetworkWorldBindingController::NetworkWorldBindingController(ConstructionKey, std::unique_ptr<Impl> implementation) noexcept
        : impl_(std::move(implementation)) {}

    NetworkWorldBindingController::~NetworkWorldBindingController() {
        Shutdown();
    }

    Result<void> NetworkWorldBindingController::Impl::CheckOwner() const {
        if (stopped || busy || std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Prepare */
    Result<void> NetworkWorldBindingController::Prepare() {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        return impl_->worlds.Prepare();
    }

    /** @copydoc NetworkWorldBindingController::Attach */
    Result<void> NetworkWorldBindingController::Attach(INetworkTransport &transport, InboundMessageDispatcher &dispatcher,
                                                       const std::shared_ptr<NetworkWorldBindingController> &owner) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (owner.get() != this || impl_->transport || impl_->detached || !dispatcher.UsesCodecRegistry(*impl_->setup.codecs) ||
            !impl_->setup.controlChannel.IsWithin(transport.Capabilities().maximumChannels))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        const auto &control = impl_->setup.policy.control;
        if (auto registered = dispatcher.RegisterHandler({control.protocol, control.id, MessageTrafficClass::Control,
                                                          Runtime::RuntimePhase::NetworkPoll, 4},
                                                         owner);
            registered.HasError())
            return registered;
        dispatcher.worldBinding_ = owner;
        dispatcher.requiresWorldBinding_ = true;
        impl_->transport = &transport;
        impl_->dispatcher = &dispatcher;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Detach */
    void NetworkWorldBindingController::Detach(const INetworkTransport &transport) noexcept {
        if (impl_->transport != &transport)
            return;
        if (impl_->dispatcher)
            impl_->dispatcher->RevokeHandler(impl_->setup.policy.control.protocol, impl_->setup.policy.control.id);
        impl_->dispatcher = nullptr;
        impl_->transport = nullptr;
        impl_->detached = true;
        impl_->ClearPending();
    }

    /** @copydoc NetworkWorldBindingController::UnbindClient */
    Result<void> NetworkWorldBindingController::UnbindClient(const NetworkOperationGeneration generation) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        return impl_->worlds.UnbindClient(generation);
    }

    /** @copydoc NetworkWorldBindingController::Handle */
    Result<void> NetworkWorldBindingController::Handle(const MessageEnvelope &) {
        return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
    }

    /** @copydoc NetworkWorldBindingController::HandleAdmitted */
    Result<void> NetworkWorldBindingController::HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (const auto &control = impl_->setup.policy.control; !impl_->transport || message.protocol != control.protocol ||
                                                               message.message != control.id || message.schema != control.schema ||
                                                               message.schemaVersion != control.version || !message.fields.empty())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        const auto decoded = DecodeNetworkWorldBinding(message.payload);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        return impl_->worlds.Accept(decoded.Value(), context);
    }

    /** @copydoc NetworkWorldBindingController::Ready */
    bool NetworkWorldBindingController::Ready(const NetworkModeWorldKind kind, const Runtime::SceneRuntimeId scene) const noexcept {
        return impl_->worlds.Ready(kind, scene);
    }

    /** @copydoc NetworkWorldBindingController::Role */
    Result<NetworkModeRoleView> NetworkWorldBindingController::Role(const NetworkModeWorldKind kind,
                                                                    const Runtime::SceneRuntimeId scene) const {
        return impl_->worlds.Role(kind, scene);
    }

    /** @copydoc NetworkWorldBindingController::Acquire */
    Result<ReplicationWorldReadLease> NetworkWorldBindingController::Acquire(const NetworkModeWorldKind kind,
                                                                             const ReplicationWorldWorkRequest &request,
                                                                             const ReplicationWorldCapability capability) const {
        return impl_->worlds.Acquire(kind, request, capability);
    }

    void NetworkWorldBindingController::Impl::ClearPending() noexcept {
        for (auto &peer : peers) {
            peer.pending.reset();
            peer.frameBytes = 0;
        }
    }

    /** @copydoc NetworkWorldBindingController::Shutdown */
    void NetworkWorldBindingController::Shutdown() noexcept {
        if (impl_->stopped)
            return;
        impl_->stopped = true;
        if (impl_->transport)
            Detach(*impl_->transport);
        impl_->worlds.Shutdown();
        for (auto &peer : impl_->peers)
            peer.lifecycle.reset();
    }
}  // namespace Horo::Network
