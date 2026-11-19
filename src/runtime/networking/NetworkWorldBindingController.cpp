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
        if (!ValidPrefabLifecycleRoute(setup.policy, *setup.codecs) || !ValidAuthoredSceneRoute(setup.policy, *setup.codecs) ||
            !ValidInitialStateRoute(setup.policy, *setup.codecs) || setup.policy.initialState.has_value() != bool(setup.initialStateHost))
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
    Result<void> NetworkWorldBindingController::Prepare(const NetworkAuthoredSceneProjection &authored,
                                                        NetworkAuthoredSceneProjection client) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        return impl_->worlds.Prepare(authored, std::move(client));
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
        if (auto registered = impl_->RegisterRoutes(dispatcher, owner); registered.HasError())
            return registered;
        dispatcher.worldBinding_ = owner;
        dispatcher.requiresWorldBinding_ = true;
        impl_->transport = &transport;
        impl_->dispatcher = &dispatcher;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::RegisterRoutes */
    Result<void> NetworkWorldBindingController::Impl::RegisterRoutes(InboundMessageDispatcher &router,
                                                                     const std::shared_ptr<NetworkWorldBindingController> &owner) const {
        struct Route final {
            const MessageIdentityDescriptor *identity;
            std::uint32_t rate;
        };

        const auto &policy = setup.policy;
        const std::array routes{Route{&policy.control, 4}, Route{policy.prefabLifecycle ? &policy.prefabLifecycle->control : nullptr, 16},
                                Route{policy.authoredScene ? &policy.authoredScene->control : nullptr, 16},
                                Route{policy.initialState ? &policy.initialState->control : nullptr, 64}};
        for (std::size_t index{}; index < routes.size(); ++index) {
            const auto [identity, rate] = routes[index];
            if (!identity)
                continue;
            auto registered = router.RegisterHandler({identity->protocol, identity->id, MessageTrafficClass::Control,
                                                      Runtime::RuntimePhase::NetworkPoll, rate},
                                                     owner);
            if (registered.HasValue())
                continue;
            for (const auto &previous : std::span{routes}.first(index)) {
                if (previous.identity)
                    router.RevokeHandler(previous.identity->protocol, previous.identity->id);
            }
            return registered;
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Detach */
    void NetworkWorldBindingController::Detach(const INetworkTransport &transport) noexcept {
        if (impl_->transport != &transport)
            return;
        if (impl_->dispatcher) {
            impl_->dispatcher->RevokeHandler(impl_->setup.policy.control.protocol, impl_->setup.policy.control.id);
            if (const auto &route = impl_->setup.policy.prefabLifecycle)
                impl_->dispatcher->RevokeHandler(route->control.protocol, route->control.id);
            if (const auto &route = impl_->setup.policy.authoredScene)
                impl_->dispatcher->RevokeHandler(route->control.protocol, route->control.id);
            if (const auto &route = impl_->setup.policy.initialState)
                impl_->dispatcher->RevokeHandler(route->control.protocol, route->control.id);
        }
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
        if (!impl_->transport || !message.fields.empty())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        const auto matches = [&message](const MessageIdentityDescriptor &identity) {
            return message.protocol == identity.protocol && message.message == identity.id && message.schema == identity.schema &&
                   message.schemaVersion == identity.version;
        };
        const auto &policy = impl_->setup.policy;
        if (policy.prefabLifecycle && matches(policy.prefabLifecycle->control))
            return impl_->worlds.AcceptPrefab(message.payload, context);
        if (policy.authoredScene && matches(policy.authoredScene->control))
            return impl_->worlds.AcceptAuthored(message.payload, context);
        if (policy.initialState && matches(policy.initialState->control))
            return impl_->AcceptInitial(message, context);
        if (!matches(policy.control))
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        const auto decoded = DecodeNetworkWorldBinding(message.payload);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        return impl_->worlds.Accept(decoded.Value(), context);
    }

    /** @copydoc NetworkWorldBindingController::HasPrefabLifecycle */
    bool NetworkWorldBindingController::HasPrefabLifecycle() const noexcept {
        return impl_->setup.policy.prefabLifecycle.has_value();
    }

    /** @copydoc NetworkWorldBindingController::HasAuthoredScene */
    bool NetworkWorldBindingController::HasAuthoredScene() const noexcept {
        return impl_->setup.policy.authoredScene.has_value();
    }

    /** @copydoc NetworkWorldBindingController::TakePrefab */
    Result<std::vector<NetworkPrefabLifecycleRecord>> NetworkWorldBindingController::TakePrefab(Runtime::SceneRuntimeId scene,
                                                                                                Runtime::RuntimePhase phase) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return Result<std::vector<NetworkPrefabLifecycleRecord>>::Failure(checked.ErrorValue());
        if (!impl_->transport)
            return Result<std::vector<NetworkPrefabLifecycleRecord>>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        const Impl::BusyScope busy{impl_->busy};
        return impl_->worlds.TakePrefab(scene, phase);
    }

    /** @copydoc NetworkWorldBindingController::PrepareClientPrefab */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> NetworkWorldBindingController::PrepareClientPrefab(
        Runtime::SceneRuntimeId scene, const NetworkPrefabLifecycleRecord &spawn, std::span<const Runtime::EntityRef> members,
        bool creating, const CancellationToken &cancellation) const {
        using Outcome = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return Outcome::Failure(checked.ErrorValue());
        if (!impl_->transport || !impl_->setup.policy.prefabLifecycle)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        return impl_->worlds.PrepareClientPrefab(scene, spawn, members, creating, cancellation);
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

    /** @copydoc NetworkWorldBindingController::PrepareAuthorityObjects */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> NetworkWorldBindingController::PrepareAuthorityObjects(
        Runtime::SceneRuntimeId scene, std::span<const ReplicationDynamicSceneObject> added, std::span<const Runtime::EntityRef> retired,
        const CancellationToken &cancellation) const {
        return impl_->worlds.PrepareAuthorityObjects(scene, added, retired, cancellation);
    }

    void NetworkWorldBindingController::Impl::ClearPending() noexcept {
        for (auto &peer : peers) {
            peer.pending.reset();
            peer.prefabPending.reset();
            peer.authoredPending.reset();
            peer.initial = {};
            peer.initialFramePending = false;
            peer.knownPrefabs.clear();
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
