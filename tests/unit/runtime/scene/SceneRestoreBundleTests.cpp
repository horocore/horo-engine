#include "SceneRestoreBundleTestHelpers.h"

namespace Horo::Runtime {
    namespace {
        using namespace RestoreTest;

        TEST_CASE("Schema-local reference identities remain independent across real prepared owners",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const bool duplicateOwner : {false, true}) {
                for (const bool reversed : {false, true}) {
                    TwoOwnerFixture owners;
                    auto &fixture = owners.fixture;
                    const auto &secondProject = owners.secondProject;
                    const auto firstOwner = fixture.adapter->Descriptor().participant.participant;
                    const auto secondOwner = owners.second->Descriptor().participant.participant;
                    auto bundle = owners.Prepare(duplicateOwner, reversed);
                    const auto oldScene = fixture.service.ActiveScene()->RuntimeId();
                    REQUIRE(fixture.service.QueuePreparationWithRestore(Definition(2), std::move(bundle)).HasValue());
                    fixture.control->ready = true;
                    fixture.Pump();
                    if (duplicateOwner) {
                        CHECK(fixture.service.ActiveScene()->RuntimeId() == oldScene);
                        CHECK(fixture.project->state.published == 0);
                        CHECK(secondProject->state.published == 0);
                        REQUIRE(fixture.operation.Snapshot()->terminalError);
                    } else {
                        REQUIRE(fixture.project->state.active.size() == 1);
                        REQUIRE(secondProject->state.active.size() == 1);
                        const auto &first = fixture.project->state.active.front();
                        const auto &other = secondProject->state.active.front();
                        CHECK(first.owner == firstOwner);
                        CHECK(other.owner == secondOwner);
                        CHECK(first.reference.value == 1);
                        CHECK(other.reference.value == 1);
                        CHECK(std::get<SaveRestoreEntityTarget>(first.target).identity == Id<PersistentEntityId>(2));
                        CHECK(std::get<SaveRestoreEntityTarget>(other.target).identity == Id<PersistentEntityId>(1));
                        CHECK(fixture.project->state.published == 1);
                        CHECK(secondProject->state.published == 1);
                        CHECK(fixture.project->state.candidateScene == fixture.service.ActiveScene()->RuntimeId().value);
                        CHECK(secondProject->state.candidateScene == fixture.project->state.candidateScene);
                        CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
                    }
                }
            }
        }

        TEST_CASE("Restore Scene readiness waits for actual Gameplay receipts and publishes cyclic fixups together",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            const auto old = *fixture.service.ActiveScene();
            const auto oldEntity = *old.Find(SceneObjectId{1});
            auto *bundle = fixture.Queue(fixture.Request());
            fixture.Pump();
            CHECK(fixture.service.ActiveScene()->RuntimeId() == old.RuntimeId());
            CHECK(fixture.project->state.prepared == 0);
            CHECK_FALSE(fixture.operation.Snapshot()->IsTerminal());
            fixture.control->ready = true;
            fixture.Pump();
            REQUIRE_FALSE(fixture.service.TakeOperationError());
            const auto active = *fixture.service.ActiveScene();
            CHECK(active.DefinitionRevision() == SceneDefinitionRevision{2});
            CHECK(active.Get(oldEntity).HasError());
            const auto current = *active.Find(SceneObjectId{1});
            CHECK(active.Get(current).Value().localTransform->translation.x == 42.0F);
            REQUIRE(bundle->Identities());
            CHECK(bundle->Identities()->Resolve(Id<PersistentEntityId>(1), {1}).Value() == current);
            REQUIRE(fixture.project->state.active.size() == 2);
            CHECK(fixture.project->state.active[0].reference.value == 1);
            CHECK(std::get<SaveRestoreEntityTarget>(fixture.project->state.active[0].target).identity == Id<PersistentEntityId>(2));
            CHECK(fixture.project->state.candidateScene == active.RuntimeId().value);
            CHECK(fixture.project->state.prepared == 1);
            CHECK(fixture.project->state.fixed == 1);
            CHECK(fixture.project->state.published == 1);
            CHECK(fixture.operation.Snapshot()->IsTerminal());
        }

