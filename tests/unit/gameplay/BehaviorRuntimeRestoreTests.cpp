#include "AllocationProbe.h"
#include "BehaviorRestoreNativeTestSupport.h"

using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Gameplay::RestoreTest;
using namespace Horo::Runtime;

TEST_CASE("Canonical Gameplay lifecycle schema preserves actual disabled and pre-start flags", "[gameplay][save][restore][lifecycle]") {
    BehaviorLifecycleSnapshot snapshot;
    snapshot.instances = {{IdValue<PersistentEntityId>(1), {7}, {3}, Type(), {true, false, false, false}},
                          {IdValue<PersistentEntityId>(2), {9}, {4}, Type(), {true, true, true, true}}};
    auto bytes = Take(EncodeBehaviorLifecycle(snapshot));
    auto restored = Take(DecodeBehaviorLifecycle(bytes.Bytes()));
    REQUIRE(restored.instances.size() == 2);
    CHECK(restored.instances[0].flags == snapshot.instances[0].flags);
    CHECK(restored.instances[1].flags == snapshot.instances[1].flags);
    CHECK(restored.instances[0].generation == PersistentEntityGeneration{7});
    auto second = Take(EncodeBehaviorLifecycle(restored));
    CHECK(std::ranges::equal(bytes.Bytes(), second.Bytes()));
}

TEST_CASE("Lifecycle codec rejects duplicate identities invalid flags and noncanonical order", "[gameplay][save][restore][lifecycle]") {
    const BehaviorLifecycleEntry valid{IdValue<PersistentEntityId>(1), {1}, {1}, Type(), {true, true, false, true}};
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        BehaviorLifecycleSnapshot snapshot{{valid}};
        if (invalid == 0)
            snapshot.instances.front().flags = {false, true, false, true};
        if (invalid == 1)
            snapshot.instances.front().flags = {true, true, false, false};
        if (invalid == 2)
            snapshot.instances.push_back(valid);
        if (invalid == 3) {
            auto other = valid;
            other.entity = IdValue<PersistentEntityId>(2);
            other.instance = {2};
            snapshot.instances.insert(snapshot.instances.begin(), other);
        }
        CHECK(EncodeBehaviorLifecycle(snapshot).HasError());
    }
}

TEST_CASE("Lifecycle decoder enforces schema trailing bytes and allocation admission", "[gameplay][save][restore][lifecycle]") {
    BehaviorLifecycleSnapshot snapshot{{{IdValue<PersistentEntityId>(1), {1}, {1}, Type(), {true, false, false, false}}}};
    const auto encoded = Take(EncodeBehaviorLifecycle(snapshot));
    std::vector<std::byte> bytes{encoded.Bytes().begin(), encoded.Bytes().end()};
    bytes.push_back(std::byte{0});
    CHECK(DecodeBehaviorLifecycle(bytes).HasError());
    bytes.pop_back();
    bytes[0] = std::byte{2};
    CHECK(DecodeBehaviorLifecycle(bytes).HasError());
    CanonicalCodecLimits limits;
    limits.maximumBytes = 1;
    CHECK(DecodeBehaviorLifecycle(encoded.Bytes(), limits).HasError());
}

