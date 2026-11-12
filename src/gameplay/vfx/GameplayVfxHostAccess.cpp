#include "GameplayVfxContextInternal.h"
#include "Horo/Foundation/Utf8.h"

namespace Horo::Gameplay::Detail {
    namespace {
        using VfxBindingDetail::Reject;

        [[nodiscard]] bool ValidGrant(const GameplayVfxGrant &grant) noexcept {
            if (grant.moduleId.empty() || grant.moduleId.size() > 256 || !IsValidUtf8ScalarSequence(grant.moduleId) ||
                grant.moduleId.find('\0') != std::string::npos)
                return false;
            if (!grant.owner.IsValid() || grant.ownerGeneration == 0 || grant.producer == 0)
                return false;
            return grant.events.size() <= GameplayVfxBindingLimits::Bindings && grant.tags.size() <= GameplayVfxBindingLimits::Bindings;
        }

        [[nodiscard]] bool ValidAllowedEvents(const GameplayVfxGrant &grant) noexcept {
            for (std::size_t index = 0; index < grant.events.size(); ++index) {
                if (!VfxBindingDetail::ValidText(grant.events[index].Value()))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (grant.events[index] == grant.events[previous])
                        return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidAllowedTags(const GameplayVfxGrant &grant) noexcept {
            for (std::size_t index = 0; index < grant.tags.size(); ++index) {
                if (!grant.tags[index].IsValid())
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (grant.tags[index] == grant.tags[previous])
                        return false;
            }
            return true;
        }

        [[nodiscard]] bool UniqueGrant(const std::span<const GameplayVfxGrant> grants, const std::size_t index) noexcept {
            for (std::size_t previous = 0; previous < index; ++previous)
                if (grants[index].moduleId == grants[previous].moduleId || grants[index].owner == grants[previous].owner ||
                    grants[index].producer == grants[previous].producer)
                    return false;
            return true;
        }

        [[nodiscard]] bool ValidSourceBudget(const GameplayVfxSourceBudget budget) noexcept {
            return budget.maximumPending > 0 && budget.maximumPending <= Vfx::VfxSpawnHardLimits::Pending && budget.maximumModules > 0 &&
                   budget.maximumModules <= Vfx::VfxSpawnHardLimits::Producers && budget.maximumBytes > 0 &&
                   budget.maximumBytes <= 16U * 1024U * 1024U;
        }

        [[nodiscard]] Result<void> ValidateGrants(const std::span<const GameplayVfxGrant> grants, const GameplayVfxSourceBudget budget) {
            if (!ValidSourceBudget(budget) || grants.empty() || grants.size() > budget.maximumModules)
                return Reject<void>(GameplayVfxErrors::ScopeInvalid);
            for (std::size_t index = 0; index < grants.size(); ++index) {
                const auto &grant = grants[index];
                if (!ValidGrant(grant) || !ValidAllowedEvents(grant) || !ValidAllowedTags(grant) || !UniqueGrant(grants, index))
                    return Reject<void>(GameplayVfxErrors::ScopeInvalid);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool ParameterFits(const GameplayVfxCompiledBinding &binding, const GameplayVfxCompiledParameter &parameter,
                                         const Vfx::VfxSpawnAssetInfo &asset) noexcept {
            const auto values = std::span{asset.parameters.data(), asset.parameterCount};
            const auto target = std::ranges::find(values, parameter.target, &Vfx::VfxSpawnParameter::channel);
            if (target == values.end())
                return false;
            if (!parameter.fromInput)
                return parameter.constant >= target->minimum && parameter.constant <= target->maximum;
            const auto &source = binding.inputs[parameter.sourceOrdinal];
            return source.minimum >= target->minimum && source.maximum <= target->maximum;
        }

        [[nodiscard]] bool LayerFits(const GameplayVfxCompiledBinding &binding, const GameplayVfxCompiledLayer &layer,
                                     const Vfx::VfxSpawnAssetInfo &asset) noexcept {
            if (layer.schema != asset.parameterSchema)
                return false;
            if (binding.requirement == Vfx::VfxRequirementClass::GameplayRequired &&
                (!asset.requiredGameplay || asset.requiredReserve == 0))
                return false;
            for (const auto &parameter : std::span{layer.parameters.data(), layer.parameterCount})
                if (!ParameterFits(binding, parameter, asset))
                    return false;
            return true;
        }

        [[nodiscard]] Vfx::VfxSpawnStatus WorldStatus(const GameplayVfxBindingTable &table, Vfx::VfxWorld &world) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            const auto &compiled = GameplayVfxBindingAccess::State(table);
            for (const auto &binding : compiled.bindings)
                for (const auto &layer : std::span{binding.layers.data(), binding.layerCount}) {
                    Vfx::VfxSpawnAssetInfo asset;
                    if (world.InspectSpawnAsset(layer.asset, asset) != Accepted)
                        return MissingAsset;
                    if (!LayerFits(binding, layer, asset))
                        return ParameterSchemaMismatch;
                }
            return Accepted;
        }

        [[nodiscard]] Result<void> ValidateWorld(const GameplayVfxBindingTable &table, Vfx::VfxWorld &world) {
            const auto status = WorldStatus(table, world);
            if (status == Vfx::VfxSpawnStatus::MissingAsset)
                return Reject<void>(GameplayVfxErrors::AssetUnavailable);
            if (status != Vfx::VfxSpawnStatus::Accepted)
                return Reject<void>(GameplayVfxErrors::ParameterMismatch);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AdmitOwners(Vfx::VfxWorld &world, const std::span<const GameplayVfxGrant> grants) {
            for (const auto &grant : grants)
                if (world.AdmitSpawnOwner(grant.owner, grant.ownerGeneration) != Vfx::VfxSpawnStatus::Accepted)
                    return Reject<void>(GameplayVfxErrors::ScopeInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<Vfx::VfxSpawnAssetInfo>> ReloadPalette(Vfx::VfxWorld &world,
                                                                                const GameplayVfxBindingDocument &document) {
            std::vector<Vfx::VfxSpawnAssetInfo> palette;
            palette.reserve(Vfx::VfxSpawnHardLimits::Effects);
            for (const auto &binding : document.bindings)
                for (const auto &layer : binding.layers) {
                    if (std::ranges::find(palette, layer.effectAsset, &Vfx::VfxSpawnAssetInfo::asset) != palette.end())
                        continue;
                    if (palette.size() == Vfx::VfxSpawnHardLimits::Effects)
                        return Reject<std::vector<Vfx::VfxSpawnAssetInfo>>(GameplayVfxErrors::LimitExceeded);
                    Vfx::VfxSpawnAssetInfo info;
                    if (world.InspectSpawnAsset(layer.effectAsset, info) != Vfx::VfxSpawnStatus::Accepted)
                        return Reject<std::vector<Vfx::VfxSpawnAssetInfo>>(GameplayVfxErrors::AssetUnavailable);
                    palette.push_back(info);
                }
            return Result<std::vector<Vfx::VfxSpawnAssetInfo>>::Success(std::move(palette));
        }

        [[nodiscard]] bool ReloadReady(const GameplayVfxSceneState &state, const std::uint64_t generation) noexcept {
            return !state.sourceOpen && state.pendingCount == 0 && state.sealCount == 0 && generation > state.table->Generation();
        }

        struct ReloadScope final {
            explicit ReloadScope(GameplayVfxSceneState &value) : state(value) {
                state.busy = true;
            }

            ReloadScope(const ReloadScope &) = delete;
            ReloadScope &operator=(const ReloadScope &) = delete;

            ~ReloadScope() {
                state.busy = false;
            }

            GameplayVfxSceneState &state;
        };

        [[nodiscard]] std::size_t GrantBytes(const GameplayVfxGrant &grant) noexcept {
            std::size_t bytes = grant.moduleId.capacity() + grant.events.capacity() * sizeof(GameplayEventTypeId) +
                                grant.tags.capacity() * sizeof(GameplayVfxEffectTag);
            for (const auto &event : grant.events)
                bytes += event.Value().capacity();
            return bytes;
        }

        /** @brief Charges actual source vectors/grant payload plus a conservative fixed root control-block allowance. */
        [[nodiscard]] std::size_t SourceBytes(const GameplayVfxSceneState &state) noexcept {
            std::size_t bytes = sizeof(state) + state.pending.capacity() * sizeof(GameplayVfxPending) +
                                state.results.capacity() * sizeof(GameplayVfxSourceRecord) +
                                state.sealed.capacity() * sizeof(GameplayVfxSealedTick) +
                                state.modules.capacity() * sizeof(GameplayVfxModuleSlot);
            for (const auto &moduleSlot : state.modules)
                bytes += sizeof(GameplayVfxModuleState) + sizeof(GameplayVfxClientState) + sizeof(GameplayVfxProducerState) +
                         sizeof(GameplayVfxContext) + sizeof(GameplayVfxCommittedProducer) + 512U + GrantBytes(moduleSlot.state->grant);
            return bytes;
        }

        [[nodiscard]] std::shared_ptr<GameplayVfxSceneState> PrepareState(Vfx::VfxWorld &world,
                                                                          std::shared_ptr<const GameplayVfxBindingTable> table,
                                                                          const GameplayVfxSourceBudget budget, const float maximumDelta) {
            auto state = std::make_shared<GameplayVfxSceneState>();
            state->world = &world;
            state->scene = world.Scene();
            state->owner = std::this_thread::get_id();
            state->witness = GameplayVfxBindingAccess::State(*table).bindings.front().layers.front().asset;
            state->table = std::move(table);
            state->pending.resize(budget.maximumPending);
            state->results.resize(budget.maximumPending);
            state->sealed.resize(budget.maximumPending);
            state->modules.reserve(budget.maximumModules);
            state->maximumDelta = maximumDelta;
            return state;
        }
    }  // namespace

    GameplayVfxModuleSlot GameplayVfxHostAccess::MakeModule(const std::shared_ptr<GameplayVfxSceneState> &scene,
                                                            const GameplayVfxGrant &grant) {
        auto moduleState = std::make_shared<GameplayVfxModuleState>();
        moduleState->grant = grant;
        moduleState->scene = scene->scene;
        moduleState->owner = scene->owner;
        auto client = std::make_shared<GameplayVfxClientState>();
        client->scene = scene;
        client->moduleState = moduleState;
        client->lifetime = std::make_shared<GameplayVfxScopeLifetime>();
        auto producer = std::make_shared<GameplayVfxProducerState>();
        producer->scene = scene;
        producer->moduleState = moduleState;
        std::shared_ptr<const GameplayVfxContext> context{new GameplayVfxContext{std::move(client)}};
        std::shared_ptr<const GameplayVfxCommittedProducer> committed{new GameplayVfxCommittedProducer{std::move(producer)}};
        return {std::move(moduleState), std::move(context), std::move(committed)};
    }

    Result<GameplayVfxSceneLease> GameplayVfxHostAccess::Prepare(Vfx::VfxWorld &world, std::shared_ptr<const GameplayVfxBindingTable> table,
                                                                 const std::span<const GameplayVfxGrant> grants,
                                                                 const GameplayVfxSourceBudget budget, const float maximumDeltaSeconds) {
        if (!table || !std::isfinite(maximumDeltaSeconds) || maximumDeltaSeconds <= 0.0F)
            return Reject<GameplayVfxSceneLease>(GameplayVfxErrors::ScopeInvalid);
        try {
            if (auto valid = ValidateGrants(grants, budget); valid.HasError())
                return Result<GameplayVfxSceneLease>::Failure(valid.ErrorValue());
            if (auto valid = ValidateWorld(*table, world); valid.HasError())
                return Result<GameplayVfxSceneLease>::Failure(valid.ErrorValue());
            auto state = PrepareState(world, std::move(table), budget, maximumDeltaSeconds);
            for (const auto &grant : grants)
                state->modules.push_back(MakeModule(state, grant));
            if (SourceBytes(*state) > budget.maximumBytes)
                return Reject<GameplayVfxSceneLease>(GameplayVfxErrors::LimitExceeded);
            if (auto admitted = AdmitOwners(world, grants); admitted.HasError())
                return Result<GameplayVfxSceneLease>::Failure(admitted.ErrorValue());
            return Result<GameplayVfxSceneLease>::Success({std::move(state)});
        } catch (const std::bad_alloc &) {
            return Reject<GameplayVfxSceneLease>(GameplayVfxErrors::AllocationFailed);
        }
    }

    void GameplayVfxHostAccess::Publish(const GameplayVfxSceneLease &lease) noexcept {
        if (lease.state && lease.state->owner == std::this_thread::get_id() && lease.state->world)
            lease.state->active = true;
    }

    Vfx::VfxIdentityScope GameplayVfxHostAccess::Scene(const GameplayVfxSceneLease &lease) noexcept {
        return lease.state ? lease.state->scene : Vfx::VfxIdentityScope{};
    }

    Vfx::VfxSpawnStatus GameplayVfxHostAccess::PreparedStatus(const GameplayVfxSceneLease &lease) noexcept {
        if (!lease.state || !lease.state->world)
            return Vfx::VfxSpawnStatus::StaleScene;
        if (lease.state->owner != std::this_thread::get_id())
            return Vfx::VfxSpawnStatus::ThreadViolation;
        Vfx::VfxSpawnAssetInfo witness;
        return lease.state->world->InspectSpawnAsset(lease.state->witness, witness);
    }

    void GameplayVfxHostAccess::Revoke(const GameplayVfxSceneLease &lease) noexcept {
        if (!lease.state || lease.state->owner != std::this_thread::get_id())
            return;
        auto &state = *lease.state;
        state.active = false;
        state.sourceOpen = false;
        for (const auto &moduleSlot : state.modules) {
            moduleSlot.state->active = false;
            if (state.world)
                static_cast<void>(state.world->CancelSpawnOwner(moduleSlot.state->grant.owner, moduleSlot.state->grant.ownerGeneration));
        }
        state.pendingCount = 0;
        state.world = nullptr;
    }

    std::shared_ptr<const GameplayVfxContext> GameplayVfxHostAccess::Module(const GameplayVfxSceneLease &lease,
                                                                            const std::string_view moduleId) noexcept {
        if (!lease.state || VfxContextDetail::Status(*lease.state) != Vfx::VfxSpawnStatus::Accepted)
            return {};
        for (const auto &moduleSlot : lease.state->modules)
            if (moduleSlot.state->grant.moduleId == moduleId)
                return moduleSlot.client;
        return {};
    }

    std::shared_ptr<const GameplayVfxCommittedProducer> GameplayVfxHostAccess::CommittedProducer(const GameplayVfxSceneLease &lease,
                                                                                                 const std::string_view moduleId) noexcept {
        if (!lease.state || VfxContextDetail::Status(*lease.state) != Vfx::VfxSpawnStatus::Accepted)
            return {};
        for (const auto &moduleSlot : lease.state->modules)
            if (moduleSlot.state->grant.moduleId == moduleId)
                return moduleSlot.producer;
        return {};
    }

    Result<std::shared_ptr<const GameplayVfxContext>> GameplayVfxHostAccess::ForkRuntime(
        const std::shared_ptr<const GameplayVfxContext> &moduleContext) {
        if (!moduleContext || !moduleContext->IsUsable() || moduleContext->state_->lifetime->parent ||
            moduleContext->state_->entity.IsValid())
            return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::ScopeInvalid);
        try {
            auto state = std::make_shared<GameplayVfxClientState>(*moduleContext->state_);
            state->lifetime = std::make_shared<GameplayVfxScopeLifetime>();
            state->lifetime->parent = moduleContext->state_->lifetime;
            state->entity = {};
            state->callback = false;
            if (!moduleContext->IsUsable())
                return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::ScopeInvalid);
            return Result<std::shared_ptr<const GameplayVfxContext>>::Success(
                std::shared_ptr<const GameplayVfxContext>{new GameplayVfxContext{std::move(state)}});
        } catch (const std::bad_alloc &) {
            return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::AllocationFailed);
        }
    }

    Result<std::shared_ptr<const GameplayVfxContext>> GameplayVfxHostAccess::ForEntity(
        const std::shared_ptr<const GameplayVfxContext> &moduleContext, const GameplayEntityRef entity) {
        if (!moduleContext || !moduleContext->IsUsable() || !entity.IsValid() || entity.scene != moduleContext->Scene().Value() ||
            moduleContext->state_->entity.IsValid())
            return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::ScopeInvalid);
        try {
            auto state = std::make_shared<GameplayVfxClientState>(*moduleContext->state_);
            state->lifetime = std::make_shared<GameplayVfxScopeLifetime>();
            state->lifetime->parent = moduleContext->state_->lifetime;
            state->entity = entity;
            state->callback = false;
            if (!moduleContext->IsUsable())
                return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::ScopeInvalid);
            return Result<std::shared_ptr<const GameplayVfxContext>>::Success(
                std::shared_ptr<const GameplayVfxContext>{new GameplayVfxContext{std::move(state)}});
        } catch (const std::bad_alloc &) {
            return Reject<std::shared_ptr<const GameplayVfxContext>>(GameplayVfxErrors::AllocationFailed);
        }
    }

