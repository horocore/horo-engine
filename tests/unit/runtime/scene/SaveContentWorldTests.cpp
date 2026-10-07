#include "../../../support/AllocationProbe.h"
#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "SaveContentWorldTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <future>

using namespace Horo;
using namespace Horo::Runtime;

using namespace Horo::Runtime::SceneContentWorldTest;

TEST_CASE("Content-bound real Scene publication and safe-point captures recompute schema2 state and survive live retirement",
          "[scene][save][content]") {
    WorldFixture fixture;
    auto first = fixture.Capture();
    auto written = first.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>());
    REQUIRE(written.HasValue());
    auto admitted = SaveArchiveReader{}.Read(written.Value().Archive().bytes);
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().Manifest().saveSchemaVersion.Value() == 2);
    REQUIRE(ValidateSaveSceneCanonicalArchive(admitted.Value()).HasValue());
    CHECK(fixture.adapter->calls == 1);
    fixture.adapter->bytes[0] = std::byte{9};
    auto second = fixture.Capture(13);
    auto changed = second.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>());
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().Summary().canonicalState != written.Value().Summary().canonicalState);
    fixture.service->Shutdown();
    fixture.source.installed->Close();
    auto worker = std::async(std::launch::async, [first, header = fixture.Header()] {
        return first.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>());
    });
    auto retired = worker.get();
    REQUIRE(retired.HasValue());
    CHECK(retired.Value().Summary().canonicalState == written.Value().Summary().canonicalState);
    CHECK(fixture.adapter->calls == 2);
}

TEST_CASE("Content declaration rejects generic capture and wrong world or tiny work budgets without invoking additional adapters",
          "[scene][save][content]") {
    WorldFixture fixture;
    const RuntimeSaveCaptureProvenance provenance{SceneTest::Id<CapturedStateId>(20),
                                                  {.value = 41},
                                                  fixture.generation.scene,
                                                  fixture.service->ActiveScene()->StructuralRevision(),
                                                  fixture.participants.Generation()};
    auto builder = RuntimeSaveCaptureBuilder::Create(provenance, fixture.participants);
    REQUIRE(builder.HasValue());
    CHECK(std::move(builder).Value().CaptureParticipants().HasError());
    CHECK(fixture.adapter->calls == 0);
    auto capture = fixture.Capture();
    auto header = fixture.Header();
    header.world = SceneTest::Id<SaveWorldId>(99);
    CHECK(capture.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>()).HasError());
    SaveArchiveReaderLimits tight;
    tight.maximumReadWorkBytes = 1;
    CHECK(capture.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>(), tight).HasError());
    tight = {};
    tight.maximumDecodedBytes = 1;
    CHECK(capture.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>(), tight).HasError());
    CHECK(fixture.adapter->calls == 1);
}

TEST_CASE("Retired or replaced live world cannot admit another callback while accepted snapshot remains detached",
          "[scene][save][content]") {
    WorldFixture fixture;
    auto accepted = fixture.Capture();
    const auto header = fixture.Header();
    SECTION("revoked installation") {
        fixture.source.installed->Close();
    }
    SECTION("ordinary replacement with the same source definition") {
        REQUIRE(fixture.service->QueuePreparation(SceneContentTest::Definition()).HasValue());
        FrameContext context{2, {}, 0.0, 0, {}, false, fixture.source.cancellation.Token()};
        REQUIRE(fixture.service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
    }
    REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
    auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                      SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                .generation = fixture.generation,
                                                                                .capturedState = SceneTest::Id<CapturedStateId>(22),
                                                                                .epoch = {.value = 41}},
                                                      fixture.participants, SaveDegradedWorldPolicy::Reject);
    CHECK(rejected.HasError());
    CHECK(fixture.adapter->calls == 1);
    auto worker = std::async(std::launch::async, [accepted, header] {
        return accepted.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>());
    });
    CHECK(worker.get().HasValue());
    CHECK(fixture.adapter->calls == 1);
}

namespace {
    thread_local std::size_t captureFailureAllocationCount{};