TEST_CASE("Actual module Save Load Save resurrects attachments and materializes before real graph fixup",
          "[gameplay][save][restore][native]") {
    Fixture fixture;
    fixture.FieldsAt(true, true);
    auto saved = fixture.Capture();
    const auto created = fixture.native.control.behaviorCreated;
    fixture.Ordinary(2, 1);
    const auto oldScene = fixture.scenes.ActiveScene()->RuntimeId();
    Accept(fixture.Queue(saved));
    fixture.Pump();
    REQUIRE_FALSE(fixture.scenes.TakeOperationError());
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() != oldScene);
    REQUIRE(fixture.owner->Active());
    CHECK(fixture.owner->Active()->InstanceCount() == 2);
    CHECK(fixture.native.control.behaviorCreated == created + 1);
    CHECK(fixture.native.control.behaviorPrehooks == 2);
    CHECK(fixture.native.control.behaviorMaterialized == 1);
    CHECK(fixture.native.control.behaviorFixed == 1);
    auto values = fixture.FieldsAt();
    REQUIRE(values.size() == 2);
    CHECK(values[0].value == 43);
    CHECK(values[1].value == 44);
    REQUIRE(values[0].resolvedPeer);
    CHECK(values[0].resolvedPeer->identity == IdValue<PersistentEntityId>(2));
    const auto again = fixture.Capture();
    REQUIRE(again.Records().size() == saved.Records().size());
    for (std::size_t i = 0; i < saved.Records().size(); ++i)
        CHECK(std::ranges::equal(again.Records()[i].Segment(0), saved.Records()[i].Segment(0)));
    CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
}

TEST_CASE("Durable restore preserves disabled and never-started instances without replaying initialization",
          "[gameplay][save][restore][native]") {
    Fixture fixture;
    Accept(fixture.owner->Active()->SetEnabled({2}, false));
    fixture.FieldsAt(true);
    auto saved = fixture.Capture();
    const auto started = fixture.native.control.behaviorStarted;
    Accept(fixture.Queue(std::move(saved)));
    fixture.Pump();
    REQUIRE_FALSE(fixture.scenes.TakeOperationError());
    CHECK(fixture.native.control.behaviorStarted == started);
    auto flags = Take(fixture.owner->CaptureLifecycle(Map(*fixture.scenes.ActiveScene())));
    REQUIRE(flags.instances.size() == 2);
    CHECK_FALSE(flags.instances[0].flags.started);
    CHECK_FALSE(flags.instances[1].flags.enabled);
    Accept(fixture.owner->Active()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}));
    CHECK(fixture.native.control.behaviorStarted == started + 1);
    auto values = fixture.FieldsAt();
    CHECK(values[0].value == 17);
    CHECK(values[1].value == 44);
    Accept(fixture.owner->Active()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}));
    CHECK(fixture.native.control.behaviorStarted == started + 1);
    CHECK(fixture.owner->ActivatePublished().HasError());
}

TEST_CASE("Previously started durable behavior does not replay OnStart after materialization", "[gameplay][save][restore][native]") {
    Fixture fixture;
    Accept(fixture.owner->Active()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}));
    fixture.FieldsAt(true);
    auto saved = fixture.Capture();
    const auto started = fixture.native.control.behaviorStarted;
    Accept(fixture.Queue(std::move(saved)));
    fixture.Pump();
    REQUIRE_FALSE(fixture.scenes.TakeOperationError());
    Accept(fixture.owner->Active()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}));
    CHECK(fixture.native.control.behaviorStarted == started);
    CHECK(fixture.FieldsAt()[0].value == 43);
}

TEST_CASE("Actual candidate construction prehook materialization and fixup failures preserve the live Scene",
          "[gameplay][save][restore][native]") {
    for (const unsigned failure : {1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U, 19U, 21U, 22U}) {
        Fixture fixture;
        fixture.FieldsAt(true, true);
        auto saved = fixture.Capture();
        const auto old = fixture.scenes.ActiveScene()->RuntimeId();
        fixture.native.control.restoreFailure = failure;
        fixture.native.control.failBehaviorConstruction = failure == 8;
        auto queued = fixture.Queue(std::move(saved));
        if (failure == 19 || failure == 21 || failure == 22) {
            REQUIRE(queued.HasError());
            const auto &expected = failure == 21 ? SaveErrors::RestoreAllocationFailed : SaveErrors::LifecycleCallbackFailed;
            CHECK(queued.ErrorValue().code.Value() == expected.code.Value());
            CHECK(fixture.native.control.behaviorMaterialized == 0);
        }
        if (queued.HasValue())
            fixture.Pump();
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
        CHECK(fixture.operation.Snapshot()->IsTerminal());
        CHECK(fixture.FieldsAt()[0].value == 43);
        CHECK(fixture.native.control.behaviorCreated == 2);
        fixture.native.control.restoreFailure = 0;
        fixture.native.control.failBehaviorConstruction = false;
    }
}