    /** @copydoc GameplayVfxHostAccess::RetireEntityClient */
    void GameplayVfxHostAccess::RetireEntityClient(const std::shared_ptr<const GameplayVfxContext> &client) noexcept {
        if (!client || client->state_->moduleState->owner != std::this_thread::get_id() || !client->state_->entity.IsValid())
            return;
        client->state_->retired = true;
        client->state_->callback = false;
    }

    void GameplayVfxHostAccess::RevokeClient(const std::shared_ptr<const GameplayVfxContext> &client) noexcept {
        if (client && client->state_->moduleState->owner == std::this_thread::get_id())
            client->state_->lifetime->revoked = true;
    }

    bool GameplayVfxHostAccess::EnterCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept {
        if (!client)
            return false;
        std::shared_ptr<GameplayVfxSceneState> scene;
        if (VfxContextDetail::ClientStatus(*client->state_, scene) != Vfx::VfxSpawnStatus::Accepted)
            return false;
        if (!scene->sourceOpen || !client->state_->entity.IsValid() || client->state_->callback)
            return false;
        client->state_->callback = true;
        return true;
    }

    void GameplayVfxHostAccess::LeaveCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept {
        if (client && client->state_->moduleState->owner == std::this_thread::get_id())
            client->state_->callback = false;
    }

