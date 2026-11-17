#pragma once
#include "GameModuleHostTestHelpers.h"
#include "GameplayBehaviorRestoreFixture.h"
#include "GameplayRetirementTestSupport.h"
#include "Horo/Gameplay/BehaviorRuntimeRestore.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

namespace Horo::Gameplay::RestoreTest {
    using namespace Runtime;

    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Accept(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
    }

    template <class Id> Id IdValue(std::uint8_t value) {
        SaveIdentityDetail::Bytes bytes{};
        bytes.back() = value;
        return Take(Id::FromBytes(bytes));
    }

    inline BehaviorTypeId Type() {
        return Take(BehaviorTypeId::Parse("game.tests.dynamic_mover"));
    }

    inline RuntimeSceneDefinition Definition(std::uint64_t revision = 1, std::uint64_t count = 2, bool duplicateIds = false) {
        SceneDefinitionBuilder builder{{3}, {revision}};
        for (std::uint64_t i = 1; i <= count; ++i) {
            RuntimeComponentSet components;
            components.behaviors.emplace_back(BehaviorInstanceId{duplicateIds ? 1 : i}, Type(), 1, !duplicateIds || i != 2,
                                              std::vector<BehaviorField>{});
            builder.Add({{i}, std::nullopt, {}, std::nullopt, std::move(components)});
        }
        return Take(std::move(builder).Build());
    }

    inline std::vector<PersistentEntityRecord> Records(std::uint64_t count = 2) {
        std::vector<PersistentEntityRecord> result;
        for (std::uint8_t i = 1; i <= count; ++i)
            result.push_back(
                {IdValue<PersistentEntityId>(i), {1}, AuthoredPersistentEntityProvenance{{3}, {i}}, PersistentEntityDisposition::Live});
        return result;
    }

    inline PersistentEntityIdentityMap Map(RuntimeSceneView scene) {
        const auto records = Records(scene.SlotCount());
        std::vector<PersistentEntityRuntimeBinding> bindings;
        for (const auto &record : records) {
            const auto object = std::get<AuthoredPersistentEntityProvenance>(record.provenance).object;
            const auto entity = scene.Find(object);
            REQUIRE(entity);
            bindings.push_back({record.identity, record.generation, *entity});
        }
        return Take(PersistentEntityIdentityMap::Create(scene.RuntimeId(), records, bindings));
    }

    class Fields final : public IBehaviorPersistenceVisitor {
    public:
        bool write{};
        bool peer{};
        std::uint64_t value{42};
        std::vector<Tests::GameplayBehaviorDurableFields> actual;

        Result<void> Visit(std::span<const BehaviorRestoreInstance> instances) override {
            for (const auto &instance : instances) {
                auto *state = dynamic_cast<Tests::IGameplayBehaviorDurableFixture *>(instance.implementation);
                REQUIRE(state);
                if (write) {
                    Tests::GameplayBehaviorDurableFields fields{value + instance.persistentEntity.Bytes().back(), {}, {}};
                    if (peer && instance.persistentEntity == IdValue<PersistentEntityId>(1))
                        fields.peer = BehaviorEntityRestoreTarget{{IdValue<SaveWorldId>(1), IdValue<PersistentEntityId>(2)}, 1};
                    state->SetDurableFields(std::move(fields));
                }
                actual.push_back(state->DurableFields());
            }
            return Result<void>::Success();
        }
    };

    class Authority final : public ISceneRestoreGenerationAuthority {
    public:
        StagedRestoreActivationEvidence current;

        StagedRestoreActivationEvidence Current() const noexcept override {
            return current;
        }
    };

    class Source final : public ISceneRestoreOwnerSource {
    public:
        std::shared_ptr<BehaviorRuntimeSceneOwner> owner;
        std::shared_ptr<const GameplayPersistenceAdapter> behavior;
        std::vector<std::shared_ptr<const GameplayPersistenceAdapter>> peers;
        std::shared_ptr<BehaviorLifecyclePersistenceAdapter> lifecycle;
        std::optional<RuntimeSaveSnapshot> snapshot;
        std::optional<SaveOperationController> producer;
        const PersistentEntityIdentityMap *identities{};

        Result<std::optional<SceneRestorePreparedOwners>> TakeReady(RuntimeSceneView scene) override {
            using Outcome = Result<std::optional<SceneRestorePreparedOwners>>;
            if (!producer || !snapshot || !identities)
                return Outcome::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
            if (producer->ObserveCancellation() == SaveCancellationObservation::Cancelled)
                return Outcome::Failure(MakeError(SaveErrors::OperationCancelled));
            auto candidate = owner->Prepared(scene.RuntimeId());
            if (!candidate)
                return Outcome::Failure(MakeError(SaveErrors::RestoreActivationStale));
            auto target = candidate->Binding(Type(), *identities);
            if (target.HasError())
                return Outcome::Failure(target.ErrorValue());
            auto installation = behavior->BindRestoreCandidate(std::move(target).Value());
            if (installation.HasError())
                return Outcome::Failure(installation.ErrorValue());
            const auto bound = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(installation.Value().AcquireAdapter());
            if (!bound)
                return Outcome::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
            auto receipts = Stage(*bound, candidate);
            if (receipts.HasError())
                return Outcome::Failure(receipts.ErrorValue());
            SceneRestorePreparedOwners ready{std::move(*producer), std::move(receipts).Value()};
            producer.reset();
            snapshot.reset();
            behavior.reset();
            peers.clear();
            lifecycle.reset();
            owner.reset();
            return Outcome::Success(std::move(ready));
        }