    /** @brief Rejects the missing declared owner before callbacks and reconciles the terminal barrier request. */
    void RequireMissingCurrentOwner(WorldFixture &fixture) {
        CanonicalStateParticipantRegistry incomplete;
        const auto *declaration = fixture.participants.Find(SaveContentRequirementsParticipant());
        REQUIRE(declaration);
        REQUIRE(incomplete.Register(declaration->Descriptor(), declaration->Adapter()).HasValue());
        auto snapshot = incomplete.Snapshot();
        REQUIRE(snapshot.HasValue());
        auto generation = fixture.generation;
        generation.registry = snapshot.Value().Generation();
        REQUIRE(fixture.barrier->Request(91, generation).HasValue());
        auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                          SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                    .generation = generation,
                                                                                    .capturedState = SceneTest::Id<CapturedStateId>(33),
                                                                                    .epoch = {.value = 41}},
                                                          std::move(snapshot).Value(), SaveDegradedWorldPolicy::Reject);
        CHECK(rejected.HasError());
        CHECK(fixture.adapter->calls == 0);
        REQUIRE(fixture.barrier->Cancel(91).HasValue());
        REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
    }

}  // namespace

namespace {
    /** @brief Proves owned fallback preparation may throw before changing admission, callbacks or the published root. */
    void RequireCapturePreflightFailure(WorldFixture &fixture, const SceneRuntimeId scene) {
        bool preflightThrew{};
        {
            Horo::Tests::AllocationProbe::ScopedFailure fail;
            try {
                (void)fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                        SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                  .generation = fixture.generation,
                                                                                  .capturedState = SceneTest::Id<CapturedStateId>(32),
                                                                                  .epoch = {.value = 41}},
                                                        fixture.participants, SaveDegradedWorldPolicy::Reject);
            } catch (const std::bad_alloc &) {
                preflightThrew = true;
            }
        }
        CHECK(preflightThrew);
        CHECK(fixture.adapter->calls == 0);
        CHECK(fixture.service->ActiveScene()->RuntimeId() == scene);
    }
}  // namespace

TEST_CASE("Capture preparation allocation failure preserves the published world and runs no participant callbacks",
          "[scene][save][content][allocation]") {
    WorldFixture fixture;
    const auto scene = fixture.service->ActiveScene()->RuntimeId();
    REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
    RequireCapturePreflightFailure(fixture, scene);
    std::size_t fallbackRequests{};
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        auto fallback = Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        fallbackRequests = measurement.Snapshot().requests;
        REQUIRE(fallback.HasError());
    }
    REQUIRE(fallbackRequests > 0);
    bool rejected{};
    bool typedFailure{};
    std::size_t allocationsAfterFailure{};
    {
        Horo::Tests::AllocationProbe::ScopedFailure fail{fallbackRequests, [](std::size_t) noexcept {
            captureFailureAllocationCount = Horo::Tests::AllocationProbe::Count();
        }};
        auto attempt = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                         SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                   .generation = fixture.generation,
                                                                                   .capturedState = SceneTest::Id<CapturedStateId>(32),
                                                                                   .epoch = {.value = 41}},
                                                         fixture.participants, SaveDegradedWorldPolicy::Reject);
        rejected = attempt.HasError();
        typedFailure = rejected && attempt.ErrorValue().code.Value() == SaveErrors::CanonicalCodecAllocationFailed.code.Value();
        allocationsAfterFailure = Horo::Tests::AllocationProbe::Count();
    }
    CHECK(rejected);
    CHECK(typedFailure);
    CHECK(allocationsAfterFailure == captureFailureAllocationCount);
    CHECK(fixture.adapter->calls == 0);
    REQUIRE(fixture.service->ActiveScene());
    CHECK(fixture.service->ActiveScene()->RuntimeId() == scene);
    auto recovered = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                       SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                 .generation = fixture.generation,
                                                                                 .capturedState = SceneTest::Id<CapturedStateId>(32),
                                                                                 .epoch = {.value = 41}},
                                                       fixture.participants, SaveDegradedWorldPolicy::Reject);
    REQUIRE(recovered.HasValue());
    REQUIRE(recovered.Value().capture);
    CHECK(fixture.adapter->calls == 1);
    CHECK(recovered.Value().capture->ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>()).HasValue());
    REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
}

