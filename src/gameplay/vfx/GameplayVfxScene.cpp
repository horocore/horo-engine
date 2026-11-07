#include "Horo/Gameplay/GameplayVfxScene.h"

#include "GameplayVfxHostAccess.h"

#include <new>
#include <thread>
#include <utility>

namespace Horo::Gameplay {
    namespace {
        using Host = Detail::GameplayVfxHostAccess;
        using Lease = Detail::GameplayVfxSceneLease;

        template <typename Value> [[nodiscard]] Result<Value> Invalid() {
            return Result<Value>::Failure(MakeError(GameplayVfxErrors::ScopeInvalid));
        }

        /** @brief Bounds cold root inputs before allocating recipe or receipt storage. */
        bool ValidConfiguration(const GameplayVfxSceneConfiguration &configuration) noexcept {
            return configuration.provider && configuration.originRevision != 0 && !configuration.recipes.empty() &&
                   configuration.recipes.size() <= Vfx::VfxSpawnHardLimits::Effects && configuration.source.maximumPending != 0 &&
                   configuration.source.maximumPending <= Vfx::VfxSpawnHardLimits::Pending;
        }

        /** @brief Copies every supported borrowed kernel input before the application configuration can leave scope. */
        struct OwnedRecipe final {
            explicit OwnedRecipe(Vfx::VfxSpawnAssetPreparation source)
                : value(std::move(source)), forces(value.simulation.forces.begin(), value.simulation.forces.end()),
                  planes(value.simulation.planes.begin(), value.simulation.planes.end()),
                  size(value.simulation.sizeOverLife.begin(), value.simulation.sizeOverLife.end()),
                  opacity(value.simulation.opacityOverLife.begin(), value.simulation.opacityOverLife.end()),
                  color(value.simulation.colorOverLife.begin(), value.simulation.colorOverLife.end()),
                  channels(value.simulation.payloadChannels.begin(), value.simulation.payloadChannels.end()),
                  modules(value.simulation.payloadModules.begin(), value.simulation.payloadModules.end()) {}

            void Bind() noexcept {
                value.simulation.forces = forces;
                value.simulation.planes = planes;
                value.simulation.sizeOverLife = size;
                value.simulation.opacityOverLife = opacity;
                value.simulation.colorOverLife = color;
                value.simulation.payloadChannels = channels;
                value.simulation.payloadModules = modules;
                value.simulation.physicsWorld = {};
                value.simulation.sceneDepth = {};
                value.simulation.stageObserverContext = nullptr;
            }

            Vfx::VfxSpawnAssetPreparation value;
            std::vector<Vfx::CpuParticleForceModule> forces;
            std::vector<Vfx::CpuParticlePlane> planes;
            std::vector<Vfx::CpuParticleCurveKey> size, opacity;
            std::vector<Vfx::CpuParticleColorKey> color;
            std::vector<Vfx::CpuParticlePayloadChannel> channels;
            std::vector<Vfx::CpuParticlePayloadModule> modules;
        };

        [[nodiscard]] bool BoundedRecipe(const Vfx::VfxSpawnAssetPreparation &recipe) noexcept {
            const auto &simulation = recipe.simulation;
            return simulation.forces.size() <= Vfx::CpuParticleSimulationHardLimits::ForceModules &&
                   simulation.planes.size() <= Vfx::CpuParticleSimulationHardLimits::Planes &&
                   simulation.sizeOverLife.size() <= Vfx::CpuParticleSimulationHardLimits::CurveKeys &&
                   simulation.opacityOverLife.size() <= Vfx::CpuParticleSimulationHardLimits::CurveKeys &&
                   simulation.colorOverLife.size() <= Vfx::CpuParticleSimulationHardLimits::CurveKeys &&
                   simulation.payloadChannels.size() <= Vfx::CpuParticleSimulationHardLimits::PayloadChannels &&
                   simulation.payloadModules.size() <= Vfx::CpuParticleSimulationHardLimits::PayloadChannels;
        }

        [[nodiscard]] bool OwnedProfile(const Vfx::VfxSpawnAssetPreparation &recipe) noexcept {
            return recipe.simulation.physicsWorld.probe == nullptr && recipe.simulation.stageObserver == nullptr &&
                   recipe.simulation.sceneDepth.samples.empty();
        }

        /** @brief Service-owned registration delegates to exactly the retained application facade, never another World. */
        class Registration final : public Runtime::SceneActivationParticipant {
        public:
            explicit Registration(std::shared_ptr<GameplayVfxScene> owner) : owner_(std::move(owner)) {}

            Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override {
                return owner_->PrepareOwnedScene(scene);
            }

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                               const Runtime::RuntimeSceneView &scene) override {
                return owner_->Prepare(definition, scene);
            }

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(const Runtime::RuntimeSceneView &scene) override {
                return owner_->PrepareRetirement(scene);
            }