        void Cancel() noexcept override {
            if (producer)
                static_cast<void>(producer->RequestShutdownCancellation());
            producer.reset();
            snapshot.reset();
        }

    private:
        Result<std::unique_ptr<IStagedRestoreParticipant>> StageOne(const GameplayPersistenceAdapter &bound,
                                                                    const std::shared_ptr<BehaviorRuntimeRestoreCandidate> &candidate,
                                                                    const OwnedCanonicalSnapshot &record) {
            const auto id = record.Record().participant;
            if (id == BehaviorLifecycleParticipant().participant)
                return lifecycle->StageRestore(ParticipantSchemaVersion::Create(1).Value(), record.Record().record, record.Segment(0),
                                               candidate, *identities);
            const GameplayPersistenceAdapter *adapter = &bound;
            if (id != bound.Descriptor().participant.participant) {
                const auto found = std::ranges::find_if(peers, [&](const auto &peer) {
                    return peer->Descriptor().participant.participant == id;
                });
                if (found == peers.end())
                    return Result<std::unique_ptr<IStagedRestoreParticipant>>::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
                adapter = found->get();
            }
            return adapter->StageRestore(adapter->Descriptor().participant.schemaVersion, record.Record().record, record.Segment(0),
                                         GameplayPersistenceRestorePolicy::ReloadableScene);
        }

        Result<std::vector<std::unique_ptr<IStagedRestoreParticipant>>> Stage(
            const GameplayPersistenceAdapter &bound, const std::shared_ptr<BehaviorRuntimeRestoreCandidate> &candidate) {
            using Outcome = Result<std::vector<std::unique_ptr<IStagedRestoreParticipant>>>;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> result;
            for (const auto &record : snapshot->Records()) {
                auto receipt = StageOne(bound, candidate, record);
                if (receipt.HasError())
                    return Outcome::Failure(receipt.ErrorValue());
                result.push_back(std::move(receipt).Value());
            }
            return Outcome::Success(std::move(result));
        }
    };

    class Observed final : public SceneAggregateRestore, public ISceneRestoreTransientRetirement {
    public:
        Observed(std::unique_ptr<SceneRestoreBundle> bundle, std::shared_ptr<BehaviorRuntimeSceneOwner> owner, Source &source)
            : bundle_(std::move(bundle)), owner_(std::move(owner)), source_(source) {}

        Result<void> PrepareScene(RuntimeScene &scene) override {
            if (auto prepared = bundle_->PrepareScene(scene); prepared.HasError())
                return prepared;
            candidate_ = scene.View().RuntimeId();
            source_.identities = bundle_->Identities();
            auto candidate = owner_->PrepareRestoreCandidate(scene);
            return candidate.HasValue() ? Result<void>::Success() : Result<void>::Failure(candidate.ErrorValue());
        }

        Result<bool> PrepareOwners() override {
            return bundle_->PrepareOwners();
        }

        Result<void> ValidatePublication(const RuntimeSceneView *active) const override {
            return bundle_->ValidatePublication(active);
        }

        Result<void> Commit(IStagedRestoreAggregatePublication &transfer) override {
            return bundle_->Commit(transfer);
        }

        Result<bool> RetirePublishedTransient() override {
            return bundle_->RetirePublishedTransient();
        }

        void Rollback() noexcept override {
            bundle_->Rollback();
            owner_->CancelPrepared(candidate_);
        }

    private:
        std::unique_ptr<SceneRestoreBundle> bundle_;
        std::shared_ptr<BehaviorRuntimeSceneOwner> owner_;
        Source &source_;
        SceneRuntimeId candidate_;
    };

    struct Fixture final {
        Tests::NativeGameplayFailureControl native{0};
        GameModuleHost host;
        std::unique_ptr<LoadedGameModule> module;
        RuntimeSceneService scenes;
        CancellationSource cancellation;
        std::shared_ptr<BehaviorRuntimeSceneOwner> owner;
        std::optional<GameplayPersistenceInstallation> installation;
        std::shared_ptr<const GameplayPersistenceAdapter> behavior;
        std::vector<GameplayPersistenceInstallation> peerInstallations;
        std::vector<std::shared_ptr<const GameplayPersistenceAdapter>> peers;
        std::shared_ptr<BehaviorLifecyclePersistenceAdapter> lifecycle;
        std::unique_ptr<CanonicalStateParticipantRegistry> registry;
        SaveParticipantRegistrySnapshot participants;
        std::shared_ptr<Authority> authority{std::make_shared<Authority>()};
        SaveOperationHandle operation;

        bool duplicateIds{};

