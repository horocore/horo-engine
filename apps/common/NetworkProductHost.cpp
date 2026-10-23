#include "NetworkProductHost.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Application::Internal {
    /** @brief Reconciles transport-produced terminals at the existing Session boundary before world services. */
    class SessionBoundaryService final : public Network::INetworkModeService {
    public:
        SessionBoundaryService(std::unique_ptr<Network::INetworkModeService> service, std::shared_ptr<ClientSessionOwner> sessions)
            : service_(std::move(service)), sessions_(std::move(sessions)) {}

        Result<void> Prepare() override {
            return service_->Prepare();
        }

        Result<void> Activate() override {
            return service_->Activate();
        }

        Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
            if (phase == Runtime::RuntimePhase::NetworkPoll) {
                if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                    return reconciled;
            }
            auto advanced = service_->RunPhase(phase);
            if (phase == Runtime::RuntimePhase::NetworkPoll) {
                if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                    return reconciled;
            }
            return advanced;
        }

        Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
            return service_->RunFixedTick(context);
        }

        void Shutdown() noexcept override {
            service_->Shutdown();
        }

    private:
        std::unique_ptr<Network::INetworkModeService> service_;
        std::shared_ptr<ClientSessionOwner> sessions_;
    };

    class ModeParticipant final : public Runtime::RuntimeLifecycleParticipant {
    public:
        ModeParticipant(Network::NetworkModeComposition mode, std::shared_ptr<ClientSessionOwner> sessions) noexcept
            : mode_(std::move(mode)), sessions_(std::move(sessions)) {
            sessions_->Bind(mode_);
        }

        ~ModeParticipant() override {
            Shutdown();
        }

        ClientSessionOwner &Sessions() noexcept {
            return *sessions_;
        }

        Network::NetworkModeComposition &Mode() noexcept {
            return mode_;
        }

        Result<void> RequestTravel(const std::span<const Network::NetworkModeWorld> worlds,
                                   const std::optional<Network::NetworkOperationGeneration> session) {
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            if (!mode_.Active())
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeUnavailable, "A stopped product cannot request travel."));
            if (pending_ || travelFailure_ || worlds.empty() || worlds.size() > nextWorlds_.size())
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Invalid or overlapping travel request."));
            std::ranges::copy(worlds, nextWorlds_.begin());
            nextWorldCount_ = worlds.size();
            nextSession_ = session;
            pending_ = true;
            return Result<void>::Success();
        }

        std::optional<Error> TakeTravelFailure() noexcept {
            return std::exchange(travelFailure_, std::nullopt);
        }

        Result<void> Startup(const CancellationToken &cancellation) override {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeShuttingDown));
            return mode_.Start();
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &) override {
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return FailPhase(reconciled.ErrorValue());
            if (phase == Runtime::RuntimePhase::NetworkPoll) {
                if (auto polled = sessions_->BeginPoll(); polled.HasError())
                    return FailPhase(polled.ErrorValue());
            }
            if (auto advanced = mode_.RunPhase(phase); advanced.HasError())
                return FailPhase(advanced.ErrorValue());
            if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || !pending_)
                return Result<void>::Success();
            pending_ = false;
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            if (auto traveled = mode_.Travel({nextWorlds_.data(), nextWorldCount_}, phase, nextSession_); traveled.HasError())
                travelFailure_ = traveled.ErrorValue();
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            return mode_.RunFixedTick(context);
        }

        void Shutdown() noexcept override {
            pending_ = false;
            // Failure teardown must preserve the original queued-travel diagnostic for the caller.
            sessions_->Shutdown();
            mode_.Shutdown();
        }

    private:
        Result<void> FailPhase(const Error &error) {
            if (pending_) {
                pending_ = false;
                travelFailure_ = error;
            }
            return Result<void>::Failure(error);
        }

        Network::NetworkModeComposition mode_;
        std::shared_ptr<ClientSessionOwner> sessions_;
        std::array<Network::NetworkModeWorld, 2> nextWorlds_{};
        std::size_t nextWorldCount_{};
        std::optional<Network::NetworkOperationGeneration> nextSession_;
        std::optional<Error> travelFailure_;
        bool pending_{};
    };

    namespace {
        /** @brief Install one session boundary only after host target and mode admission have succeeded. */
        Result<std::shared_ptr<ClientSessionOwner>> PrepareSessionOwner(const NetworkProductHostInputs &inputs,
                                                                        Network::NetworkModeFactories &factories) {
            auto sessions = inputs.selection.role == Network::NetworkProjectRole::Standalone ? std::make_shared<ClientSessionOwner>(nullptr)
                                                                                             : inputs.sessions;
            if (inputs.selection.role != Network::NetworkProjectRole::Standalone) {
                if (!sessions)
                    return Result<std::shared_ptr<ClientSessionOwner>>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleInvalid));
                if (auto valid = sessions->Validate(); valid.HasError())
                    return Result<std::shared_ptr<ClientSessionOwner>>::Failure(valid.ErrorValue());
            }
            if (auto &sessionFactory = factories.services[static_cast<std::size_t>(Network::NetworkModeServiceKind::Session)];
                sessionFactory) {
                sessionFactory = [original = std::move(sessionFactory), sessions](const Network::NetworkModeServiceRequest &request) {
                    auto service = original(request);
                    if (service.HasError())
                        return service;
                    if (!service.Value())
                        return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeUnavailable));
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Success(
                        std::make_unique<SessionBoundaryService>(std::move(service).Value(), sessions));
                };
            }
            return Result<std::shared_ptr<ClientSessionOwner>>::Success(std::move(sessions));
        }
    }  // namespace

    NetworkProductHost::NetworkProductHost(ConstructionKey, std::unique_ptr<Runtime::RuntimeHost> runtime, ModeParticipant *mode,
                                           std::shared_ptr<NetworkDebuggerService> debugger) noexcept
        : debugger_(std::move(debugger)), runtime_(std::move(runtime)), mode_(mode) {}

    NetworkProductHost::~NetworkProductHost() noexcept {
        Shutdown();
    }

    Result<std::unique_ptr<NetworkProductHost>> NetworkProductHost::Create(Clock &clock, const NetworkProductHostInputs &inputs,
                                                                           Network::NetworkModeFactories factories,
                                                                           Network::NetworkTargetDiagnostic *diagnostic,
                                                                           std::shared_ptr<NetworkDebuggerService> debugger) {
        if (inputs.selection.role != Network::NetworkProjectRole::Standalone) {
            if (const auto complete = Network::RequireCompleteNetworkReplicationInventory(inputs.project); complete.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(complete.ErrorValue());
        }
        const auto assessment = Network::AssessNetworkTarget(inputs.project, inputs.product, inputs.inventory, inputs.hostFacts,
                                                             inputs.requirements, inputs.selection);
        if (diagnostic != nullptr)
            *diagnostic = assessment.diagnostic;
        if (!assessment.Admitted())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(
                MakeError(Network::NetworkErrors::NetworkModeUnavailable, "Product, package, host or project does not admit this mode."));
        auto plan =
            Network::ResolveNetworkModePlan(assessment, inputs.selection, inputs.hostGeneration, inputs.worlds, inputs.presentation);
        if (plan.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(plan.ErrorValue());
        auto sessions = PrepareSessionOwner(inputs, factories);
        if (sessions.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(sessions.ErrorValue());
        auto composition = Network::NetworkModeComposition::Create(std::move(plan).Value(), std::move(factories));
        if (composition.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(composition.ErrorValue());
        auto runtime = Runtime::RuntimeHost::Create(clock);
        if (runtime.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(runtime.ErrorValue());
        auto participant = std::make_unique<ModeParticipant>(std::move(composition).Value(), std::move(sessions).Value());
        auto *mode = participant.get();
        if (auto added = runtime.Value()->AddParticipant(std::move(participant)); added.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(added.ErrorValue());
        return Result<std::unique_ptr<NetworkProductHost>>::Success(
            std::make_unique<NetworkProductHost>(ConstructionKey{}, std::move(runtime).Value(), mode, std::move(debugger)));
    }

    Result<void> NetworkProductHost::Startup() {
        auto started = runtime_->Startup();
        if (debugger_ && started.HasValue())
            (void)debugger_->Publish(debugger_->Producer().Source());
        return started;
    }

    Result<void> NetworkProductHost::RunFrame() {
        auto advanced = runtime_->RunFrame();
        if (debugger_ && advanced.HasValue())
            (void)debugger_->Publish(debugger_->Producer().Source());
        return advanced;
    }

    Result<void> NetworkProductHost::RequestTravel(const std::span<const Network::NetworkModeWorld> worlds,
                                                   const std::optional<Network::NetworkOperationGeneration> session) {
        auto requested = mode_->RequestTravel(worlds, session);
        // Travel revokes the old scene publication before any replacement scene is exposed.
        if (debugger_ && requested.HasValue())
            debugger_->Producer().Detach();
        return requested;
    }

    /** @copydoc NetworkProductHost::TakeTravelFailure */
    std::optional<Error> NetworkProductHost::TakeTravelFailure() {
        return mode_->TakeTravelFailure();
    }

    /** @copydoc NetworkProductHost::AdmitClientSession */
    Result<void> NetworkProductHost::AdmitClientSession(const Network::NetworkModeWorldKind world,
                                                        std::shared_ptr<Network::PeerSessionLifecycle> session,
                                                        const Network::ConnectionHandle connection,
                                                        const Network::NetworkOperationGeneration generation) {
        return mode_->Sessions().Admit(world, std::move(session), connection, generation);
    }

    Result<void> NetworkProductHost::DisconnectClient(const Network::NetworkModeWorldKind world,
                                                      const Network::NetworkOperationGeneration generation) {
        return mode_->Sessions().Disconnect(world, generation);
    }

    /** @copydoc NetworkProductHost::ClientSessionTerminal */
    const Network::PeerSessionTerminalSnapshot *NetworkProductHost::ClientSessionTerminal() const noexcept {
        return mode_->Sessions().Terminal();
    }

    Result<Network::NetworkModeRoleView> NetworkProductHost::Role(const Network::NetworkModeWorldKind world,
                                                                  const Runtime::SceneRuntimeId scene) const {
        return mode_->Mode().Role(world, scene);
    }

    void NetworkProductHost::Shutdown() noexcept {
        if (debugger_)
            debugger_->Producer().Detach();
        if (runtime_ != nullptr)
            runtime_->Shutdown();
    }
}  // namespace Horo::Application::Internal