TEST_CASE("Deferred published native retirement preserves Load outcome and blocks reload until real retry",
          "[gameplay][save][restore][native]") {
    Fixture fixture;
    fixture.FieldsAt(true);
    auto saved = fixture.Capture();
    const auto old = fixture.scenes.ActiveScene()->RuntimeId();
    fixture.native.control.restoreFailure = 5;
    Accept(fixture.Queue(std::move(saved)));
    fixture.Pump();
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() != old);
    CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
    CHECK(fixture.owner->QuiesceForReload().HasError());
    REQUIRE(fixture.owner->Active());
    CHECK(fixture.FieldsAt()[0].value == 43);
    fixture.native.control.restoreFailure = 0;
    CHECK(Take(fixture.scenes.RetirePublishedRestorePreparation()));
    CHECK(fixture.native.control.preparationRetired == 4);
    auto roundtrip = fixture.Capture();
    CHECK(fixture.FieldsAt()[0].value == 43);
    roundtrip = {};
    auto reload = Take(fixture.owner->Active()->CaptureReloadSnapshot());
    Accept(fixture.owner->QuiesceForReload());
    fixture.ReleaseSaveBindings();
    auto moduleState = fixture.module->PrepareReload();
    REQUIRE(moduleState.HasValue());
    fixture.DetachNativeProbe();
    fixture.module.reset();
    fixture.module = Take(fixture.host.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
    Accept(fixture.owner->ResumeAfterReload(fixture.module->Registry(), reload));
    CHECK(fixture.FieldsAt()[0].value == 43);
}

TEST_CASE("Published Load survives real native retirement callback failures and permits an explicit retry",
          "[gameplay][save][restore][native]") {
    for (unsigned failure : {10U, 11U}) {
        Fixture fixture;
        fixture.FieldsAt(true);
        auto saved = fixture.Capture();
        const auto old = fixture.scenes.ActiveScene()->RuntimeId();
        fixture.native.control.restoreFailure = failure;
        Accept(fixture.Queue(std::move(saved)));
        fixture.Pump();
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() != old);
        CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
        REQUIRE(fixture.scenes.TakeOperationError());
        CHECK(fixture.FieldsAt()[0].value == 43);
        CHECK(fixture.owner->QuiesceForReload().HasError());
        fixture.native.control.restoreFailure = 0;
        CHECK(Take(fixture.scenes.RetirePublishedRestorePreparation()));
    }
}

