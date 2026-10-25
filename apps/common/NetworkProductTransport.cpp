#include "NetworkProductTransport.h"

#include "Horo/Network/NetworkMetricTransport.h"

#include <algorithm>
#include <new>

namespace Horo::Application::Internal {
    /** @copydoc NetworkProductAdmission::NetworkProductAdmission */
    NetworkProductAdmission::NetworkProductAdmission(ConstructionKey, const Network::NetworkAdmissionPolicy &policy,
                                                     Network::NetworkAdmissionProviders providers,
                                                     std::shared_ptr<const Network::MessageCodecRegistry> codecs,
                                                     const std::span<const NetworkProductInboundHandler> handlers)
        : channels_(policy.gameplayChannels.begin(), policy.gameplayChannels.end()), handlers_(handlers.begin(), handlers.end()),
          policy_(policy), providers_(std::move(providers)), codecs_(std::move(codecs)) {
        const auto &features = policy.local.compatibility.features;
        std::ranges::copy(features.supported, supported_.begin());
        std::ranges::copy(features.required, required_.begin());
        policy_.local.compatibility.features = {std::span{supported_}.first(features.supported.size()),
                                                std::span{required_}.first(features.required.size())};
        policy_.gameplayChannels = channels_;
    }

    /** @copydoc NetworkProductAdmission::Capture */
    Result<std::shared_ptr<const NetworkProductAdmission>> NetworkProductAdmission::Capture(
        const Network::NetworkAdmissionPolicy &policy, Network::NetworkAdmissionProviders providers,
        std::shared_ptr<const Network::MessageCodecRegistry> codecs, const std::span<const NetworkProductInboundHandler> handlers) {
        if (!codecs || policy.local.compatibility.features.supported.size() > Network::MaximumHandshakeFeatures ||
            policy.local.compatibility.features.required.size() > Network::MaximumHandshakeFeatures || policy.gameplayChannels.empty() ||
            policy.gameplayChannels.size() > 64 || handlers.size() > 128)
            return Result<std::shared_ptr<const NetworkProductAdmission>>::Failure(
                MakeError(Network::NetworkErrors::AdmissionPolicyInvalid));
        try {
            return Result<std::shared_ptr<const NetworkProductAdmission>>::Success(
                std::make_shared<NetworkProductAdmission>(ConstructionKey{}, policy, std::move(providers), std::move(codecs), handlers));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<const NetworkProductAdmission>>::Failure(
                MakeError(Network::NetworkErrors::AdmissionLimitExceeded));
        }
    }

    namespace {
        namespace Net = Network;

        /** @brief One actual product transport service; backend selection remains in its application factory. */
        class ProductTransportService final : public Net::INetworkModeService {
        public:
            ProductTransportService(std::unique_ptr<Net::INetworkTransport> transport, const Net::NetworkProjectRole role,
                                    const Net::NetworkAddress &bind, const Net::NetworkAddress &connect,
                                    std::shared_ptr<NetworkDebuggerService> debugger, std::shared_ptr<ClientSessionOwner> sessions,
                                    std::shared_ptr<const NetworkProductAdmission> admission)
                : debugger_(std::move(debugger)), sessions_(std::move(sessions)), capture_(std::move(admission)),
                  metrics_(debugger_->Producer().Source().session, true, &debugger_->Producer()), role_(role), bind_(bind),
                  connect_(connect),
                  transport_(std::make_unique<Net::NetworkMetricTransport>(std::move(transport), metrics_, &debugger_->Producer())) {}

            ~ProductTransportService() override {
                Shutdown();
            }

            /** @brief Allocate bounded routing/control owners without invoking native initialization or authorities. */
            Result<void> Configure() {
                if (auto selected = ConfigureCodecs(); selected.HasError())
                    return selected;
                auto created = Net::InboundMessageDispatcher::Create(*transport_, *codecs_);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                router_ = std::move(created).Value();
                if (auto admission = ConfigureAdmission(); admission.HasError())
                    return admission;
                router_->SetSessionHost(sessions_);
                return Result<void>::Success();
            }