        private:
            std::shared_ptr<GameplayVfxScene> owner_;
        };
    }  // namespace

    struct GameplayVfxScene::State final {
        struct Pending final {
            Runtime::RuntimeSceneView view;
            Vfx::VfxWorld *world{};
        };

        struct Candidate final : Runtime::SceneActivationCandidate {
            Candidate(std::shared_ptr<State> owner, Lease source, Runtime::SceneRuntimeId scene, bool retirement)
                : owner(std::move(owner)), source(std::move(source)), scene(scene), retirement(retirement) {}

            ~Candidate() override {
                Shutdown();
            }

            Result<void> ValidatePublication() const override {
                const auto current = owner.lock();
                if (!current || !current->alive || closed || current->lane != std::this_thread::get_id())
                    return Invalid<void>();
                if (retirement && current->active.state != source.state)
                    return Invalid<void>();
                if (Host::Scene(source).Value() != scene.value || Host::PreparedStatus(source) != Vfx::VfxSpawnStatus::Accepted)
                    return Invalid<void>();
                return Result<void>::Success();
            }

            Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
                return Runtime::SceneCanonicalDatasetProjection::Absent;
            }

            void Publish() noexcept override {
                const auto current = owner.lock();
                if (!current)
                    return;
                if (retirement) {
                    Host::Revoke(source);
                    if (current->active.state == source.state)
                        current->active = {};
                } else {
                    Host::Publish(source);
                    current->active = source;
                }
            }

            void Shutdown() noexcept override {
                if (closed)
                    return;
                closed = true;
                if (retirement)
                    return;
                Host::Revoke(source);
                if (const auto current = owner.lock(); current && current->active.state == source.state)
                    current->active = {};
            }

            std::weak_ptr<State> owner;
            Lease source;
            Runtime::SceneRuntimeId scene;
            bool retirement{};
            bool closed{};
        };

        [[nodiscard]] static bool Matching(const Pending &pending, const Runtime::RuntimeSceneDefinition &definition,
                                           const Runtime::RuntimeSceneView &scene) noexcept {
            return pending.world && pending.view.IsCurrent() && scene.IsCurrent() && pending.view.RuntimeId() == scene.RuntimeId() &&
                   scene.DefinitionId() == definition.Id() && scene.DefinitionRevision() == definition.Revision();
        }

        struct SealedReceipt final {
            Lease source;
            std::shared_ptr<const Runtime::SceneStructuralReceipt> receipt;
            std::uint64_t tick{};
        };

        explicit State(GameplayVfxSceneConfiguration config) : config(std::move(config)) {
            sealed.resize(this->config.source.maximumPending);
        }

        /** @brief Reserves actual receipt storage within the same source byte envelope. */
        [[nodiscard]] GameplayVfxSourceBudget SourceBudget() const noexcept {
            auto budget = config.source;
            const auto receiptBytes = sealed.capacity() * sizeof(SealedReceipt);
            budget.maximumBytes = receiptBytes < budget.maximumBytes ? budget.maximumBytes - receiptBytes : 0;
            return budget;
        }

        void ClearPreparedSeal() noexcept {
            preparedReceipt.reset();
            preparedTick = 0;
        }

        [[nodiscard]] std::vector<Vfx::VfxSpawnAssetPreparation> Recipes() const {
            std::vector<Vfx::VfxSpawnAssetPreparation> output;
            output.reserve(recipes.size());
            for (const auto &recipe : recipes)
                output.push_back(recipe.value);
            return output;
        }

        [[nodiscard]] Result<std::vector<Vfx::VfxSpawnAssetInfo>> Palette(Vfx::VfxWorld &world) const {
            std::vector<Vfx::VfxSpawnAssetInfo> output;
            output.reserve(recipes.size());
            for (const auto &recipe : recipes) {
                Vfx::VfxSpawnAssetInfo info;
                if (world.InspectSpawnAsset(recipe.value.asset, info) != Vfx::VfxSpawnStatus::Accepted)
                    return Invalid<std::vector<Vfx::VfxSpawnAssetInfo>>();
                output.push_back(info);
            }
            return Result<std::vector<Vfx::VfxSpawnAssetInfo>>::Success(std::move(output));
        }

        GameplayVfxSceneConfiguration config;
        std::vector<OwnedRecipe> recipes;
        std::thread::id lane{std::this_thread::get_id()};
        Pending pending;
        Lease active;
        GameplayVfxTickResult last;
        std::vector<SealedReceipt> sealed;
        std::size_t sealHead{};
        std::size_t sealCount{};
        std::shared_ptr<const Runtime::SceneStructuralReceipt> preparedReceipt;
        std::uint64_t preparedTick{};
        bool alive{true};
    };

    GameplayVfxScene::GameplayVfxScene(std::shared_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc GameplayVfxScene::Create */
    Result<std::shared_ptr<GameplayVfxScene>> GameplayVfxScene::Create(GameplayVfxSceneConfiguration configuration) {
        if (!ValidConfiguration(configuration))
            return Invalid<std::shared_ptr<GameplayVfxScene>>();
        try {
            auto state = std::make_shared<State>(std::move(configuration));
            state->recipes.reserve(state->config.recipes.size());
            for (const auto &recipe : state->config.recipes) {
                if (!BoundedRecipe(recipe) || !OwnedProfile(recipe))
                    return Invalid<std::shared_ptr<GameplayVfxScene>>();
                state->recipes.emplace_back(recipe);
            }
            for (auto &recipe : state->recipes)
                recipe.Bind();
            state->config.recipes.clear();
            return Result<std::shared_ptr<GameplayVfxScene>>::Success(
                std::shared_ptr<GameplayVfxScene>{new GameplayVfxScene{std::move(state)}});
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayVfxScene>>::Failure(MakeError(GameplayVfxErrors::AllocationFailed));
        }
    }

    /** @copydoc GameplayVfxScene::CreateSibling */
    Result<std::shared_ptr<GameplayVfxScene>> GameplayVfxScene::CreateSibling() const {
        if (state_->lane != std::this_thread::get_id() || !state_->alive)
            return Invalid<std::shared_ptr<GameplayVfxScene>>();
        try {
            auto configuration = state_->config;
            configuration.recipes = state_->Recipes();
            return Create(std::move(configuration));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayVfxScene>>::Failure(MakeError(GameplayVfxErrors::AllocationFailed));
        }
    }

    /** @copydoc GameplayVfxScene::~GameplayVfxScene */
    GameplayVfxScene::~GameplayVfxScene() {
        state_->alive = false;
        Host::Revoke(state_->active);
    }

    /** @copydoc GameplayVfxScene::MakeActivationParticipant */
    std::unique_ptr<Runtime::SceneActivationParticipant> GameplayVfxScene::MakeActivationParticipant() {
        return std::make_unique<Registration>(shared_from_this());
    }

    /** @copydoc GameplayVfxScene::Scene */
    Vfx::VfxIdentityScope GameplayVfxScene::Scene() const noexcept {
        return Host::Scene(state_->active);
    }

    /** @copydoc GameplayVfxScene::PrepareOwnedScene */
    Result<void> GameplayVfxScene::PrepareOwnedScene(Runtime::RuntimeScene &scene) {
        if (state_->lane != std::this_thread::get_id() || !state_->alive)
            return Invalid<void>();
        auto scope = Vfx::VfxIdentityScope::Create(scene.View().RuntimeId().value);
        if (scope.HasError())
            return Result<void>::Failure(scope.ErrorValue());
        const auto recipes = state_->Recipes();
        auto world = Vfx::VfxWorld::PrepareSpawns(scope.Value(), recipes, state_->config.spawn, state_->config.extraction);
        if (world.HasError())
            return Result<void>::Failure(world.ErrorValue());
        if (auto attached = scene.AttachVfxWorld(std::move(world).Value(), state_->config.origin, state_->config.originRevision);
            attached.HasError())
            return attached;
        state_->pending = {scene.View(), scene.VfxWorld()};
        return Result<void>::Success();
    }

    /** @copydoc GameplayVfxScene::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> GameplayVfxScene::Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         const Runtime::RuntimeSceneView &scene) {
        auto pending = std::exchange(state_->pending, {});
        if (!State::Matching(pending, definition, scene))
            return Invalid<std::unique_ptr<Runtime::SceneActivationCandidate>>();
        auto palette = state_->Palette(*pending.world);
        if (palette.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(palette.ErrorValue());
        auto table =
            GameplayVfxBindingTable::Cook(state_->config.bindings, state_->config.registry, *state_->config.provider, palette.Value());
        if (table.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(table.ErrorValue());
        if (!scene.IsCurrent())
            return Invalid<std::unique_ptr<Runtime::SceneActivationCandidate>>();
        auto source = Host::Prepare(*pending.world, std::move(table).Value(), state_->config.grants, state_->SourceBudget(),
                                    state_->config.spawn.maximumDeltaSeconds);
        if (source.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(source.ErrorValue());
        return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
            std::make_unique<State::Candidate>(state_, std::move(source).Value(), scene.RuntimeId(), false));
    }

    /** @copydoc GameplayVfxScene::PrepareRetirement */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> GameplayVfxScene::PrepareRetirement(const Runtime::RuntimeSceneView &scene) {
        if (!scene.IsCurrent() || Scene().Value() != scene.RuntimeId().value)
            return Invalid<std::unique_ptr<Runtime::SceneActivationCandidate>>();
        return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
            std::make_unique<State::Candidate>(state_, state_->active, scene.RuntimeId(), true));
    }

    /** @copydoc GameplayVfxScene::Module */
    std::shared_ptr<const GameplayVfxContext> GameplayVfxScene::Module(const std::string_view moduleId) const noexcept {
        return Host::Module(state_->active, moduleId);
    }

    /** @copydoc GameplayVfxScene::CommittedProducer */
    std::shared_ptr<const GameplayVfxCommittedProducer> GameplayVfxScene::CommittedProducer(
        const std::string_view moduleId) const noexcept {
        return Host::CommittedProducer(state_->active, moduleId);
    }

    /** @copydoc GameplayVfxScene::BeginTick */
    Vfx::VfxSpawnStatus GameplayVfxScene::BeginTick(const std::uint64_t tick, const float deltaSeconds) noexcept {
        const auto status = Host::BeginTick(state_->active, tick, deltaSeconds);
        if (status != Vfx::VfxSpawnStatus::Accepted)
            state_->last = {.status = status};
        return status;
    }

    /** @copydoc GameplayVfxScene::CommitTick */
    GameplayVfxTickResult GameplayVfxScene::CommitTick(const std::uint64_t tick) noexcept {
        state_->last = Host::CommitTick(state_->active, tick);
        return state_->last;
    }

    /** @copydoc GameplayVfxScene::AbortTick */
    void GameplayVfxScene::AbortTick(const std::uint64_t tick) noexcept {
        Host::AbortTick(state_->active, tick);
        if (state_->preparedTick == tick)
            state_->ClearPreparedSeal();
    }

    /** @copydoc GameplayVfxScene::PrepareSealTick */
    Vfx::VfxSpawnStatus GameplayVfxScene::PrepareSealTick(const std::uint64_t tick,
                                                          std::shared_ptr<const Runtime::SceneStructuralReceipt> receipt) noexcept {
        if (state_->lane != std::this_thread::get_id())
            return Vfx::VfxSpawnStatus::ThreadViolation;
        if (state_->sealCount == state_->sealed.size())
            return Vfx::VfxSpawnStatus::EventQueueFull;
        const auto status = Host::PrepareSealTick(state_->active, tick);
        if (status == Vfx::VfxSpawnStatus::Accepted) {
            state_->preparedReceipt = std::move(receipt);
            state_->preparedTick = tick;
        }
        return status;
    }

    /** @copydoc GameplayVfxScene::SealTick */
    void GameplayVfxScene::SealTick(const std::uint64_t tick) noexcept {
        if (state_->lane != std::this_thread::get_id() || state_->preparedTick != tick || state_->sealCount == state_->sealed.size())
            std::terminate();
        Host::SealTick(state_->active, tick);
        const auto index = (state_->sealHead + state_->sealCount) % state_->sealed.size();
        state_->sealed[index] = {state_->active, std::move(state_->preparedReceipt), tick};
        ++state_->sealCount;
        state_->ClearPreparedSeal();
    }

    /** @copydoc GameplayVfxScene::FlushSealedTicks */
    GameplayVfxTickResult GameplayVfxScene::FlushSealedTicks() noexcept {
        if (state_->lane != std::this_thread::get_id())
            return {.status = Vfx::VfxSpawnStatus::ThreadViolation};
        while (state_->sealCount != 0) {
            auto &sealed = state_->sealed[state_->sealHead];
            if (sealed.receipt && !sealed.receipt->Complete())
                break;
            const bool committed = !sealed.receipt || sealed.receipt->ResultValue() != nullptr;
            state_->last = Host::CommitSealedTick(sealed.source, sealed.tick, committed);
            sealed = {};
            state_->sealHead = (state_->sealHead + 1) % state_->sealed.size();
            --state_->sealCount;
        }
        return state_->last;
    }

    /** @copydoc GameplayVfxScene::LastTick */
    GameplayVfxTickResult GameplayVfxScene::LastTick() const noexcept {
        return state_->last;
    }

    /** @copydoc GameplayVfxScene::Reload */
    Result<void> GameplayVfxScene::Reload(GameplayVfxBindingDocument document, Assets::AssetRegistrySnapshot registry,
                                          const Assets::IAssetProvider &provider, const CancellationToken &cancellation) {
        const auto active = state_->active;
        auto prepared = Host::PrepareReload(active, std::move(document), std::move(registry), provider, cancellation);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (active.state != state_->active.state)
            return Invalid<void>();
        if (Host::PublishReload(active, std::move(prepared).Value()) != Vfx::VfxSpawnStatus::Accepted)
            return Invalid<void>();
        return Result<void>::Success();
    }
}  // namespace Horo::Gameplay