TEST_CASE("Engine-issued target is revoked across same Scene generation replacement", "[gameplay][save][restore][native][reload]") {
    Fixture fixture;
    fixture.FieldsAt(true);
    auto retained = Take(fixture.owner->BindingForActive(Type(), Map(*fixture.scenes.ActiveScene())));
    auto reload = Take(fixture.owner->Active()->CaptureReloadSnapshot());
    Accept(fixture.owner->QuiesceForReload());
    CHECK_FALSE(retained.CanUse());
    fixture.ReleaseSaveBindings();
    REQUIRE(fixture.module->PrepareReload().HasValue());
    fixture.DetachNativeProbe();
    fixture.module.reset();
    fixture.module = Take(fixture.host.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
    Accept(fixture.owner->ResumeAfterReload(fixture.module->Registry(), reload));
    auto current = Take(fixture.owner->BindingForActive(Type(), Map(*fixture.scenes.ActiveScene())));
    Fields visitor;
    CHECK(current.CanUse());
    CHECK_FALSE(retained.CanUse());
    CHECK(retained.Visit(visitor).HasError());
    CHECK_FALSE(retained.SameOwner(current));
    CHECK_FALSE(retained.SameOwner(retained));
}

TEST_CASE("Same-count changed attachment identity cannot receive old reload lifecycle flags", "[gameplay][save][restore][native][reload]") {
    for (bool changeType : {false, true}) {
        Fixture fixture;
        auto reload = Take(fixture.owner->Active()->CaptureReloadSnapshot());
        Accept(fixture.owner->QuiesceForReload());
        const auto created = fixture.native.control.behaviorCreated;
        const auto old = *fixture.scenes.ActiveScene()->Find({1});
        SceneCommandBuffer commands;
        commands.Destroy(old);
        RuntimeEntityCreateInfo replacement;
        replacement.authoredObject = SceneObjectId{1};
        auto type = changeType ? Take(BehaviorTypeId::Parse("game.tests.prefab_spawner")) : Type();
        replacement.components.behaviors.emplace_back(BehaviorInstanceId{changeType ? 1U : 3U}, std::move(type), 1, true,
                                                      std::vector<BehaviorField>{});
        static_cast<void>(commands.Create(std::move(replacement)));
        Accept(fixture.scenes.QueueStructuralCommands(std::move(commands)));
        fixture.Pump();
        REQUIRE_FALSE(fixture.scenes.TakeOperationError());
        auto resumed = fixture.owner->ResumeAfterReload(fixture.module->Registry(), reload);
        CHECK(resumed.HasError());
        CHECK_FALSE(fixture.owner->Active());
        CHECK(fixture.native.control.behaviorCreated == created);
    }
}

TEST_CASE("Inactive restore rejects genuinely unavailable required services before project prehooks", "[gameplay][save][restore][native]") {
    for (unsigned required : {1U, 2U, 4U, 8U, 16U}) {
        Fixture fixture;
        auto saved = fixture.Capture();
        const auto old = fixture.scenes.ActiveScene()->RuntimeId();
        fixture.native.control.restoreRequiredServices = required;
        auto queued = fixture.Queue(std::move(saved));
        if (queued.HasValue())
            fixture.Pump();
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
        CHECK(fixture.native.control.behaviorPrehooks == 0);
        CHECK(fixture.operation.Snapshot()->IsTerminal());
    }
}

TEST_CASE("Actual native inventory rejects foreign duplicate absent source and missing component references",
          "[gameplay][save][restore][native][references]") {
    for (unsigned failure : {12U, 13U, 14U, 15U}) {
        Fixture fixture;
        fixture.FieldsAt(true, true);
        auto saved = fixture.Capture();
        const auto old = fixture.scenes.ActiveScene()->RuntimeId();
        fixture.native.control.restoreFailure = failure;
        auto queued = fixture.Queue(std::move(saved));
        if (queued.HasValue())
            fixture.Pump();
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
        CHECK(fixture.FieldsAt()[0].value == 43);
        CHECK(fixture.operation.Snapshot()->IsTerminal());
        CHECK(fixture.native.control.behaviorFixed == 0);
    }
}

TEST_CASE("Stopped Scene invalidates retained installed capture without entering retired native objects",
          "[gameplay][save][restore][native]") {
    Fixture fixture;
    auto target = Take(fixture.owner->BindingForActive(Type(), Map(*fixture.scenes.ActiveScene())));
    const auto old = *fixture.scenes.ActiveScene();
    const auto captured = fixture.native.control.behaviorCaptured;
    fixture.scenes.Shutdown();
    CHECK_FALSE(target.CanUse());
    auto builder = Take(RuntimeSaveCaptureBuilder::Create({.capturedState = IdValue<CapturedStateId>(9),
                                                           .epoch = {1},
                                                           .sceneIncarnation = old.RuntimeId().value,
                                                           .sceneRevision = old.DefinitionRevision().value,
                                                           .registryGeneration = fixture.participants.Generation()},
                                                          fixture.participants));
    CHECK(builder.CaptureParticipants().HasError());
    CHECK(fixture.native.control.behaviorCaptured == captured);
}

namespace {
    class RetirementAllocationFault final {
    public:
        explicit RetirementAllocationFault(Tests::GameplayRetirementFixture &control) : control_(control) {
            control_.allocationOwner = this;
            control_.armAllocationFailure = [](void *owner) noexcept {
                static_cast<RetirementAllocationFault *>(owner)->failure_.emplace(0);
            };
            control_.restoreFailure = 20;
        }

        ~RetirementAllocationFault() {
            control_.armAllocationFailure = nullptr;
            control_.allocationOwner = nullptr;
            control_.restoreFailure = 0;
        }

        void Reset() noexcept {
            failure_.reset();
        }

    private:
        Tests::GameplayRetirementFixture &control_;
        std::optional<Tests::AllocationProbe::ScopedFailure> failure_;
    };
}  // namespace

TEST_CASE("Actual allocation failure during post-publication cleanup cannot roll back committed Scene",
          "[gameplay][save][restore][native][allocation]") {
    Fixture fixture;
    fixture.FieldsAt(true);
    auto saved = fixture.Capture();
    const auto old = fixture.scenes.ActiveScene()->RuntimeId();
    Accept(fixture.Queue(std::move(saved)));
    RetirementAllocationFault fault{fixture.native.control};
    auto phase =
        fixture.scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {1, {}, 0, 0, {}, false, fixture.cancellation.Token()});
    fault.Reset();
    fixture.native.control.restoreFailure = 0;
    Accept(std::move(phase));
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() != old);
    CHECK(fixture.operation.Snapshot()->state == SaveOperationState::Completed);
    REQUIRE(fixture.scenes.TakeOperationError());
    CHECK(fixture.FieldsAt()[0].value == 43);
    CHECK(Take(fixture.scenes.RetirePublishedRestorePreparation()));
}

