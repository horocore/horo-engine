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
        ModeParticipant(Network::NetworkModeComposition mode, std::shared_ptr<ClientSessionOwner> sessions,
                        std::unique_ptr<Runtime::RuntimeLifecycleParticipant> fixed) noexcept
            : mode_(std::move(mode)), sessions_(std::move(sessions)), fixed_(std::move(fixed)) {
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
            if (mode_.TravelInProgress() || pending_ || travelFailure_ || worlds.empty() || worlds.size() > nextWorlds_.size())
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
            if (fixed_) {
                auto started = fixed_->Startup(cancellation);
                if (started.HasError())
                    return started;
            }
            return mode_.Start();
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) override {
            if (auto prepared = PreparePhase(phase, frame); prepared.HasError())
                return FailPhase(prepared.ErrorValue());
            if (auto advanced = mode_.RunPhase(phase); advanced.HasError())
                return FailPhase(advanced.ErrorValue());
            if (auto failed = mode_.TakeTravelFailure(); failed && !travelFailure_)
                travelFailure_ = std::move(failed);
            return phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges && pending_ ? AdvanceQueuedTravel()
                                                                                              : Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            if (fixed_) {
                auto begun = fixed_->OnFixedUpdate(context);
                if (begun.HasError())
                    return begun;
            }
            return mode_.RunFixedTick(context);
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
            return fixed_ ? fixed_->PrepareFixedCommit(context) : Result<void>::Success();
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (fixed_)
                fixed_->CommitFixedUpdate(context);
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (fixed_)
                fixed_->AbortFixedUpdate(context);
        }

        void Shutdown() noexcept override {
            pending_ = false;
            // Failure teardown must preserve the original queued-travel diagnostic for the caller.
            sessions_->Shutdown();
            mode_.Shutdown();
            if (fixed_)
                fixed_->Shutdown();
        }

    private:
        Result<void> PreparePhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) {
            if (fixed_) {
                auto forwarded = fixed_->OnPhase(phase, frame);
                if (forwarded.HasError())
                    return forwarded;
            }
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            return phase == Runtime::RuntimePhase::NetworkPoll ? sessions_->BeginPoll() : Result<void>::Success();
        }

        Result<void> AdvanceQueuedTravel() {
            pending_ = false;
            if (auto reconciled = sessions_->Reconcile(); reconciled.HasError())
                return reconciled;
            if (auto traveled = mode_.Travel({nextWorlds_.data(), nextWorldCount_}, Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                             nextSession_);
                traveled.HasError())
                travelFailure_ = traveled.ErrorValue();
            return Result<void>::Success();
        }

        Result<void> FailPhase(const Error &error) {
            if (pending_) {
                pending_ = false;
                travelFailure_ = error;
            }
            return Result<void>::Failure(error);
        }

        Network::NetworkModeComposition mode_;
        std::shared_ptr<ClientSessionOwner> sessions_;
        std::unique_ptr<Runtime::RuntimeLifecycleParticipant> fixed_;
        std::array<Network::NetworkModeWorld, 2> nextWorlds_{};
        std::size_t nextWorldCount_{};
        std::optional<Network::NetworkOperationGeneration> nextSession_;
        std::optional<Error> travelFailure_;
        bool pending_{};
    };

    namespace {
        Result<Network::NetworkModePlan> PrepareProductPlan(const NetworkProductHostInputs &inputs,
                                                            Network::NetworkTargetDiagnostic *diagnostic) {
            if (inputs.selection.role != Network::NetworkProjectRole::Standalone) {
                if (const auto complete = Network::RequireCompleteNetworkReplicationInventory(inputs.project); complete.HasError())
                    return Result<Network::NetworkModePlan>::Failure(complete.ErrorValue());
            }
            const auto assessment = Network::AssessNetworkTarget(inputs.project, inputs.product, inputs.inventory, inputs.hostFacts,
                                                                 inputs.requirements, inputs.selection);
            if (diagnostic != nullptr)
                *diagnostic = assessment.diagnostic;
            if (!assessment.Admitted())
                return Result<Network::NetworkModePlan>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable,
                                                                           "Product, package, host or project does not admit this mode."));
            return Network::ResolveNetworkModePlan(assessment, inputs.selection, inputs.hostGeneration, inputs.worlds, inputs.presentation);
        }

        Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>> PrepareFixedParticipant(const NetworkProductHostInputs &inputs,
                                                                                              Runtime::RuntimeDispatchSource source) {
            using Return = Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>;
            if (!inputs.fixedParticipant)
                return Return::Success(nullptr);
            auto created = inputs.fixedParticipant(std::move(source));
            if (created.HasError())
                return created;
            if (!created.Value())
                return Return::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            return created;
        }

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
                                           std::shared_ptr<NetworkDebuggerService> debugger,
                                           NetworkNativeRetirementPoll retirement) noexcept
        : pollRetirement_(std::move(retirement)), debugger_(std::move(debugger)), runtime_(std::move(runtime)), mode_(mode) {}

    NetworkProductHost::~NetworkProductHost() noexcept {
        Shutdown();
        auto retired = PollShutdown(64);
        if (retired.HasError() || !retired.Value())
            std::terminate();
    }

    Result<std::unique_ptr<NetworkProductHost>> NetworkProductHost::Create(Clock &clock, const NetworkProductHostInputs &inputs,
                                                                           Network::NetworkModeFactories factories,
                                                                           Network::NetworkTargetDiagnostic *diagnostic,
                                                                           std::shared_ptr<NetworkDebuggerService> debugger) {
        auto plan = PrepareProductPlan(inputs, diagnostic);
        if (plan.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(plan.ErrorValue());
        auto sessions = PrepareSessionOwner(inputs, factories);
        if (sessions.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(sessions.ErrorValue());
        auto composition =
            Network::NetworkModeComposition::Create(std::move(plan).Value(), std::move(factories), inputs.purpose, inputs.worldBinding);
        if (composition.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(composition.ErrorValue());
        auto runtime = Runtime::RuntimeHost::Create(clock);
        if (runtime.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(runtime.ErrorValue());
        auto fixed = PrepareFixedParticipant(inputs, runtime.Value()->DispatchSource());
        if (fixed.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(std::move(fixed).ErrorValue());
        auto participant =
            std::make_unique<ModeParticipant>(std::move(composition).Value(), std::move(sessions).Value(), std::move(fixed).Value());
        auto *mode = participant.get();
        if (auto added = runtime.Value()->AddParticipant(std::move(participant)); added.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(added.ErrorValue());
        return Result<std::unique_ptr<NetworkProductHost>>::Success(
            std::make_unique<NetworkProductHost>(ConstructionKey{}, std::move(runtime).Value(), mode, std::move(debugger),
                                                 inputs.pollRetirement));
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

    /** @copydoc NetworkProductHost::ClientConnectionTerminal */
    const ClientConnectionAttemptTerminal *NetworkProductHost::ClientConnectionTerminal() const noexcept {
        return mode_->Sessions().ConnectionAttemptTerminal();
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

    /** @copydoc NetworkProductHost::AcquireReplication */
    Result<Network::ReplicationWorldReadLease> NetworkProductHost::AcquireReplication(
        const Network::NetworkModeWorldKind world, const Network::ReplicationWorldWorkRequest &request,
        const Network::ReplicationWorldCapability capability) const {
        return mode_->Mode().AcquireReplication(world, request, capability);
    }

    void NetworkProductHost::RequestShutdown() const noexcept {
        if (runtime_)
            runtime_->RequestShutdown();
    }

    /** @copydoc NetworkProductHost::PollShutdown */
    Result<bool> NetworkProductHost::PollShutdown(std::uint32_t maximumNativeRecords) {
        Shutdown();
        if (runtime_ && runtime_->State() != Runtime::RuntimeLifecycleState::Stopped)
            return Result<bool>::Success(false);
        if (!pollRetirement_)
            return Result<bool>::Success(true);
        return pollRetirement_(maximumNativeRecords);
    }

    void NetworkProductHost::Shutdown() noexcept {
        if (debugger_)
            debugger_->Producer().Detach();
        if (runtime_ != nullptr)
            runtime_->Shutdown();
    }
}  // namespace Horo::Application::Internal
