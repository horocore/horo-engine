#include "Horo/Network/NetworkModeComposition.h"

#include "Horo/Network/NetworkErrors.h"

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
            if (!world.scene.IsValid() || (world.kind == NetworkModeWorldKind::AuthorityServer) != world.authority.IsValid() ||
                (index != 0 && worlds[0].scene == world.scene)) {
                return Result<NetworkModePlan>::Failure(
                    MakeError(NetworkErrors::NetworkModeInvalid, "World identities or authority epochs do not match the selected mode."));
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

    NetworkModeComposition::NetworkModeComposition(NetworkModePlan plan, NetworkModeFactories factories) noexcept
        : plan_(std::move(plan)), factories_(std::move(factories)) {}

    NetworkModeComposition::NetworkModeComposition(NetworkModeComposition &&other) noexcept
        : plan_(std::move(other.plan_)), factories_(std::move(other.factories_)), entries_(std::move(other.entries_)),
          entryCount_(std::exchange(other.entryCount_, 0)), sessions_(std::move(other.sessions_)),
          lastSessions_(std::move(other.lastSessions_)), started_(std::exchange(other.started_, false)),
          stopped_(std::exchange(other.stopped_, true)) {}

    NetworkModeComposition::~NetworkModeComposition() noexcept {
        Shutdown();
    }

    /** @copydoc NetworkModeComposition::Create */
    Result<NetworkModeComposition> NetworkModeComposition::Create(NetworkModePlan plan, NetworkModeFactories factories) {
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
            if (!world.scene.IsValid() || (world.kind == NetworkModeWorldKind::AuthorityServer) != world.authority.IsValid() ||
                (index != 0 && plan.worlds[0].scene == world.scene)) {
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
        return Result<NetworkModeComposition>::Success(NetworkModeComposition(std::move(plan), std::move(factories)));
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
        if (auto built = BuildEntries(); built.HasError()) {
            ReleaseEntries();
            return built;
        }
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto prepared = entries_[index].service->Prepare();
            if (prepared.HasError()) {
                ReleaseEntries();
                return prepared;
            }
        }
        for (std::size_t index = 0; index < entryCount_; ++index) {
            auto activated = entries_[index].service->Activate();
            if (activated.HasError()) {
                ReleaseEntries();
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
    Result<void> NetworkModeComposition::AdmitClientSession(const NetworkModeWorldKind world, const PeerSessionLifecycle &session,
                                                            const ConnectionHandle connection, const NetworkOperationGeneration generation,
                                                            const std::uint64_t nowTick) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        const auto index = WorldIndex(world);
        if (world != NetworkModeWorldKind::Client || index == plan_.worldCount)
            return Invalid("Only the selected client world may receive an admitted gameplay session.");
        if (auto admitted = session.AdmitGameplay(connection, generation, nowTick); admitted.HasError())
            return admitted;
        if (sessions_[index].has_value() && sessions_[index] == generation)
            return Result<void>::Success();
        if (lastSessions_[index].has_value() && generation <= *lastSessions_[index])
            return Stale("The admitted session generation does not advance the current owner.");
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
        sessions_[index].reset();
        return Result<void>::Success();
    }

    Result<void> NetworkModeComposition::AppendTravelEntry(std::array<Entry, 5> &prepared, std::size_t &count,
                                                           const NetworkModeServiceRequest &request) {
        auto created = factories_.services[static_cast<std::size_t>(request.service)](request);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        if (created.Value() == nullptr)
            return Unavailable("A travel factory returned no world service.");
        prepared[count++] = {.request = request, .service = std::move(created).Value()};
        return Result<void>::Success();
    }

    void NetworkModeComposition::DiscardTravelEntries(std::array<Entry, 5> &prepared, std::size_t count) noexcept {
        while (count != 0) {
            auto &entry = prepared[--count];
            if (entry.service != nullptr)
                entry.service->Shutdown();
        }
    }

    Result<std::size_t> NetworkModeComposition::PrepareTravelEntries(const std::span<const NetworkModeWorld> replacements,
                                                                     std::array<Entry, 5> &prepared) {
        std::size_t count = 0;
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            for (const auto service : {NetworkModeServiceKind::Scene, NetworkModeServiceKind::Physics}) {
                auto added = AppendTravelEntry(prepared, count, Request(plan_, service, &replacements[index]));
                if (added.HasError()) {
                    DiscardTravelEntries(prepared, count);
                    return Result<std::size_t>::Failure(added.ErrorValue());
                }
            }
        }
        if (Needs(plan_, NetworkModeServiceKind::LocalPlayer)) {
            auto added = AppendTravelEntry(prepared, count,
                                           Request(plan_, NetworkModeServiceKind::LocalPlayer, &replacements[plan_.worldCount - 1]));
            if (added.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(added.ErrorValue());
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto result = prepared[index].service->Prepare();
            if (result.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(result.ErrorValue());
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto result = prepared[index].service->Activate();
            if (result.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(result.ErrorValue());
            }
        }
        return Result<std::size_t>::Success(count);
    }

    void NetworkModeComposition::CommitTravelEntries(const std::span<const NetworkModeWorld> replacements, std::array<Entry, 5> &prepared,
                                                     const std::size_t count) noexcept {
        // Owner-thread safe-point publication has no failing step after preparation.
        // Hide the old role views before swapping either member of a listen pair.
        started_ = false;
        std::array<std::unique_ptr<INetworkModeService>, 5> oldServices{};
        for (std::size_t replacement = 0; replacement < count; ++replacement) {
            for (std::size_t current = 0; current < entryCount_; ++current) {
                if (entries_[current].request.service == prepared[replacement].request.service &&
                    entries_[current].request.world == prepared[replacement].request.world) {
                    oldServices[replacement] = std::move(entries_[current].service);
                    entries_[current] = std::move(prepared[replacement]);
                    break;
                }
            }
        }
        std::ranges::copy(replacements, plan_.worlds.begin());
        for (std::size_t index = count; index != 0; --index)
            oldServices[index - 1]->Shutdown();
        started_ = true;
    }

    /** @copydoc NetworkModeComposition::Travel */
    Result<void> NetworkModeComposition::Travel(const std::span<const NetworkModeWorld> replacements, const Runtime::RuntimePhase phase,
                                                const std::optional<NetworkOperationGeneration> admittedClientSession) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || !WorldShapeMatches(plan_.mode, replacements))
            return Invalid("Travel must replace the entire mode world set at the canonical lifecycle safe point.");
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (!replacements[index].scene.IsValid() || replacements[index].scene.value <= plan_.worlds[index].scene.value ||
                (replacements[index].kind == NetworkModeWorldKind::AuthorityServer) != replacements[index].authority.IsValid() ||
                (replacements[index].authority.IsValid() && replacements[index].authority <= plan_.worlds[index].authority) ||
                (index != 0 && replacements[0].scene == replacements[index].scene))
                return Invalid("Travel must supply fresh distinct Scene and authority generations.");
        }
        if (const auto clientIndex = WorldIndex(NetworkModeWorldKind::Client);
            clientIndex != plan_.worldCount && (!admittedClientSession.has_value() || sessions_[clientIndex] != admittedClientSession))
            return Stale("Client travel requires the current admitted session generation.");
        std::array<Entry, 5> prepared{};
        auto ready = PrepareTravelEntries(replacements, prepared);
        if (ready.HasError())
            return Result<void>::Failure(ready.ErrorValue());
        CommitTravelEntries(replacements, prepared, ready.Value());
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::Role */
    Result<NetworkModeRoleView> NetworkModeComposition::Role(const NetworkModeWorldKind world, const Runtime::SceneRuntimeId scene) const {
        if (!started_ || stopped_)
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, "Mode worlds are not published."));
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
        return Result<NetworkModeRoleView>::Success({plan_.hostGeneration, scene, role, sessions_[index], plan_.worlds[index].authority});
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
            const auto current = Role(plan_.worlds[index].kind, view.scene);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().role != view.role || current.Value().session != view.session || current.Value().authority != view.authority)
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
        ReleaseEntries();
    }

    /** @copydoc NetworkModeComposition::Active */
    bool NetworkModeComposition::Active() const noexcept {
        return started_ && !stopped_;
    }
}  // namespace Horo::Network