TEST_CASE("Cancellation revokes only the unpublished native behavior candidate", "[gameplay][save][restore][native]") {
    Fixture fixture;
    fixture.FieldsAt(true);
    auto saved = fixture.Capture();
    const auto old = fixture.scenes.ActiveScene()->RuntimeId();
    auto activeTarget = Take(fixture.owner->BindingForActive(Type(), Map(*fixture.scenes.ActiveScene())));
    Accept(fixture.Queue(std::move(saved)));
    static_cast<void>(fixture.operation.RequestCancellation());
    fixture.Pump();
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
    CHECK(activeTarget.CanUse());
    CHECK(fixture.FieldsAt()[0].value == 43);
    CHECK(fixture.operation.Snapshot()->IsTerminal());
    CHECK(fixture.native.control.behaviorMaterialized == 0);
}

TEST_CASE("Actual module admission revocation prevents pending restore materialization", "[gameplay][save][restore][native]") {
    Fixture fixture;
    auto saved = fixture.Capture();
    const auto old = fixture.scenes.ActiveScene()->RuntimeId();
    auto target = Take(fixture.owner->BindingForActive(Type(), Map(*fixture.scenes.ActiveScene())));
    Accept(fixture.Queue(std::move(saved)));
    fixture.module->RequestRetirement();
    CHECK_FALSE(target.CanUse());
    fixture.Pump();
    CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
    CHECK(fixture.operation.Snapshot()->IsTerminal());
    CHECK(fixture.native.control.behaviorMaterialized == 0);
}

