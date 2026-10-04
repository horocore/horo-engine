#include "NetworkProductHost.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Application::Internal {
    class ModeParticipant final : public Runtime::RuntimeLifecycleParticipant {
    public:
        explicit ModeParticipant(Network::NetworkModeComposition mode) noexcept : mode_(std::move(mode)) {}

        Network::NetworkModeComposition &Mode() noexcept {
            return mode_;
        }

        Result<void> RequestTravel(const std::span<const Network::NetworkModeWorld> worlds,
                                   const std::optional<Network::NetworkOperationGeneration> session) {
            if (!mode_.Active())
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeUnavailable, "A stopped product cannot request travel."));
            if (pending_ || worlds.empty() || worlds.size() > nextWorlds_.size())
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Invalid or overlapping travel request."));
            std::ranges::copy(worlds, nextWorlds_.begin());
            nextWorldCount_ = worlds.size();
            nextSession_ = session;
            travelFailure_.reset();
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
            if (auto advanced = mode_.RunPhase(phase);
                advanced.HasError() || phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || !pending_)
                return advanced;
            pending_ = false;
            if (auto traveled = mode_.Travel({nextWorlds_.data(), nextWorldCount_}, phase, nextSession_); traveled.HasError())
                travelFailure_ = traveled.ErrorValue();
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            return mode_.RunFixedTick(context);
        }

        void Shutdown() noexcept override {
            pending_ = false;
            travelFailure_.reset();
            mode_.Shutdown();
        }

    private:
        Network::NetworkModeComposition mode_;
        std::array<Network::NetworkModeWorld, 2> nextWorlds_{};
        std::size_t nextWorldCount_{};
        std::optional<Network::NetworkOperationGeneration> nextSession_;
        std::optional<Error> travelFailure_;
        bool pending_{};
    };

    NetworkProductHost::NetworkProductHost(ConstructionKey, std::unique_ptr<Runtime::RuntimeHost> runtime, ModeParticipant *mode) noexcept
        : runtime_(std::move(runtime)), mode_(mode) {}

    NetworkProductHost::~NetworkProductHost() noexcept {
        Shutdown();
    }

    Result<std::unique_ptr<NetworkProductHost>> NetworkProductHost::Create(Clock &clock, const NetworkProductHostInputs &inputs,
                                                                           Network::NetworkModeFactories factories,
                                                                           Network::NetworkTargetDiagnostic *diagnostic) {
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
        auto composition = Network::NetworkModeComposition::Create(std::move(plan).Value(), std::move(factories));
        if (composition.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(composition.ErrorValue());
        auto runtime = Runtime::RuntimeHost::Create(clock);
        if (runtime.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(runtime.ErrorValue());
        auto participant = std::make_unique<ModeParticipant>(std::move(composition).Value());
        auto *mode = participant.get();
        if (auto added = runtime.Value()->AddParticipant(std::move(participant)); added.HasError())
            return Result<std::unique_ptr<NetworkProductHost>>::Failure(added.ErrorValue());
        return Result<std::unique_ptr<NetworkProductHost>>::Success(
            std::make_unique<NetworkProductHost>(ConstructionKey{}, std::move(runtime).Value(), mode));
    }

    Result<void> NetworkProductHost::Startup() {
        return runtime_->Startup();
    }

    Result<void> NetworkProductHost::RunFrame() {
        return runtime_->RunFrame();
    }

    Result<void> NetworkProductHost::RequestTravel(const std::span<const Network::NetworkModeWorld> worlds,
                                                   const std::optional<Network::NetworkOperationGeneration> session) {
        return mode_->RequestTravel(worlds, session);
    }

    std::optional<Error> NetworkProductHost::TakeTravelFailure() {
        return mode_->TakeTravelFailure();
    }

    Result<void> NetworkProductHost::AdmitClientSession(const Network::NetworkModeWorldKind world,
                                                        const Network::PeerSessionLifecycle &session,
                                                        const Network::ConnectionHandle connection,
                                                        const Network::NetworkOperationGeneration generation, const std::uint64_t nowTick) {
        return mode_->Mode().AdmitClientSession(world, session, connection, generation, nowTick);
    }

    Result<void> NetworkProductHost::DisconnectClient(const Network::NetworkModeWorldKind world,
                                                      const Network::NetworkOperationGeneration generation) {
        return mode_->Mode().DisconnectClient(world, generation);
    }

    Result<Network::NetworkModeRoleView> NetworkProductHost::Role(const Network::NetworkModeWorldKind world,
                                                                  const Runtime::SceneRuntimeId scene) const {
        return mode_->Mode().Role(world, scene);
    }

    void NetworkProductHost::Shutdown() noexcept {
        if (runtime_ != nullptr)
            runtime_->Shutdown();
    }
}  // namespace Horo::Application::Internal