    Result<std::shared_ptr<const GameplayVfxBindingTable>> GameplayVfxHostAccess::PrepareReload(const GameplayVfxSceneLease &lease,
                                                                                                GameplayVfxBindingDocument document,
                                                                                                Assets::AssetRegistrySnapshot registry,
                                                                                                const Assets::IAssetProvider &provider,
                                                                                                const CancellationToken &cancellation) {
        const auto state = lease.state;
        if (!state || VfxContextDetail::Status(*state) != Vfx::VfxSpawnStatus::Accepted)
            return Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::ScopeInvalid);
        if (!ReloadReady(*state, document.generation))
            return Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::BindingInvalid);
        const ReloadScope scope{*state};
        try {
            if (auto valid = VfxBindingDetail::ValidateDocument(document); valid.HasError())
                return Result<std::shared_ptr<const GameplayVfxBindingTable>>::Failure(valid.ErrorValue());
            auto palette = ReloadPalette(*state->world, document);
            if (palette.HasError())
                return Result<std::shared_ptr<const GameplayVfxBindingTable>>::Failure(palette.ErrorValue());
            auto replacement =
                GameplayVfxBindingTable::Cook(std::move(document), std::move(registry), provider, palette.Value(), cancellation);
            if (!state->active || !state->world)
                return Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::ScopeInvalid);
            return replacement;
        } catch (const std::bad_alloc &) {
            return Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::AllocationFailed);
        }
    }

    Vfx::VfxSpawnStatus GameplayVfxHostAccess::PublishReload(const GameplayVfxSceneLease &lease,
                                                             std::shared_ptr<const GameplayVfxBindingTable> replacement) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (!lease.state)
            return StaleScene;
        auto &state = *lease.state;
        if (const auto status = VfxContextDetail::Status(state); status != Accepted)
            return status;
        if (state.sourceOpen || state.pendingCount != 0 || state.sealCount != 0)
            return Reentrant;
        if (!replacement || replacement->Generation() <= state.table->Generation())
            return StaleBinding;
        if (const auto status = WorldStatus(*replacement, *state.world); status != Accepted)
            return status;
        state.table = std::move(replacement);
        return Accepted;
    }
}  // namespace Horo::Gameplay::Detail