TEST_CASE("Installed source rejects foreign-thread capture before mutable Scene slot admission",
          "[gameplay][save][restore][native][thread]") {
    Fixture fixture;
    const auto captured = fixture.native.control.behaviorCaptured;
    const auto old = *fixture.scenes.ActiveScene();
    const RuntimeSaveCaptureProvenance provenance{.capturedState = IdValue<CapturedStateId>(9),
                                                  .epoch = {1},
                                                  .sceneIncarnation = old.RuntimeId().value,
                                                  .sceneRevision = old.DefinitionRevision().value,
                                                  .registryGeneration = fixture.participants.Generation()};
    auto worker = std::async(std::launch::async, [participants = fixture.participants, provenance] {
        auto created = RuntimeSaveCaptureBuilder::Create(provenance, participants);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        auto builder = std::move(created).Value();
        return builder.CaptureParticipants();
    });
    const auto attempted = worker.get();
    REQUIRE(attempted.HasError());
    CHECK(attempted.ErrorValue().code.Value() == SaveErrors::ThreadAffinityViolation.code.Value());
    CHECK(fixture.native.control.behaviorCaptured == captured);
}

TEST_CASE("Per-object duplicate attachment IDs survive real Save Load Save and exact same-Scene reload",
          "[gameplay][save][restore][native][reload]") {
    Fixture fixture{true};
    fixture.FieldsAt(true, true);
    auto saved = fixture.Capture();
    Accept(fixture.Queue(std::move(saved)));
    fixture.Pump();
    REQUIRE_FALSE(fixture.scenes.TakeOperationError());
    auto actual = fixture.FieldsAt();
    REQUIRE(actual.size() == 2);
    CHECK(actual[0].value == 43);
    CHECK(actual[1].value == 44);
    auto lifecycle = Take(fixture.owner->CaptureLifecycle(Map(*fixture.scenes.ActiveScene())));
    REQUIRE(lifecycle.instances.size() == 2);
    CHECK(lifecycle.instances[0].instance == lifecycle.instances[1].instance);
    CHECK(lifecycle.instances[0].entity != lifecycle.instances[1].entity);
    CHECK(lifecycle.instances[0].flags.enabled);
    CHECK_FALSE(lifecycle.instances[1].flags.enabled);
    auto roundtrip = fixture.Capture();
    CHECK(roundtrip.IsValid());
    roundtrip = {};
    auto reload = Take(fixture.owner->Active()->CaptureReloadSnapshot());
    REQUIRE(reload.instances.size() == 2);
    CHECK(reload.instances[0].instanceId == reload.instances[1].instanceId);
    CHECK(reload.instances[0].entity != reload.instances[1].entity);
    Accept(fixture.owner->QuiesceForReload());
    fixture.ReleaseSaveBindings();
    REQUIRE(fixture.module->PrepareReload().HasValue());
    fixture.DetachNativeProbe();
    fixture.module.reset();
    fixture.module = Take(fixture.host.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
    Accept(fixture.owner->ResumeAfterReload(fixture.module->Registry(), reload));
    actual = fixture.FieldsAt();
    CHECK(actual[0].value == 43);
    CHECK(actual[1].value == 44);
    lifecycle = Take(fixture.owner->CaptureLifecycle(Map(*fixture.scenes.ActiveScene())));
    CHECK(lifecycle.instances[0].flags.enabled);
    CHECK_FALSE(lifecycle.instances[1].flags.enabled);
}

namespace {
    Result<RuntimeSaveSnapshot> CaptureAfterKnownRequirements(Fixture &fixture, std::size_t &writerInvocations) {
        auto requirements = fixture.owner->ValidateKnownNoServiceRestoreRequirements();
        if (requirements.HasError())
            return Result<RuntimeSaveSnapshot>::Failure(requirements.ErrorValue());
        ++writerInvocations;
        return Result<RuntimeSaveSnapshot>::Success(fixture.Capture());
    }
}  // namespace

TEST_CASE("Known no-service restore admission is read-only before the actual native capture writer",
          "[gameplay][save][restore][native][admission]") {
    Fixture fixture;
    fixture.FieldsAt(true);
    const auto created = fixture.native.control.behaviorCreated;
    const auto enabled = fixture.native.control.behaviorEnabled;
    std::size_t writers{};
    auto captured = CaptureAfterKnownRequirements(fixture, writers);
    REQUIRE(captured.HasValue());
    CHECK(writers == 1);
    CHECK(fixture.native.control.behaviorCaptured == 1);
    CHECK(fixture.native.control.behaviorCreated == created);
    CHECK(fixture.native.control.behaviorEnabled == enabled);
    CHECK(fixture.native.control.behaviorStarted == 0);
    CHECK(fixture.native.control.behaviorPrehooks == 0);
    CHECK(fixture.native.control.behaviorMaterialized == 0);
    CHECK(fixture.native.control.behaviorFixed == 0);
    CHECK(fixture.FieldsAt()[0].value == 43);
}

TEST_CASE("Known unavailable or unknown candidate services deny Save before writer or restore callbacks",
          "[gameplay][save][restore][native][admission]") {
    for (unsigned required : {1U, 2U, 4U, 8U, 16U}) {
        Fixture fixture;
        fixture.FieldsAt(true);
        fixture.native.control.restoreRequiredServices = required;
        std::size_t writers{};
        auto captured = CaptureAfterKnownRequirements(fixture, writers);
        REQUIRE(captured.HasError());
        CHECK(writers == 0);
        CHECK(captured.ErrorValue().code.Value() == SaveErrors::RestoreAdapterContractInvalid.code.Value());
        CHECK(fixture.native.control.behaviorCaptured == 0);
        CHECK(fixture.native.control.behaviorPrehooks == 0);
        CHECK(fixture.native.control.behaviorMaterialized == 0);
        CHECK(fixture.native.control.behaviorFixed == 0);
        CHECK(fixture.FieldsAt()[0].value == 43);
    }
}

TEST_CASE("Actual active native object without a restore prehook denies Save before writer",
          "[gameplay][save][restore][native][admission]") {
    Fixture fixture;
    RuntimeComponentSet components;
    components.behaviors.emplace_back(BehaviorInstanceId{1}, Take(BehaviorTypeId::Parse("game.tests.prefab_spawner")), 1, true,
                                      std::vector<BehaviorField>{});
    SceneDefinitionBuilder builder{{3}, {2}};
    builder.Add({{1}, std::nullopt, {}, std::nullopt, std::move(components)});
    Accept(fixture.scenes.QueuePreparation(Take(std::move(builder).Build())));
    fixture.Pump();
    Accept(fixture.owner->ActivatePublished());
    std::size_t writers{};
    auto captured = CaptureAfterKnownRequirements(fixture, writers);
    REQUIRE(captured.HasError());
    CHECK(writers == 0);
    CHECK(captured.ErrorValue().code.Value() == SaveErrors::RestoreAdapterContractInvalid.code.Value());
    CHECK(fixture.native.control.behaviorCaptured == 0);
    CHECK(fixture.native.control.behaviorPrehooks == 0);
    CHECK(fixture.native.control.behaviorMaterialized == 0);
    CHECK(fixture.native.control.behaviorFixed == 0);
}

TEST_CASE("Known requirement success does not attest future native result capabilities", "[gameplay][save][restore][native][admission]") {
    for (unsigned missing : {7U, 9U}) {
        Fixture fixture;
        fixture.native.control.restoreFailure = missing;
        std::size_t writers{};
        auto captured = CaptureAfterKnownRequirements(fixture, writers);
        REQUIRE(captured.HasValue());
        CHECK(writers == 1);
        const auto old = fixture.scenes.ActiveScene()->RuntimeId();
        auto queued = fixture.Queue(std::move(captured).Value());
        if (queued.HasValue())
            fixture.Pump();
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
        CHECK(fixture.operation.Snapshot()->IsTerminal());
        CHECK(fixture.native.control.behaviorMaterialized == 0);
    }
}

namespace {
    class RejectLifecycleWrites final : public ICanonicalCaptureSink {
    public:
        std::size_t writes{};

        Result<void> WriteCopied(SaveRecordId, std::span<const std::byte>) override {
            ++writes;
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));
        }

        Result<void> WriteImmutable(SaveRecordId, std::shared_ptr<const IImmutableCanonicalPayload>) override {
            ++writes;
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));
        }
    };
}  // namespace