        TEST_CASE("Required missing or stale references roll back the real staged world before any publication",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const auto invalid : {Target(9), Target(2, 2)}) {
                Fixture fixture;
                const auto old = *fixture.service.ActiveScene();
                auto request = fixture.Request();
                request.references.front().target = invalid;
                fixture.control->ready = true;
                fixture.Queue(std::move(request));
                fixture.Pump();
                REQUIRE(fixture.service.TakeOperationError());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == old.RuntimeId());
                const auto entity = *old.Find(SceneObjectId{1});
                CHECK(old.Get(entity).Value().localTransform->translation.x == 0.0F);
                CHECK(fixture.project->state.prepared == 1);
                CHECK(fixture.project->state.fixed == 0);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
                CHECK(fixture.control->cancelled == 1);
            }
        }

        TEST_CASE("A deeper prerequisite chain and closing deferred cycle restore identically in either input order",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const bool reversed : {false, true}) {
                Fixture fixture{std::unique_ptr<SceneActivationParticipant>{}, 8};
                auto request = fixture.Request();
                request.entities.clear();
                request.nodes.clear();
                request.references.clear();
                const auto owner = fixture.adapter->Descriptor().participant.participant;
                for (std::uint8_t identity = 1; identity <= 8; ++identity) {
                    request.entities.push_back(Entity(identity));
                    request.nodes.push_back(Target(identity));
                    if (identity > 1)
                        request.references.push_back({.identity = static_cast<std::uint64_t>(identity - 1),
                                                      .owner = owner,
                                                      .source = Target(identity),
                                                      .target = Target(static_cast<std::uint8_t>(identity - 1)),
                                                      .phase = RestoreReferencePhase::AllocationPrerequisite});
                }
                request.references.push_back({.identity = 8, .owner = owner, .source = Target(1), .target = Target(8)});
                if (reversed) {
                    std::ranges::reverse(request.entities);
                    std::ranges::reverse(request.nodes);
                    std::ranges::reverse(request.references);
                }
                fixture.control->ready = true;
                const auto *bundle = fixture.Queue(std::move(request));
                fixture.Pump();
                REQUIRE_FALSE(fixture.service.TakeOperationError());
                const auto active = *fixture.service.ActiveScene();
                REQUIRE(fixture.project->state.active.size() == 8);
                REQUIRE(bundle->Identities());
                for (std::uint8_t identity = 1; identity <= 8; ++identity) {
                    const auto &reference = fixture.project->state.active[identity - 1];
                    CHECK(reference.reference.value == identity);
                    CHECK(std::get<SaveRestoreEntityTarget>(reference.target).identity == Id<PersistentEntityId>(identity));
                    const auto entity = *active.Find(SceneObjectId{identity});
                    CHECK(bundle->Identities()->Resolve(Id<PersistentEntityId>(identity), {1}).Value() == entity);
                }
                CHECK(fixture.project->state.fixed == 1);
                CHECK(fixture.project->state.published == 1);
                CHECK(fixture.project->state.candidateScene == active.RuntimeId().value);
                CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
            }
        }

        /** @brief Builds the same real component payload and reference declaration for publication and foreign-fault cases. */
        SceneRestoreBundleRequest ComponentRequest(Fixture &fixture, ComponentState &component) {
            Gameplay::ComponentRegistry types;
            REQUIRE(types.Register({.typeId = ComponentType(), .schemaVersion = 1, .displayName = "Links"}).HasValue());
            REQUIRE(types.Freeze().HasValue());
            const std::array requirements{SaveableComponentRequirement{ComponentType(), SaveableComponentPresence::Required}};
            const std::array<std::shared_ptr<const ISaveableComponentStateAdapter>, 1> adapters{
                std::make_shared<ComponentAdapter>(component)};
            auto registry = SaveableComponentAdapterRegistry::Create(types, requirements, adapters);
            REQUIRE(registry.HasValue());
            auto request = fixture.Request();
            request.componentAdapters = std::make_shared<SaveableComponentAdapterRegistry>(std::move(registry).Value());
            CanonicalValueWriter writer;
            REQUIRE(writer.WriteUInt32(73).HasValue());
            const auto encoded = std::move(writer).Finalize().Value();
            const RestoreComponentReference source{Target(1), ComponentType()};
            request.nodes.push_back(source);
            request.references.push_back(
                {.identity = 3, .owner = fixture.adapter->Descriptor().participant.participant, .source = source, .target = Target(2)});
            request.components.push_back({.record = {.entity = Id<PersistentEntityId>(1),
                                                     .generation = {1},
                                                     .type = ComponentType(),
                                                     .schemaVersion = 1,
                                                     .payload = {encoded.Bytes().begin(), encoded.Bytes().end()}},
                                          .owner = fixture.adapter->Descriptor().participant.participant,
                                          .references = {{3}}});
            return request;
        }

        TEST_CASE("Component payload fixups publish with actual Scene and Gameplay owners through one gate",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            ComponentState component;
            auto request = ComponentRequest(fixture, component);
            fixture.control->ready = true;
            fixture.Queue(std::move(request));
            fixture.Pump();
            REQUIRE_FALSE(fixture.service.TakeOperationError());
            REQUIRE(fixture.service.ActiveScene()->Get(component.entity).HasValue());
            CHECK(component.value == 73);
            CHECK(component.linked == Id<PersistentEntityId>(2));
            CHECK(component.fixed == 1);
            CHECK(component.published == 1);
            CHECK(fixture.project->state.published == 1);
            CHECK(fixture.operation.Snapshot()->IsTerminal());
        }

        TEST_CASE("Foreign component preparation faults preserve the old Scene and terminate without publication",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const unsigned fault : {1U, 2U, 3U}) {
                Fixture fixture;
                const auto oldScene = fixture.service.ActiveScene()->RuntimeId();
                ComponentState component;
                component.prepareFault = fault;
                auto request = ComponentRequest(fixture, component);
                fixture.control->ready = true;
                auto producer =
                    CreateSaveOperation({.operation = 1437, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
                fixture.operation = producer.Handle();
                auto source = std::make_unique<OwnerSource>(fixture.adapter, *fixture.snapshot, std::move(producer), fixture.control);
                auto bundle = SceneRestoreBundle::Create(std::move(request), std::move(source), fixture.authority);
                REQUIRE(bundle.HasValue());
                const auto rejected =
                    fixture.service.QueuePreparationWithRestore(Definition(2, fixture.entityCount), std::move(bundle).Value());
                REQUIRE(rejected.HasError());
                const auto &expected = fault == 1 ? SaveErrors::RestoreAllocationFailed : SaveErrors::RestoreAdapterContractInvalid;
                CHECK(rejected.ErrorValue().code.Value() == expected.code.Value());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == oldScene);
                CHECK(component.published == 0);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }

        TEST_CASE("Cancellation and live generation changes discard pending restore without changing active roots",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const bool stale : {false, true}) {
                Fixture fixture;
                const auto old = fixture.service.ActiveScene()->RuntimeId();
                fixture.Queue(fixture.Request());
                if (stale)
                    ++fixture.authority->evidence.sessionGeneration;
                else
                    static_cast<void>(fixture.operation.RequestCancellation());
                fixture.control->ready = stale;
                fixture.Pump();
                REQUIRE(fixture.service.TakeOperationError());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == old);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }

        TEST_CASE("Legacy Gameplay receipts reject declared references and invalid allocation cycles cannot activate",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const bool legacy : {false, true}) {
                Fixture fixture;
                const auto old = fixture.service.ActiveScene()->RuntimeId();
                auto request = fixture.Request();
                fixture.project->legacy = legacy;
                if (!legacy)
                    for (auto &reference : request.references)
                        reference.phase = RestoreReferencePhase::AllocationPrerequisite;
                fixture.control->ready = true;
                fixture.Queue(std::move(request));
                fixture.Pump();
                REQUIRE(fixture.service.TakeOperationError());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == old);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }

        TEST_CASE("Legacy non-reference Gameplay schemas retain their source-compatible empty fixup path",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            fixture.project->legacy = true;
            auto request = fixture.Request();
            request.references.clear();
            fixture.control->ready = true;
            fixture.Queue(std::move(request));
            fixture.Pump();
            REQUIRE_FALSE(fixture.service.TakeOperationError());
            CHECK(fixture.service.ActiveScene()->DefinitionRevision() == SceneDefinitionRevision{2});
            CHECK(fixture.project->state.published == 1);
            CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
        }

        TEST_CASE("Completion observes the whole new bundle while the old Scene remains retained through deferred shutdown",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            const auto old = *fixture.service.ActiveScene();
            const auto oldEntity = *old.Find(SceneObjectId{1});
            fixture.Queue(fixture.Request());
            bool observed{};
            REQUIRE(fixture.operation
                        .OnCompletion([&](const SaveOperationSnapshot &snapshot) {
                CHECK(snapshot.state == SaveOperationState::Completed);
                CHECK(old.Get(oldEntity).HasValue());
                CHECK(fixture.service.ActiveScene()->DefinitionRevision() == SceneDefinitionRevision{2});
                CHECK(fixture.project->state.published == 1);
                CHECK(fixture.project->state.active.size() == 2);
                fixture.service.Shutdown();
                // Shutdown called by a completion observer is deferred until the scoped gate returns.
                CHECK(fixture.service.ActiveScene().has_value());
                observed = true;
            }).HasValue());
            fixture.control->ready = true;
            fixture.Pump();
            CHECK(observed);
            CHECK_FALSE(fixture.service.ActiveScene());
            CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
        }

        TEST_CASE("Pending restore shutdown terminalizes the sole producer without preparing owner candidates",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            fixture.Queue(fixture.Request());
            fixture.Pump();
            CHECK_FALSE(fixture.operation.Snapshot()->IsTerminal());
            fixture.service.Shutdown();
            CHECK(fixture.control->cancelled == 1);
            CHECK(fixture.project->state.prepared == 0);
            CHECK(fixture.operation.Snapshot()->IsTerminal());
        }

        TEST_CASE("Repeated failed owner preparation preserves the terminal cause instead of reporting pending",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture fixture;
            auto request = fixture.Request();
            request.references.front().target = Target(9);
            auto producer =
                CreateSaveOperation({.operation = 1437, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
            const auto operation = producer.Handle();
            auto source = std::make_unique<OwnerSource>(fixture.adapter, *fixture.snapshot, std::move(producer), fixture.control);
            auto bundle = SceneRestoreBundle::Create(std::move(request), std::move(source), fixture.authority).Value();
            auto candidate = RuntimeScene::Create(Definition(2), SceneRuntimeId{999}).Value();
            REQUIRE(bundle->PrepareScene(*candidate).HasValue());
            fixture.control->ready = true;
            const auto failed = bundle->PrepareOwners();
            REQUIRE(failed.HasError());
            REQUIRE(operation.Snapshot()->IsTerminal());
            const auto repeated = bundle->PrepareOwners();
            REQUIRE(repeated.HasError());
            CHECK(repeated.ErrorValue().domain.Value() == failed.ErrorValue().domain.Value());
            CHECK(repeated.ErrorValue().code.Value() == failed.ErrorValue().code.Value());
            CHECK(fixture.project->state.published == 0);
            bundle->Rollback();
        }

        TEST_CASE("Ordinary Scene candidate rejection prevents every prepared restore root from publishing",
                  "[unit][runtime][scene][save][restore][references]") {
            auto control = std::make_shared<SceneParticipantControl>();
            Fixture fixture{std::make_unique<SceneParticipant>(control)};
            REQUIRE(control->published == 1);
            const auto old = fixture.service.ActiveScene()->RuntimeId();
            fixture.Queue(fixture.Request());
            control->reject = true;
            fixture.control->ready = true;
            fixture.Pump();
            REQUIRE(fixture.service.TakeOperationError());
            CHECK(fixture.service.ActiveScene()->RuntimeId() == old);
            CHECK(fixture.project->state.fixed == 1);
            CHECK(fixture.project->state.published == 0);
            CHECK(control->published == 1);
            CHECK(fixture.operation.Snapshot()->IsTerminal());
        }

        TEST_CASE("Prepared Gameplay fixup exceptions retain their phase-specific typed errors before activation",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const unsigned failure : {0U, 1U, 2U}) {
                Fixture fixture;
                const auto old = fixture.service.ActiveScene()->RuntimeId();
                fixture.project->state.throwFixup = true;
                fixture.project->state.throwAllocation = failure == 1;
                fixture.project->state.throwNonStandard = failure == 2;
                fixture.control->ready = true;
                fixture.Queue(fixture.Request());
                fixture.Pump();
                const auto error = fixture.service.TakeOperationError();
                REQUIRE(error);
                const auto &expected = failure == 1 ? SaveErrors::RestoreAllocationFailed : SaveErrors::LifecycleCallbackFailed;
                CHECK(error->domain.Value() == expected.domain.Value());
                CHECK(error->code.Value() == expected.code.Value());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == old);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }

        TEST_CASE("Gameplay foreign fixup translation returns owned failure without allocating while the allocator rejects new work",
                  "[unit][runtime][scene][save][restore][references]") {
            class NoDependencies final : public ICanonicalRestoreDependencyLookup {
            public:
                const ICanonicalRestorePreparedState *Find(const SaveParticipantId &) const noexcept override {
                    return nullptr;
                }
            } dependencies;

            for (const bool allocationFault : {false, true}) {
                Fixture fixture;
                const auto &descriptor = fixture.adapter->Descriptor();
                const auto records = fixture.snapshot->Records();
                REQUIRE(records.size() == 1);
                auto staged =
                    fixture.adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, records.front().Segment(0));
                REQUIRE(staged.HasValue());
                auto receipt = std::move(staged).Value();
                const auto context = fixture.Request().context;
                REQUIRE(receipt->Decode(context).HasValue());
                REQUIRE(receipt->Validate(context).HasValue());
                REQUIRE(receipt->Instantiate(context).HasValue());
                REQUIRE(receipt->ApplyState(dependencies).HasValue());
                auto &state = fixture.project->state;
                state.throwFixup = true;
                state.throwAllocation = allocationFault;
                state.throwNonStandard = !allocationFault;
                state.failFailureTranslationAllocation = true;
                std::optional<Result<void>> translated;
                bool escaped{};
                try {
                    translated.emplace(receipt->FixupReferences(dependencies, {}));
                } catch (...) {
                    escaped = true;
                }
                const auto allocationsAfterReturn = Tests::AllocationProbe::Count();
                state.failureTranslation.reset();
                REQUIRE_FALSE(escaped);
                REQUIRE(translated);
                REQUIRE(translated->HasError());
                CHECK(allocationsAfterReturn == state.allocationCountAtFault);
                const auto &expected = allocationFault ? SaveErrors::RestoreAllocationFailed : SaveErrors::LifecycleCallbackFailed;
                CHECK(translated->ErrorValue().code.Value() == expected.code.Value());
                CHECK(state.published == 0);
                receipt->RollbackPrepared();
            }
        }

        TEST_CASE("Foreign owner source faults retain the sole producer and unpublished scene until normal rollback",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const unsigned fault : {1U, 2U, 3U}) {
                Fixture fixture;
                const auto old = fixture.service.ActiveScene()->RuntimeId();
                fixture.control->sourceFault = fault;
                fixture.control->ready = true;
                fixture.Queue(fixture.Request());
                fixture.Pump();
                const auto error = fixture.service.TakeOperationError();
                REQUIRE(error);
                const auto &expected = fault == 1 ? SaveErrors::RestoreAllocationFailed : SaveErrors::RestoreAdapterContractInvalid;
                CHECK(error->code.Value() == expected.code.Value());
                CHECK(fixture.service.ActiveScene()->RuntimeId() == old);
                CHECK(fixture.project->state.prepared == 0);
                CHECK(fixture.project->state.published == 0);
                CHECK(fixture.control->cancelled == 1);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }

        TEST_CASE("Pending cancellation observers cannot destroy an executing restore preparation or rollback",
                  "[unit][runtime][scene][save][restore][references]") {
            for (const unsigned path : {0U, 1U, 2U}) {
                Fixture fixture;
                const auto old = *fixture.service.ActiveScene();
                const auto entity = *old.Find(SceneObjectId{1});
                fixture.Queue(fixture.Request());
                unsigned observed{};
                REQUIRE(fixture.operation
                            .OnCompletion([&](const SaveOperationSnapshot &snapshot) {
                    CHECK(snapshot.IsTerminal());
                    CHECK(old.Get(entity).HasValue());
                    CHECK(fixture.project->state.published == 0);
                    CHECK(fixture.service.QueueUnload().HasError());
                    fixture.service.Shutdown();
                    CHECK(fixture.service.ActiveScene().has_value());
                    ++observed;
                }).HasValue());
                if (path == 0) {
                    static_cast<void>(fixture.operation.RequestCancellation());
                    fixture.Pump();
                } else if (path == 1) {
                    REQUIRE(fixture.service.QueueUnload().HasValue());
                } else {
                    fixture.service.Shutdown();
                }
                CHECK(observed == 1);
                CHECK_FALSE(fixture.service.ActiveScene());
                CHECK(fixture.project->state.prepared == 0);
                CHECK(fixture.operation.Snapshot()->IsTerminal());
            }
        }
    }  // namespace
}  // namespace Horo::Runtime