        explicit Fixture(bool duplicates = false) : duplicateIds(duplicates) {
            module = Take(host.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
            owner = Take(BehaviorRuntimeSceneOwner::Create(scenes, module->Registry()));
            Accept(scenes.AddActivationParticipant(owner->MakeActivationParticipant()));
            Accept(scenes.Startup(cancellation.Token()));
            Ordinary(1, 2);
            installation.emplace(Take(module->AcquireInstalledPersistence(Take(SaveParticipantId::Parse("game.tests.durable0")))));
            behavior = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(installation->AcquireAdapter());
            REQUIRE(behavior);
            auto binding = Take(owner->BindingForActive(Type(), Map(*scenes.ActiveScene())));
            auto bound = Take(behavior->BindRestoreCandidate(std::move(binding)));
            behavior = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(bound.AcquireAdapter());
            installation.emplace(std::move(bound));
            lifecycle = Take(BehaviorLifecyclePersistenceAdapter::Create(owner, [this] {
                return Result<PersistentEntityIdentityMap>::Success(Map(*scenes.ActiveScene()));
            }));
            registry = std::make_unique<CanonicalStateParticipantRegistry>();
            static_cast<void>(Take(registry->Register(behavior->Descriptor().participant, behavior)));
            static_cast<void>(Take(registry->Register(BehaviorLifecycleParticipant(), lifecycle)));
            for (unsigned i = 1; i < 4; ++i) {
                auto peer =
                    Take(module->AcquireInstalledPersistence(Take(SaveParticipantId::Parse(std::format("game.tests.durable{}", i)))));
                auto adapter = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(peer.AcquireAdapter());
                REQUIRE(adapter);
                static_cast<void>(Take(registry->Register(adapter->Descriptor().participant, adapter)));
                peers.push_back(adapter);
                peerInstallations.push_back(std::move(peer));
            }
            participants = Take(registry->Snapshot());
        }

        ~Fixture() {
            scenes.Shutdown();
            participants = {};
            registry.reset();
            behavior.reset();
            peers.clear();
            peerInstallations.clear();
            installation.reset();
            lifecycle.reset();
            owner.reset();
            module.reset();
        }

        void Pump() {
            Accept(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {1, {}, 0, 0, {}, false, cancellation.Token()}));
        }

        void Ordinary(std::uint64_t revision, std::uint64_t count) {
            Accept(scenes.QueuePreparation(Definition(revision, count, duplicateIds)));
            Pump();
            REQUIRE_FALSE(scenes.TakeOperationError());
            Accept(owner->ActivatePublished());
        }

        std::vector<Tests::GameplayBehaviorDurableFields> FieldsAt(bool write = false, bool peer = false) {
            Fields visitor;
            visitor.write = write;
            visitor.peer = peer;
            auto target = Take(owner->BindingForActive(Type(), Map(*scenes.ActiveScene())));
            Accept(target.Visit(visitor));
            return visitor.actual;
        }

        RuntimeSaveSnapshot Capture() {
            auto builder = Take(RuntimeSaveCaptureBuilder::Create({.capturedState = IdValue<CapturedStateId>(9),
                                                                   .epoch = {1},
                                                                   .sceneIncarnation = scenes.ActiveScene()->RuntimeId().value,
                                                                   .sceneRevision = scenes.ActiveScene()->DefinitionRevision().value,
                                                                   .registryGeneration = participants.Generation()},
                                                                  participants));
            Accept(builder.CaptureParticipants());
            return Take(builder.Seal());
        }

        Result<void> Queue(RuntimeSaveSnapshot snapshot, std::uint64_t count = 2) {
            auto source = std::make_unique<Source>();
            source->owner = owner;
            source->behavior = behavior;
            source->peers = peers;
            source->lifecycle = lifecycle;
            source->snapshot = std::move(snapshot);
            source->producer =
                Take(CreateSaveOperation({.operation = 1437, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}));
            operation = source->producer->Handle();
            auto *borrow = source.get();
            authority->current = {participants.Generation(), 17, scenes.ActiveScene()->RuntimeId().value};
            SceneRestoreBundleRequest request;
            request.context = {1437, participants.Generation(), 17, scenes.ActiveScene()->RuntimeId().value, 5};
            request.world = IdValue<SaveWorldId>(1);
            request.definition = {3};
            request.revision = {3};
            request.participants = participants;
            request.entities = Records(count);
            auto bundle = Take(SceneRestoreBundle::Create(std::move(request), std::move(source), authority));
            return scenes.QueuePreparationWithRestore(Definition(3, count, duplicateIds),
                                                      std::make_unique<Observed>(std::move(bundle), owner, *borrow));
        }

        void DetachNativeProbe() {
            native.configure(nullptr);
            native.configure = [](Tests::GameplayRetirementFixture *) noexcept {
            };
            native.image.reset();
        }

        void ReleaseSaveBindings() {
            participants = {};
            registry.reset();
            behavior.reset();
            peers.clear();
            peerInstallations.clear();
            installation.reset();
            lifecycle.reset();
        }
    };
}  // namespace Horo::Gameplay::RestoreTest