TEST_CASE("Actual lifecycle capture honors a tiny admitted record cap as a wire budget rather than invalid policy",
          "[gameplay][save][restore][native][budget]") {
    Fixture fixture;
    CanonicalStateParticipantRegistry registry;
    static_cast<void>(Take(registry.Register(BehaviorLifecycleParticipant(), fixture.lifecycle)));
    const auto participants = Take(registry.Snapshot());
    const auto scene = *fixture.scenes.ActiveScene();
    RuntimeSaveCaptureLimits limits;
    limits.maximumParticipants = 1;
    limits.maximumRecords = 1;
    limits.maximumSegments = 1;
    limits.maximumPayloadBytes = 64;
    limits.maximumCopiedRecordBytes = 64;
    auto builder = Take(RuntimeSaveCaptureBuilder::Create({.capturedState = IdValue<CapturedStateId>(9),
                                                           .epoch = {1},
                                                           .sceneIncarnation = scene.RuntimeId().value,
                                                           .sceneRevision = scene.DefinitionRevision().value,
                                                           .registryGeneration = participants.Generation()},
                                                          participants, limits));
    const auto captured = builder.CaptureParticipants();
    REQUIRE(captured.HasError());
    CHECK(captured.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
    CHECK(fixture.native.control.behaviorCaptured == 0);
}

TEST_CASE("Exhausted lifecycle admission rejects before identity callbacks or sink writes", "[gameplay][save][restore][native][budget]") {
    Fixture fixture;
    std::size_t identityCalls{};
    auto adapter = Take(BehaviorLifecyclePersistenceAdapter::Create(fixture.owner, [&] {
        ++identityCalls;
        return Result<PersistentEntityIdentityMap>::Success(Map(*fixture.scenes.ActiveScene()));
    }));
    const auto descriptor = BehaviorLifecycleParticipant();
    for (unsigned exhausted = 0; exhausted < 6; ++exhausted) {
        CanonicalCaptureContext context{.provenance = {.capturedState = IdValue<CapturedStateId>(9),
                                                       .epoch = {1},
                                                       .sceneIncarnation = fixture.scenes.ActiveScene()->RuntimeId().value,
                                                       .sceneRevision = fixture.scenes.ActiveScene()->DefinitionRevision().value,
                                                       .registryGeneration = fixture.participants.Generation()},
                                        .participant = descriptor.participant,
                                        .schemaVersion = descriptor.schemaVersion,
                                        .scope = descriptor.scope,
                                        .admission = {.operationRecords = 1,
                                                      .operationSegments = 1,
                                                      .operationPayloadBytes = 1024,
                                                      .participantRecords = 1,
                                                      .participantPayloadBytes = 1024,
                                                      .maximumCopiedRecordBytes = 1024}};
        if (exhausted == 0)
            context.admission.operationRecords = 0;
        if (exhausted == 1)
            context.admission.operationSegments = 0;
        if (exhausted == 2)
            context.admission.operationPayloadBytes = 0;
        if (exhausted == 3)
            context.admission.participantRecords = 0;
        if (exhausted == 4)
            context.admission.participantPayloadBytes = 0;
        if (exhausted == 5)
            context.admission.maximumCopiedRecordBytes = 0;
        RejectLifecycleWrites sink;
        const auto captured = adapter->Capture(context, sink);
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == SaveErrors::CaptureBudgetExceeded.code.Value());
        CHECK(sink.writes == 0);
    }
    CHECK(identityCalls == 0);
}
