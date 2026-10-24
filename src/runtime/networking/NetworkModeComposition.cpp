#include "Horo/Network/NetworkModeComposition.h"

#include "Horo/Network/NetworkErrors.h"
#include "NetworkModeHostAccess.h"

#include <algorithm>
#include <utility>

namespace Horo::Network {
    namespace {
        [[nodiscard]] Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid, message));
        }

        [[nodiscard]] Result<void> Unavailable(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, message));
        }

        [[nodiscard]] Result<void> Stale(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale, message));
        }

        [[nodiscard]] bool WorldShapeMatches(const NetworkProjectRole mode, const std::span<const NetworkModeWorld> worlds) noexcept {
            switch (mode) {
                case NetworkProjectRole::Standalone:
                    return worlds.size() == 1 && worlds[0].kind == NetworkModeWorldKind::Standalone;
                case NetworkProjectRole::Client:
                    return worlds.size() == 1 && worlds[0].kind == NetworkModeWorldKind::Client;
                case NetworkProjectRole::ListenServer:
                    return worlds.size() == 2 && worlds[0].kind == NetworkModeWorldKind::AuthorityServer &&
                           worlds[1].kind == NetworkModeWorldKind::Client;
                case NetworkProjectRole::DedicatedServer:
                    return worlds.size() == 1 && worlds[0].kind == NetworkModeWorldKind::AuthorityServer;
                case NetworkProjectRole::Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool Needs(const NetworkModePlan &plan, const NetworkModeServiceKind service) noexcept {
            const bool networked = plan.mode != NetworkProjectRole::Standalone;
            using enum NetworkModeServiceKind;
            switch (service) {
                case Transport:
                case Session:
                case Replication:
                    return networked;
                case Scene:
                case Physics:
                    return true;
                case Audio:
                    return plan.presentation.audio;
                case Renderer:
                    return plan.presentation.renderer;
                case Gui:
                    return plan.presentation.gui;
                case Input:
                    return plan.presentation.input;
                case LocalPlayer:
                    return plan.presentation.localPlayer;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] NetworkModeServiceRequest Request(const NetworkModePlan &plan, const NetworkModeServiceKind service,
                                                        const NetworkModeWorld *world = nullptr) noexcept {
            return {service, plan.mode, plan.provider, world == nullptr ? NetworkModeWorldKind::Count : world->kind,
                    world == nullptr ? Runtime::SceneRuntimeId{} : world->scene};
        }
    }  // namespace

    /** @copydoc ResolveNetworkModePlan */
    Result<NetworkModePlan> ResolveNetworkModePlan(const NetworkTargetAssessment &assessment, const NetworkTargetSelection &selection,
                                                   const std::uint64_t hostGeneration, const std::span<const NetworkModeWorld> worlds,
                                                   const NetworkModePresentation presentation) {
        if (!assessment.Admitted() || selection.lifecycle != NetworkTargetLifecycle::Active || hostGeneration == 0 ||
            !WorldShapeMatches(selection.role, worlds) || !selection.expectedBuild.IsValid() ||
            selection.expectedBuild != assessment.matrix.build || selection.expectedProductRevision != assessment.matrix.productRevision ||
            selection.expectedHostRevision != assessment.matrix.hostRevision ||
            selection.expectedProjectRevision != assessment.matrix.projectRevision ||
            !assessment.matrix.roles[static_cast<std::size_t>(selection.role)].selected) {
            return Result<NetworkModePlan>::Failure(
                MakeError(NetworkErrors::NetworkModeInvalid, "The selected mode does not match an admitted exact target assessment."));
        }
        if (selection.role == NetworkProjectRole::DedicatedServer &&
            (presentation.audio || presentation.renderer || presentation.gui || presentation.input || presentation.localPlayer)) {
            return Result<NetworkModePlan>::Failure(
                MakeError(NetworkErrors::NetworkModeInvalid,
                          "Dedicated mode cannot install presentation, input or local-player services."));
        }
        if ((selection.role == NetworkProjectRole::Client || selection.role == NetworkProjectRole::ListenServer) &&
            !presentation.localPlayer) {
            return Result<NetworkModePlan>::Failure(
                MakeError(NetworkErrors::NetworkModeInvalid, "Client modes require an explicitly owned local-player service."));
        }
        for (std::size_t index = 0; index < worlds.size(); ++index) {
            const auto &world = worlds[index];
            if (!world.scene.IsValid() || (index != 0 && worlds[0].scene == world.scene)) {
                return Result<NetworkModePlan>::Failure(
                    MakeError(NetworkErrors::NetworkModeInvalid, "World identities do not match the selected mode."));
            }
        }
        NetworkModePlan plan{.mode = selection.role,
                             .hostGeneration = hostGeneration,
                             .build = assessment.matrix.build,
                             .productRevision = assessment.matrix.productRevision,
                             .hostRevision = assessment.matrix.hostRevision,
                             .projectRevision = assessment.matrix.projectRevision,
                             .provider = selection.provider,
                             .protocolVersion = selection.protocolVersion,
                             .presentation = presentation,
                             .worldCount = worlds.size()};
        std::ranges::copy(worlds, plan.worlds.begin());
        return Result<NetworkModePlan>::Success(plan);
    }

    bool NetworkModeComposition::WorldShapeMatches(const NetworkProjectRole mode, const std::span<const NetworkModeWorld> worlds) noexcept {
        return Horo::Network::WorldShapeMatches(mode, worlds);
    }

    NetworkModeServiceRequest NetworkModeComposition::Request(const NetworkModePlan &plan, const NetworkModeServiceKind service,
                                                              const NetworkModeWorld *world) noexcept {
        return Horo::Network::Request(plan, service, world);
    }

    NetworkModeComposition::NetworkModeComposition(NetworkModePlan plan, NetworkModeFactories factories,
                                                   const NetworkWorldCompositionPurpose purpose,
                                                   std::shared_ptr<NetworkWorldBindingController> binding) noexcept
        : plan_(std::move(plan)), factories_(std::move(factories)), purpose_(purpose), binding_(std::move(binding)) {}

    NetworkModeComposition::NetworkModeComposition(NetworkModeComposition &&other) noexcept
        : plan_(std::move(other.plan_)), factories_(std::move(other.factories_)), purpose_(other.purpose_),
          binding_(std::move(other.binding_)), entries_(std::move(other.entries_)), entryCount_(std::exchange(other.entryCount_, 0)),
          travelEntries_(std::move(other.travelEntries_)), travelWorlds_(other.travelWorlds_),
          travelEntryCount_(std::exchange(other.travelEntryCount_, 0)), travelFailure_(std::move(other.travelFailure_)),
          travelPending_(std::exchange(other.travelPending_, false)), sessions_(std::move(other.sessions_)),
          lastSessions_(std::move(other.lastSessions_)), started_(std::exchange(other.started_, false)),
          stopped_(std::exchange(other.stopped_, true)) {}

    NetworkModeComposition::~NetworkModeComposition() noexcept {
        Shutdown();
    }

    /** @copydoc NetworkModeComposition::Create */
    Result<NetworkModeComposition> NetworkModeComposition::Create(NetworkModePlan plan, NetworkModeFactories factories,
                                                                  const NetworkWorldCompositionPurpose purpose,
                                                                  std::optional<NetworkWorldBindingSetup> binding) {
        if (plan.hostGeneration == 0 || plan.worldCount == 0 || plan.worldCount > plan.worlds.size() || !plan.build.IsValid() ||
            !plan.productRevision.IsValid() || !plan.hostRevision.IsValid() || !plan.projectRevision.IsValid() ||
            !WorldShapeMatches(plan.mode, {plan.worlds.data(), plan.worldCount}) ||
            ((plan.mode == NetworkProjectRole::Standalone) != !plan.provider.IsValid()) ||
            ((plan.mode == NetworkProjectRole::Standalone) != !plan.protocolVersion.IsValid()) ||
            (plan.mode == NetworkProjectRole::DedicatedServer &&
             (plan.presentation.audio || plan.presentation.renderer || plan.presentation.gui || plan.presentation.input ||
              plan.presentation.localPlayer)) ||
            ((plan.mode == NetworkProjectRole::Client || plan.mode == NetworkProjectRole::ListenServer) &&
             !plan.presentation.localPlayer)) {
            return Result<NetworkModeComposition>::Failure(MakeError(NetworkErrors::NetworkModeInvalid, "Invalid mode plan."));
        }
        for (std::size_t index = 0; index < plan.worldCount; ++index) {
            const auto &world = plan.worlds[index];
            if (!world.scene.IsValid() || (index != 0 && plan.worlds[0].scene == world.scene)) {
                return Result<NetworkModeComposition>::Failure(MakeError(NetworkErrors::NetworkModeInvalid, "Invalid world identity."));
            }
        }
        for (std::size_t index = 0; index < static_cast<std::size_t>(NetworkModeServiceKind::Count); ++index) {
            const auto service = static_cast<NetworkModeServiceKind>(index);
            if (Needs(plan, service) && !factories.services[index]) {
                return Result<NetworkModeComposition>::Failure(
                    MakeError(NetworkErrors::NetworkModeUnavailable, "A selected mode service has no host factory."));
            }
        }
        if (purpose >= NetworkWorldCompositionPurpose::Count ||
            (plan.mode != NetworkProjectRole::Standalone && purpose == NetworkWorldCompositionPurpose::Gameplay && !binding))
            return Result<NetworkModeComposition>::Failure(
                MakeError(NetworkErrors::AdmissionPolicyInvalid,
                          "Requested gameplay requires explicit authenticated world-binding policy."));
        std::shared_ptr<NetworkWorldBindingController> owner;
        if (plan.mode != NetworkProjectRole::Standalone && purpose == NetworkWorldCompositionPurpose::Gameplay) {
            auto created = NetworkWorldBindingController::Create(plan, std::move(*binding));
            if (created.HasError())
                return Result<NetworkModeComposition>::Failure(created.ErrorValue());
            owner = std::move(created).Value();
        }
        return Result<NetworkModeComposition>::Success(
            NetworkModeComposition(std::move(plan), std::move(factories), purpose, std::move(owner)));
    }

    Result<void> NetworkModeComposition::BuildEntries() {
        const auto append = [this](const NetworkModeServiceRequest &request) {
            auto created = factories_.services[static_cast<std::size_t>(request.service)](request);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            if (created.Value() == nullptr)
                return Unavailable("A required host factory returned no service.");
            entries_[entryCount_++] = {.request = request, .service = std::move(created).Value()};
            return Result<void>::Success();
        };
        for (const auto service :
             {NetworkModeServiceKind::Transport, NetworkModeServiceKind::Session, NetworkModeServiceKind::Replication}) {
            if (Needs(plan_, service)) {
                auto added = append(Request(plan_, service));
                if (added.HasError())
                    return added;
            }
        }
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            const auto &world = plan_.worlds[index];
            for (const auto service : {NetworkModeServiceKind::Scene, NetworkModeServiceKind::Physics}) {
                auto added = append(Request(plan_, service, &world));
                if (added.HasError())
                    return added;
            }
        }
        for (const auto service : {NetworkModeServiceKind::Audio, NetworkModeServiceKind::Renderer, NetworkModeServiceKind::Gui,
                                   NetworkModeServiceKind::Input}) {
            if (Needs(plan_, service)) {
                auto added = append(Request(plan_, service));
                if (added.HasError())
                    return added;
            }
        }
        if (Needs(plan_, NetworkModeServiceKind::LocalPlayer)) {
            const auto world = plan_.worlds[plan_.worldCount - 1];
            auto added = append(Request(plan_, NetworkModeServiceKind::LocalPlayer, &world));
            if (added.HasError())
                return added;
        }
        return Result<void>::Success();
    }

    void NetworkModeComposition::ReleaseEntries() noexcept {
        while (entryCount_ != 0) {
            auto &entry = entries_[--entryCount_];
            if (entry.service != nullptr)
                entry.service->Shutdown();
            entry.service.reset();
        }
    }

    /** @copydoc NetworkModeComposition::Start */
    Result<void> NetworkModeComposition::Start() {
        if (stopped_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeShuttingDown, "Mode composition has stopped."));
        if (started_)
            return Invalid("Mode composition has already started.");
        if (binding_) {
            if (auto staged = binding_->Prepare(); staged.HasError())
                return staged;
        }
        if (auto built = BuildEntries(); built.HasError()) {
            Shutdown();
            return built;
        }
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto prepared = entries_[index].service->Prepare();
            if (prepared.HasError()) {
                Shutdown();
                return prepared;
            }
        }
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto activated = entries_[index].service->Activate();
            if (activated.HasError()) {
                Shutdown();
                return activated;
            }
        }
        started_ = true;
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::RunPhase */
    Result<void> NetworkModeComposition::RunPhase(const Runtime::RuntimePhase phase) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        if (phase > Runtime::RuntimePhase::EndFrame || phase == Runtime::RuntimePhase::FixedUpdate)
            return Invalid("A fixed tick requires its canonical context.");
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto advanced = entries_[index].service->RunPhase(phase);
            if (advanced.HasError())
                return advanced;
        }
        if (binding_ && phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges) {
            auto advanced = travelPending_ ? AdvanceTravel(phase) : binding_->Commit(phase);
            if (advanced.HasError() && travelPending_) {
                travelFailure_ = advanced.ErrorValue();
                (void)binding_->CancelTravel();
                DiscardTravelEntries(travelEntries_, travelEntryCount_);
                travelEntryCount_ = 0;
                travelPending_ = false;
                return Result<void>::Success();
            }
            return advanced;
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::RunFixedTick */
    Result<void> NetworkModeComposition::RunFixedTick(const Runtime::FixedStepContext &context) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto advanced = entries_[index].service->RunFixedTick(context);
            if (advanced.HasError())
                return advanced;
        }
        return Result<void>::Success();
    }

    std::size_t NetworkModeComposition::WorldIndex(const NetworkModeWorldKind kind) const noexcept {
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (plan_.worlds[index].kind == kind)
                return index;
        }
        return plan_.worldCount;
    }

    /** @copydoc NetworkModeComposition::AdmitClientSession */
    Result<void> NetworkModeComposition::AdmitClientSession(const NetworkModeWorldKind world, std::shared_ptr<PeerSessionLifecycle> session,
                                                            const ConnectionHandle connection, const NetworkOperationGeneration generation,
                                                            const std::uint64_t nowTick) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        const auto index = WorldIndex(world);
        if (world != NetworkModeWorldKind::Client || index == plan_.worldCount)
            return Invalid("Only the selected client world may receive an admitted gameplay session.");
        if (!session)
            return Invalid("Client admission requires the same owned canonical peer lifecycle.");
        if (auto admitted = session->AdmitGameplay(connection, generation, nowTick); admitted.HasError())
            return admitted;
        if (sessions_[index].has_value() && sessions_[index] == generation)
            return binding_ ? binding_->BindPeer(AdmissionEndpoint::Client, std::move(session), generation, nowTick)
                            : Result<void>::Success();
        if (lastSessions_[index].has_value() && generation <= *lastSessions_[index])
            return Stale("The admitted session generation does not advance the current owner.");
        if (binding_) {
            if (auto bound = binding_->BindPeer(AdmissionEndpoint::Client, std::move(session), generation, nowTick); bound.HasError())
                return bound;
        }
        sessions_[index] = generation;
        lastSessions_[index] = generation;
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::DisconnectClient */
    Result<void> NetworkModeComposition::DisconnectClient(const NetworkModeWorldKind world, const NetworkOperationGeneration generation) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        const auto index = WorldIndex(world);
        if (world != NetworkModeWorldKind::Client || index == plan_.worldCount)
            return Invalid("Only a client world may disconnect a gameplay session.");
        if (!sessions_[index].has_value() || sessions_[index] != generation)
            return Stale("The disconnected session is not current.");
        if (binding_) {
            if (auto revoked = binding_->UnbindClient(generation); revoked.HasError())
                return revoked;
        }
        sessions_[index].reset();
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::Role */
    Result<NetworkModeRoleView> NetworkModeComposition::Role(const NetworkModeWorldKind world, const Runtime::SceneRuntimeId scene) const {
        if (!started_ || stopped_)
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, "Mode worlds are not published."));
        if (binding_)
            return binding_->Role(world, scene);
        if (plan_.mode != NetworkProjectRole::Standalone)
            return Result<NetworkModeRoleView>::Failure(
                MakeError(NetworkErrors::NetworkModeUnavailable, "Transport qualification does not publish gameplay world roles."));
        const auto index = WorldIndex(world);
        if (index == plan_.worldCount || plan_.worlds[index].scene != scene)
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeStale, "World role identity is stale."));
        const auto role = [world] {
            switch (world) {
                case NetworkModeWorldKind::Standalone:
                    return ReplicationExecutionRole::Standalone;
                case NetworkModeWorldKind::AuthorityServer:
                    return ReplicationExecutionRole::AuthorityServer;
                case NetworkModeWorldKind::Client:
                    return ReplicationExecutionRole::AutonomousClient;
                case NetworkModeWorldKind::Count:
                    return ReplicationExecutionRole::Count;
            }
            return ReplicationExecutionRole::Count;
        }();
        return Result<NetworkModeRoleView>::Success({plan_.hostGeneration, scene, role, sessions_[index], {}});
    }

    /** @copydoc NetworkModeHostAccess::Binding */
    std::weak_ptr<NetworkWorldBindingController> NetworkModeHostAccess::Binding(NetworkModeComposition &mode) noexcept {
        return mode.binding_;
    }

    /** @copydoc NetworkModeComposition::AcquireReplication */
    Result<ReplicationWorldReadLease> NetworkModeComposition::AcquireReplication(const NetworkModeWorldKind world,
                                                                                 const ReplicationWorldWorkRequest &request,
                                                                                 const ReplicationWorldCapability capability) const {
        if (!started_ || stopped_ || !binding_)
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        return binding_->Acquire(world, request, capability);
    }

    /** @copydoc NetworkModeComposition::ValidateRole */
    Result<void> NetworkModeComposition::ValidateRole(const NetworkModeRoleView &view) const {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        if (view.hostGeneration != plan_.hostGeneration)
            return Stale("The role view belongs to a replaced host generation.");
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (plan_.worlds[index].scene != view.scene)
                continue;
            if (sessions_[index] != view.session)
                return Stale("The role view belongs to a retired peer generation.");
            const auto current = Role(plan_.worlds[index].kind, view.scene);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().role != view.role || current.Value().session != view.session ||
                current.Value().authority != view.authority || current.Value().worldSession != view.worldSession)
                return Stale("The role view was replaced by travel, reconnect or disconnect.");
            return Result<void>::Success();
        }
        return Stale("The role view names an old Scene generation.");
    }

    /** @copydoc NetworkModeComposition::Shutdown */
    void NetworkModeComposition::Shutdown() noexcept {
        if (stopped_)
            return;
        started_ = false;
        sessions_ = {};
        lastSessions_ = {};
        stopped_ = true;
        if (binding_)
            binding_->Shutdown();
        DiscardTravelEntries(travelEntries_, travelEntryCount_);
        travelEntryCount_ = 0;
        travelPending_ = false;
        ReleaseEntries();
    }

    /** @copydoc NetworkModeComposition::ServicesStarted */
    bool NetworkModeComposition::ServicesStarted() const noexcept {
        return started_ && !stopped_;
    }

    /** @copydoc NetworkModeComposition::Active */
    bool NetworkModeComposition::Active() const noexcept {
        if (!started_ || stopped_)
            return false;
        if (binding_)
            for (std::size_t index = 0; index < plan_.worldCount; ++index)
                if (!binding_->Ready(plan_.worlds[index].kind, plan_.worlds[index].scene))
                    return false;
        return true;
    }
}  // namespace Horo::Network