TEST_CASE("Declared current owner cannot disappear from a recaptured manifest and valid retry preserves the last good world",
          "[scene][save][content]") {
    WorldFixture original;
    auto captured = original.Capture();
    auto written = captured.ReSave(original.Header(), SceneContentTest::V<ArchiveFormatVersion>());
    REQUIRE(written.HasValue());
    auto policy = original.source.policy;
    policy.compatibility.saveSchemaVersions.direct = {SceneContentTest::V<SaveSchemaVersion>(2), SceneContentTest::V<SaveSchemaVersion>(2)};
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    policy.compatibility.participants = {{SaveParticipantId::Parse("project.state").Value(), {{schema, schema}, {}}, true, {}}};
    WorldFixture fixture{{}, {}, written.Value().Archive(), policy};
    const auto scene = fixture.service->ActiveScene()->RuntimeId();
    SECTION("required current owner absent from registry") {
        RequireMissingCurrentOwner(fixture);
    }
    SECTION("required owner callback skips its state") {
        fixture.adapter->skip = true;
        REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
        auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                          SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                    .generation = fixture.generation,
                                                                                    .capturedState = SceneTest::Id<CapturedStateId>(33),
                                                                                    .epoch = {.value = 41}},
                                                          fixture.participants, SaveDegradedWorldPolicy::Reject);
        REQUIRE(rejected.HasValue());
        CHECK_FALSE(rejected.Value().capture);
        CHECK(rejected.Value().error.has_value());
        CHECK(fixture.adapter->calls == 1);
        REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
        fixture.adapter->skip = false;
    }
    REQUIRE(fixture.service->ActiveScene());
    CHECK(fixture.service->ActiveScene()->RuntimeId() == scene);
    auto retry = fixture.Capture(34);
    CHECK(retry.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>()).HasValue());
}

TEST_CASE("Cooked baseline substitution preserves logical declaration and sealed header across capture and next source admission",
          "[scene][save][content][substitution]") {
    const auto replacement = Assets::AssetId::FromBytes(SceneTest::Id<SaveBaseSceneId>(77).Bytes());
    SceneContentTest::Fixture substituted{false, SceneContentTest::SceneType(), {7, 3}, replacement};
    WorldFixture fixture{{}, {}, substituted.archive, substituted.policy, substituted.provider};
    CHECK(fixture.source.decoder.observed == replacement);
    REQUIRE(fixture.world->Diagnostics().size() == 1);
    CHECK(fixture.world->Diagnostics().front().remedy == SaveContentRemedy::ReviewApprovedSubstitution);
    auto captured = fixture.Capture(12, SaveDegradedWorldPolicy::PreserveOpaque);
    REQUIRE(captured.Diagnostics().size() == 1);
    CHECK(captured.Diagnostics().front().replacement == replacement);
    auto written = captured.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>());
    REQUIRE(written.HasValue());
    auto readback = SaveArchiveReader{}.Read(written.Value().Archive().bytes);
    REQUIRE(readback.HasValue());
    CHECK(readback.Value().Header().baseScene == fixture.source.descriptor.baseScene);
    auto bytes = readback.Value().SelectChunk(SaveContentRequirementsRecord());
    REQUIRE(bytes.HasValue());
    REQUIRE(bytes.Value());
    auto declarations = DecodeSaveContentRequirements(*bytes.Value());
    REQUIRE(declarations.HasValue());
    REQUIRE(declarations.Value().size() == 1);
    const auto &logical = std::get<SaveAssetContentRequirement>(declarations.Value().front().content);
    CHECK(logical.asset == Assets::AssetId::FromBytes(fixture.source.descriptor.baseScene.Bytes()));
    CHECK(logical.envelopeDigest == fixture.source.descriptor.contentDigest);
    auto policy = fixture.source.policy;
    policy.compatibility.saveSchemaVersions.direct = {SceneContentTest::V<SaveSchemaVersion>(2), SceneContentTest::V<SaveSchemaVersion>(2)};
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    policy.compatibility.participants = {{SaveParticipantId::Parse("project.state").Value(), {{schema, schema}, {}}, true, {}}};
    auto proof =
        ReconciledSaveContent::Prepare(*fixture.source.installed, written.Value().Archive(), policy, fixture.source.cancellation.Token());
    REQUIRE(proof.HasValue());
    REQUIRE(proof.Value().Diagnostics().size() == 1);
    CHECK(proof.Value().Diagnostics().front().disposition == SaveContentDisposition::Substituted);
    CHECK(proof.Value().Diagnostics().front().replacement == replacement);
    auto next = PrepareSavedSceneBootstrap(fixture.source.descriptor, SceneContentTest::SceneType(), std::move(proof).Value(),
                                           &fixture.source.decoder);
    REQUIRE(next.HasValue());
    CHECK(fixture.source.decoder.observed == replacement);
}