            Result<void> Prepare() override {
                if (stopped_ || prepared_)
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkLifecycleTransitionInvalid));
                if (auto initialized = transport_->Initialize({maximumPeers_, 64, maximumMessageBytes_}); initialized.HasError())
                    return initialized;
                if (auto attached = sessions_->AttachTransport(*transport_); attached.HasError())
                    return attached;
                prepared_ = true;
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                if (!prepared_ || stopped_)
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkLifecycleTransitionInvalid));
                using enum Net::NetworkProjectRole;
                if (role_ == ListenServer || role_ == DedicatedServer) {
                    auto listening = transport_->Listen({.bindAddress = bind_, .maximumConnections = maximumPeers_});
                    if (listening.HasError())
                        return Result<void>::Failure(listening.ErrorValue());
                    listener_ = listening.Value();
                }
                if (role_ == Client || role_ == ListenServer) {
                    auto connecting = transport_->Connect({.endpoint = connect_});
                    if (connecting.HasError())
                        return Result<void>::Failure(connecting.ErrorValue());
                    // No consumer is called by Connect. Retain its exact handle before the first owner poll.
                    if (auto pending = sessions_->BeginConnectionAttempt(connecting.Value()); pending.HasError())
                        return pending;  // Normal startup rollback still owns and closes the accepted native attempt.
                }
                return Result<void>::Success();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                if (phase != Runtime::RuntimePhase::NetworkPoll)
                    return Result<void>::Success();
                auto tick = sessions_->PollTick();
                if (tick.HasError())
                    return Result<void>::Failure(tick.ErrorValue());
                lastTick_ = tick.Value();
                if (admission_) {
                    if (auto begun = admission_->BeginPoll(lastTick_); begun.HasError())
                        return begun;
                }
                if (auto polled = router_->RunNetworkPoll(lastTick_); polled.HasError()) {
                    if (admission_)
                        admission_->FailTransport(Net::NetworkFailureKind::TransportUnavailable, lastTick_);
                    return Result<void>::Failure(polled.ErrorValue());
                }
                (void)metrics_.Publish();
                const auto snapshot = metrics_.Snapshot();
                (void)debugger_->Publish(debugger_->Producer().Source(), &snapshot);
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                if (stopped_)
                    return;
                stopped_ = true;
                if (admission_)
                    admission_->Shutdown(lastTick_);
                if (router_)
                    router_->Shutdown();
                admission_.reset();
                router_.reset();
                if (transport_) {
                    sessions_->DetachTransport(*transport_);
                    if (listener_.IsValid())
                        (void)transport_->CloseListener(listener_);
                    transport_->Shutdown();
                    transport_.reset();
                }
                codecs_.reset();
                capture_.reset();  // Providers retire only after the exact native borrow is detached and destroyed.
            }

        private:
            /** @brief Select immutable codec policy before native initialization. */
            Result<void> ConfigureCodecs() {
                if (capture_) {
                    const bool outgoing = role_ == Net::NetworkProjectRole::Client || role_ == Net::NetworkProjectRole::ListenServer;
                    if (const bool incoming =
                            role_ == Net::NetworkProjectRole::DedicatedServer || role_ == Net::NetworkProjectRole::ListenServer;
                        capture_->Policy().endpoints[static_cast<std::size_t>(Net::AdmissionEndpoint::Client)] != outgoing ||
                        capture_->Policy().endpoints[static_cast<std::size_t>(Net::AdmissionEndpoint::Server)] != incoming)
                        return Result<void>::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid));
                    codecs_ = capture_->Codecs();
                    maximumPeers_ = static_cast<std::uint32_t>(capture_->Policy().maximumPeers);
                    maximumMessageBytes_ = static_cast<std::uint32_t>(capture_->Policy().envelope.maximumFrameBytes);
                } else {
                    auto identities = Net::ProtocolIdentityRegistry::Create({});
                    if (identities.HasError())
                        return Result<void>::Failure(identities.ErrorValue());
                    auto codecs = Net::MessageCodecRegistry::Create({}, identities.Value());
                    if (codecs.HasError())
                        return Result<void>::Failure(codecs.ErrorValue());
                    codecs_ = std::make_shared<const Net::MessageCodecRegistry>(std::move(codecs).Value());
                }
                return Result<void>::Success();
            }

            /** @brief Reserve exact control IDs and install bounded existing handler bindings. */
            Result<void> ConfigureAdmission() {
                if (!capture_)
                    return Result<void>::Success();
                auto controller = Net::NetworkAdmissionController::Create(*transport_, *router_, codecs_, capture_->Policy(),
                                                                          capture_->Providers(), sessions_);
                if (controller.HasError())
                    return Result<void>::Failure(controller.ErrorValue());
                admission_ = std::move(controller).Value();
                router_->SetAdmissionController(admission_);
                for (const auto &binding : capture_->Handlers()) {
                    if (std::ranges::any_of(capture_->Policy().controls, [&binding](const auto &control) {
                        return binding.binding.protocol == control.protocol && binding.binding.message == control.id;
                    }))
                        return Result<void>::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid));
                    auto owner = binding.owner.lock();
                    if (!owner)
                        return Result<void>::Failure(MakeError(Net::NetworkErrors::GameplayDispatchRejected));
                    if (auto installed = router_->RegisterHandler(binding.binding, owner); installed.HasError())
                        return installed;
                }
                return Result<void>::Success();
            }

            std::shared_ptr<NetworkDebuggerService> debugger_;
            std::shared_ptr<ClientSessionOwner> sessions_;
            std::shared_ptr<const NetworkProductAdmission> capture_;
            Net::NetworkMetrics metrics_;
            Net::NetworkProjectRole role_;
            Net::NetworkAddress bind_;
            Net::NetworkAddress connect_;
            std::unique_ptr<Net::INetworkTransport> transport_;
            std::shared_ptr<const Net::MessageCodecRegistry> codecs_;
            std::unique_ptr<Net::InboundMessageDispatcher> router_;
            std::shared_ptr<Net::NetworkAdmissionController> admission_;
            Net::ListenerHandle listener_{};
            std::uint64_t lastTick_{};
            std::uint32_t maximumPeers_{8};
            std::uint32_t maximumMessageBytes_{1200};
            bool prepared_{};
            bool stopped_{};
        };
    }  // namespace

    /** @copydoc CreateNetworkProductTransport */
    Result<std::unique_ptr<Network::INetworkModeService>> CreateNetworkProductTransport(
        std::unique_ptr<Network::INetworkTransport> transport, const Network::NetworkProjectRole role, const Network::NetworkAddress &bind,
        const Network::NetworkAddress &connect, std::shared_ptr<NetworkDebuggerService> debugger,
        std::shared_ptr<ClientSessionOwner> sessions, std::shared_ptr<const NetworkProductAdmission> admission) {
        if (!transport || !debugger || !sessions || role >= Network::NetworkProjectRole::Count ||
            role == Network::NetworkProjectRole::Standalone)
            return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
        try {
            auto service = std::make_unique<ProductTransportService>(std::move(transport), role, bind, connect, std::move(debugger),
                                                                     std::move(sessions), std::move(admission));
            if (auto configured = service->Configure(); configured.HasError())
                return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(configured.ErrorValue());
            return Result<std::unique_ptr<Network::INetworkModeService>>::Success(std::move(service));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(
                MakeError(Network::NetworkErrors::AdmissionLimitExceeded));
        }
    }
}  // namespace Horo::Application::Internal
