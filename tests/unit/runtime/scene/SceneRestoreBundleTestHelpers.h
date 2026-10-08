#pragma once

#include "../save/SaveGameplayPersistenceTestUtils.h"
#include "AllocationProbe.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Horo::Runtime::RestoreTest {
    using Test::Id;
    using namespace Test::GameplayPersistence;

    inline Gameplay::ComponentTypeId ComponentType() {
        return Gameplay::ComponentTypeId::Parse("game.tests.links").Value();
    }

    inline RuntimeSceneDefinition Definition(const std::uint64_t revision = 1, const std::uint8_t count = 2) {
        SceneDefinitionBuilder builder{SceneDefinitionId{7}, SceneDefinitionRevision{revision}};
        for (std::uint64_t object = 1; object <= count; ++object) {
            RuntimeEntityDefinition entity;
            entity.object = SceneObjectId{object};
            entity.components.gameplayComponents.push_back(
                {ComponentType(), 1, Gameplay::ComponentPayloadEncoding::CanonicalJson, {std::byte{'{'}, std::byte{'}'}}});
            builder.Add(std::move(entity));
        }
        return std::move(builder).Build().Value();
    }

    inline PersistentEntityRecord Entity(const std::uint8_t identity) {
        return {.identity = Id<PersistentEntityId>(identity),
                .generation = {1},
                .provenance = AuthoredPersistentEntityProvenance{SceneDefinitionId{7}, SceneObjectId{identity}},
                .disposition = PersistentEntityDisposition::Live};
    }

    inline RestoreEntityReference Target(const std::uint8_t identity, const std::uint64_t generation = 1) {
        return {{Id<SaveWorldId>(1), Id<PersistentEntityId>(identity)}, {generation}};
    }

    /** @brief Foreign project failure deliberately outside the standard exception hierarchy. */
    struct ForeignProjectFixupFailure final {};

    inline void InjectForeignBoundaryFault(const unsigned fault) {
        if (fault == 1)
            throw std::bad_alloc{};
        if (fault == 2)
            throw ForeignProjectFixupFailure{};
        if (fault == 3)
            throw std::runtime_error{"foreign boundary fixture fault"};
    }

    struct ProjectState final {
        std::vector<SaveRestoreReferenceResult> active;
        std::uint64_t candidateScene{};
        unsigned prepared{};
        unsigned fixed{};
        unsigned published{};
        bool throwFixup{};
        bool throwAllocation{};
        bool throwNonStandard{};
        bool failFailureTranslationAllocation{};
        std::optional<Tests::AllocationProbe::ScopedFailure> failureTranslation;
        std::size_t allocationCountAtFault{};
    };

    class ProjectCandidate final : public IPreparedGameplayPersistenceState {
    public:
        explicit ProjectCandidate(ProjectState &state) : state_(state) {}

        Result<void> FixupRuntimeReferences(const SaveRestoreReferenceView &references) override {
            if (state_.throwFixup) {
                if (state_.failFailureTranslationAllocation) {
                    state_.failureTranslation.emplace(0);
                    state_.allocationCountAtFault = Tests::AllocationProbe::Count();
                }
                if (state_.throwAllocation)
                    throw std::bad_alloc{};
                if (state_.throwNonStandard)
                    throw ForeignProjectFixupFailure{};
                throw std::runtime_error{"project fixup failure"};
            }
            ++state_.fixed;
            candidate_.assign(references.references.begin(), references.references.end());
            generation_ = references.generation.candidateScene;
            return Result<void>::Success();
        }

        void Publish() noexcept override {
            state_.active.swap(candidate_);
            state_.candidateScene = generation_;
            ++state_.published;
        }

    private:
        ProjectState &state_;
        std::vector<SaveRestoreReferenceResult> candidate_;
        std::uint64_t generation_{};
    };

    class ProjectSource final : public IGameplayPersistenceSource {
    public:
        ProjectState state;
        bool legacy{};

        Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t) const override {
            return Result<std::vector<std::byte>>::Success({std::byte{0x42}});
        }

        Result<std::unique_ptr<IPreparedGameplayPersistenceState>> PrepareRuntimeState(std::span<const std::byte>) override {
            ++state.prepared;
            if (legacy) {
                class LegacyCandidate final : public IPreparedGameplayPersistenceState {
                public:
                    explicit LegacyCandidate(ProjectState &state) : state_(state) {}

                    void Publish() noexcept override {
                        ++state_.published;
                    }

                private:
                    ProjectState &state_;
                };

                return Result<std::unique_ptr<IPreparedGameplayPersistenceState>>::Success(std::make_unique<LegacyCandidate>(state));
            }
            return Result<std::unique_ptr<IPreparedGameplayPersistenceState>>::Success(std::make_unique<ProjectCandidate>(state));
        }
    };

    struct ComponentState final {
        unsigned prepareFault{};
        EntityRef entity;
        PersistentEntityId linked;
        std::uint32_t value{};
        unsigned fixed{};
        unsigned published{};
    };

    class ComponentCandidate final : public IPreparedSaveableComponentState {
    public:
        ComponentCandidate(ComponentState &active, const EntityRef entity, const std::uint32_t value)
            : active_(active), entity_(entity), value_(value) {}

        Result<void> FixupReferences(const SaveRestoreReferenceView &references) override {
            const auto *result = references.Find({3});
            if (!result || result->disposition != SaveRestoreReferenceDisposition::Resolved ||
                !std::holds_alternative<SaveRestoreEntityTarget>(result->target))
                return Result<void>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            linked_ = std::get<SaveRestoreEntityTarget>(result->target).identity;
            ++active_.fixed;
            return Result<void>::Success();
        }

        void PublishPrepared() noexcept override {
            active_.entity = entity_;
            active_.linked = linked_;
            active_.value = value_;
            ++active_.published;
        }

    private:
        ComponentState &active_;
        EntityRef entity_;
        PersistentEntityId linked_;
        std::uint32_t value_{};
    };

    class ComponentAdapter final : public ISaveableComponentStateAdapter {
    public:
        explicit ComponentAdapter(ComponentState &state) : state_(state) {}

        const SaveableComponentAdapterDescriptor &Descriptor() const noexcept override {
            return descriptor_;
        }

        Result<std::optional<CanonicalEncodedValue>> Capture(EntityRef) const override {
            return Result<std::optional<CanonicalEncodedValue>>::Success(std::nullopt);
        }

        Result<CanonicalEncodedValue> Migrate(std::uint32_t, CanonicalValueReader) const override {
            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
        }

        Result<void> Validate(CanonicalValueReader source) const override {
            const auto value = source.ReadUInt32();
            return value.HasValue() ? Result<void>::Success() : Result<void>::Failure(value.ErrorValue());
        }

        Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareApply(EntityRef entity,
                                                                              CanonicalValueReader source) const override {
            InjectForeignBoundaryFault(state_.prepareFault);
            const auto value = source.ReadUInt32();
            if (value.HasError())
                return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(value.ErrorValue());
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(
                std::make_unique<ComponentCandidate>(state_, entity, value.Value()));
        }

        Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareDefault(EntityRef entity) const override {
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(
                std::make_unique<ComponentCandidate>(state_, entity, 0));
        }

    private:
        ComponentState &state_;
        SaveableComponentAdapterDescriptor descriptor_{ComponentType(), 1, 64};
    };

    class Authority final : public ISceneRestoreGenerationAuthority {
    public:
        StagedRestoreActivationEvidence evidence;

        StagedRestoreActivationEvidence Current() const noexcept override {
            return evidence;
        }
    };

    struct PreparationControl final {
        unsigned sourceFault{};
        bool ready{};
        unsigned cancelled{};
    };

    struct SceneParticipantControl final {
        bool reject{};
        unsigned published{};
    };

    class SceneCandidate final : public SceneActivationCandidate {
    public:
        explicit SceneCandidate(std::shared_ptr<SceneParticipantControl> control) : control_(std::move(control)) {}

        Result<void> ValidatePublication() const override {
            return control_->reject ? Result<void>::Failure(MakeError(SaveErrors::CompositionInjectedFailure)) : Result<void>::Success();
        }

        void Publish() noexcept override {
            ++control_->published;
        }

        void Shutdown() noexcept override {}

    private:
        std::shared_ptr<SceneParticipantControl> control_;
    };

    class SceneParticipant final : public SceneActivationParticipant {
    public:
        explicit SceneParticipant(std::shared_ptr<SceneParticipantControl> control) : control_(std::move(control)) {}

        Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView) override {
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<SceneCandidate>(control_));
        }

    private:
        std::shared_ptr<SceneParticipantControl> control_;
    };

    /** @brief Uses the production gameplay adapter's canonical captured bytes and real detached restore receipt. */
    class OwnerSource final : public ISceneRestoreOwnerSource {
    public:
        OwnerSource(std::shared_ptr<GameplayPersistenceAdapter> adapter, RuntimeSaveSnapshot snapshot, SaveOperationController operation,
                    std::shared_ptr<PreparationControl> control)
            : OwnerSource(std::vector{std::move(adapter)}, std::move(snapshot), std::move(operation), std::move(control)) {}

        OwnerSource(std::vector<std::shared_ptr<GameplayPersistenceAdapter>> adapters, RuntimeSaveSnapshot snapshot,
                    SaveOperationController operation, std::shared_ptr<PreparationControl> control)
            : adapters_(std::move(adapters)), snapshot_(std::move(snapshot)), operation_(std::move(operation)),
              control_(std::move(control)) {}

        Result<std::optional<SceneRestorePreparedOwners>> TakeReady(RuntimeSceneView) override {
            InjectForeignBoundaryFault(control_->sourceFault);
            if (operation_->ObserveCancellation() == SaveCancellationObservation::Cancelled) {
                const auto snapshot = operation_->Handle().Snapshot();
                return Result<std::optional<SceneRestorePreparedOwners>>::Failure(
                    snapshot->terminalError.value_or(MakeError(SaveErrors::OperationCancelled)));
            }
            if (!control_->ready)
                return Result<std::optional<SceneRestorePreparedOwners>>::Success(std::nullopt);
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
            for (const auto &adapter : adapters_) {
                const auto &descriptor = adapter->Descriptor();
                const auto records = snapshot_.Records();
                const auto record = std::find_if(records.begin(), records.end(), [&](const auto &candidate) {
                    return candidate.Record().participant == descriptor.participant.participant &&
                           candidate.Record().record == descriptor.record;
                });
                REQUIRE(record != records.end());
                auto receipt = adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, record->Segment(0));
                if (receipt.HasError())
                    return Result<std::optional<SceneRestorePreparedOwners>>::Failure(receipt.ErrorValue());
                receipts.push_back(std::move(receipt).Value());
            }
            SceneRestorePreparedOwners owners{std::move(*operation_), std::move(receipts)};
            operation_.reset();
            return Result<std::optional<SceneRestorePreparedOwners>>::Success(std::move(owners));
        }

        void Cancel() noexcept override {
            ++control_->cancelled;
            if (operation_)
                static_cast<void>(operation_->RequestShutdownCancellation());
            operation_.reset();
        }

    private:
        std::vector<std::shared_ptr<GameplayPersistenceAdapter>> adapters_;
        RuntimeSaveSnapshot snapshot_;
        std::optional<SaveOperationController> operation_;
        std::shared_ptr<PreparationControl> control_;
    };

    struct Fixture final {
        std::shared_ptr<ProjectSource> project{std::make_shared<ProjectSource>()};
        std::shared_ptr<GameplayPersistenceAdapter> adapter;
        SaveParticipantRegistrySnapshot participants;
        std::shared_ptr<Authority> authority{std::make_shared<Authority>()};
        std::shared_ptr<PreparationControl> control{std::make_shared<PreparationControl>()};
        std::optional<RuntimeSaveSnapshot> snapshot;
        std::uint8_t entityCount{2};
        CancellationSource cancellation;
        RuntimeSceneService service;
        SaveOperationHandle operation;

        explicit Fixture(std::unique_ptr<SceneActivationParticipant> ordinary = {}, const std::uint8_t count = 2) : entityCount(count) {
            adapter = GameplayPersistenceAdapter::Create(Descriptor(GameplayPersistenceOwner::Service), project, std::make_shared<int>(1))
                          .Value();
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(adapter->Descriptor().participant, adapter).HasValue());
            participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            REQUIRE(builder.CaptureParticipants().HasValue());
            snapshot = builder.Seal().Value();
            if (ordinary)
                REQUIRE(service.AddActivationParticipant(std::move(ordinary)).HasValue());
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(service.QueuePreparation(Definition(1, entityCount)).HasValue());
            Pump();
            authority->evidence = {participants.Generation(), 17, service.ActiveScene()->RuntimeId().value};
        }

        void Pump() {
            const FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        }

        SceneRestoreBundleRequest Request() const {
            SceneRestoreBundleRequest request;
            request.context = {.operation = 1437,
                               .registryGeneration = participants.Generation(),
                               .sessionGeneration = 17,
                               .sceneIncarnation = service.ActiveScene()->RuntimeId().value,
                               .maximumParticipants = 1};
            request.world = Id<SaveWorldId>(1);
            request.definition = SceneDefinitionId{7};
            request.revision = SceneDefinitionRevision{2};
            request.participants = participants;
            request.entities = {Entity(1), Entity(2)};
            request.nodes = {Target(2), Target(1)};
            const auto owner = adapter->Descriptor().participant.participant;
            request.references = {{.identity = 2, .owner = owner, .source = Target(2), .target = Target(1)},
                                  {.identity = 1, .owner = owner, .source = Target(1), .target = Target(2)}};
            Math::Transform transform;
            transform.translation.x = 42.0F;
            request.transforms = {{Id<PersistentEntityId>(1), {1}, transform}};
            return request;
        }

        SceneRestoreBundle *Queue(SceneRestoreBundleRequest request) {
            auto producer =
                CreateSaveOperation({.operation = 1437, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
            operation = producer.Handle();
            auto source = std::make_unique<OwnerSource>(adapter, *snapshot, std::move(producer), control);
            auto bundle = SceneRestoreBundle::Create(std::move(request), std::move(source), authority);
            REQUIRE(bundle.HasValue());
            auto *borrowed = bundle.Value().get();
            REQUIRE(service.QueuePreparationWithRestore(Definition(2, entityCount), std::move(bundle).Value()).HasValue());
            return borrowed;
        }
    };

    struct GraphFixture final {
        Fixture fixture;
        std::optional<PersistentEntityIdentityMap> identities;
        std::array<SaveParticipantId, 1> owners;

        GraphFixture() : owners{fixture.adapter->Descriptor().participant.participant} {
            const auto scene = *fixture.service.ActiveScene();
            const std::array records{Entity(1), Entity(2)};
            const std::array bindings{PersistentEntityRuntimeBinding{Id<PersistentEntityId>(1), {1}, *scene.Find(SceneObjectId{1})},
                                      PersistentEntityRuntimeBinding{Id<PersistentEntityId>(2), {1}, *scene.Find(SceneObjectId{2})}};
            identities = PersistentEntityIdentityMap::Create(scene.RuntimeId(), records, bindings).Value();
        }

        RestoreReferenceAuthorities Authorities() const {
            return {.world = Id<SaveWorldId>(1),
                    .scene = *fixture.service.ActiveScene(),
                    .identities = &*identities,
                    .participants = fixture.participants,
                    .preparedOwners = owners};
        }
    };

    class ProjectService final : public Gameplay::IGameplayService {
    public:
        Result<void> Start(const Gameplay::GameplayServiceContext &) override {
            return Result<void>::Success();
        }

        void Stop(const Gameplay::GameplayServiceContext &) noexcept override {}
    };

    struct ServiceOwner final {
        Gameplay::GameServiceRegistry registry{"game.tests"};
        std::unique_ptr<Gameplay::GameplayServiceRuntime> runtime;

        ServiceOwner() {
            Gameplay::GameplayServiceRegistration
                registration{.descriptor = {.id = Gameplay::GameplayServiceId::Parse("game.tests.references").Value(),
                                            .scope = Gameplay::GameplayServiceScope::Project,
                                            .affinity = Gameplay::GameplayThreadAffinity::RuntimeOwner,
                                            .sceneReplacement = Gameplay::GameplaySceneReplacementPolicy::Preserve,
                                            .observabilityCategory = "game.tests.services"},
                             .factory = {.create = [](void *) -> Gameplay::IGameplayService * {
                return new ProjectService;
            }, .destroy = [](void *, Gameplay::IGameplayService *service) noexcept {
                delete service;
            }}};
            REQUIRE(registry.Register(std::move(registration)).HasValue());
            REQUIRE(registry.Freeze().HasValue());
            runtime = Gameplay::GameplayServiceRuntime::Create(registry, Gameplay::GameplayServiceScope::Project).Value();
        }
    };

    struct TwoOwnerFixture final {
        Fixture fixture;
        std::shared_ptr<ProjectSource> secondProject;
        std::shared_ptr<GameplayPersistenceAdapter> second;
        SaveParticipantRegistrySnapshot participants;
        std::optional<RuntimeSaveSnapshot> snapshot;

        TwoOwnerFixture() {
            secondProject = std::make_shared<ProjectSource>();
            auto descriptor = Descriptor(GameplayPersistenceOwner::Service, 2);
            descriptor.participant.participant = SaveParticipantId::Parse("project.gameplay.second").Value();
            second = GameplayPersistenceAdapter::Create(descriptor, secondProject, std::make_shared<int>(2)).Value();
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(fixture.adapter->Descriptor().participant, fixture.adapter).HasValue());
            REQUIRE(registry.Register(second->Descriptor().participant, second).HasValue());
            participants = registry.Snapshot().Value();
            auto capture = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            REQUIRE(capture.CaptureParticipants().HasValue());
            snapshot = capture.Seal().Value();
        }

        std::unique_ptr<SceneRestoreBundle> Prepare(const bool duplicateOwner, const bool reversed) {
            const auto firstOwner = fixture.adapter->Descriptor().participant.participant;
            const auto secondOwner = second->Descriptor().participant.participant;
            auto request = fixture.Request();
            request.participants = participants;
            request.context.registryGeneration = participants.Generation();
            request.context.maximumParticipants = 2;
            fixture.authority->evidence.registryGeneration = participants.Generation();
            request.references =
                {{.identity = 1, .owner = firstOwner, .source = Target(1), .target = Target(2)},
                 {.identity = 1, .owner = duplicateOwner ? firstOwner : secondOwner, .source = Target(2), .target = Target(1)}};
            std::vector adapters{fixture.adapter, second};
            if (reversed) {
                std::reverse(request.references.begin(), request.references.end());
                std::reverse(request.nodes.begin(), request.nodes.end());
                std::reverse(adapters.begin(), adapters.end());
            }
            auto admitted = CreateSaveOperation({.operation = 1437, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4});
            REQUIRE(admitted.HasValue());
            auto producer = std::move(admitted).Value();
            fixture.operation = producer.Handle();
            auto source = std::make_unique<OwnerSource>(std::move(adapters), *snapshot, std::move(producer), fixture.control);
            auto bundle = SceneRestoreBundle::Create(std::move(request), std::move(source), fixture.authority);
            REQUIRE(bundle.HasValue());
            return std::move(bundle).Value();
        }
    };

    inline void PrepareCookedScene(RuntimeSceneService &service, const CancellationToken &cancellation, const Assets::AssetId asset,
                                   const Assets::AssetTypeId &type) {
        REQUIRE(service.Startup(cancellation).HasValue());
        SceneDefinitionBuilder builder{SceneDefinitionId{7}, SceneDefinitionRevision{1}};
        RuntimeEntityDefinition entity;
        entity.object = SceneObjectId{1};
        builder.Add(std::move(entity));
        REQUIRE(builder.RequireAsset({asset, type}).HasValue());
        REQUIRE(service.QueuePreparation(std::move(builder).Build().Value()).HasValue());
        const FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation};
        for (unsigned attempt = 0; !service.ActiveScene() && attempt < 10'000; ++attempt) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            std::this_thread::yield();
        }
        REQUIRE(service.ActiveScene());
        REQUIRE_FALSE(service.TakeOperationError());
    }
}  // namespace Horo::Runtime::RestoreTest
