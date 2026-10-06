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
    auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                      SceneTest::Id<CapturedStateId>(22), {.value = 41}, fixture.participants,
                                                      SaveDegradedWorldPolicy::Reject);
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

    struct RetainedFixture final {
        SceneContentTest::Fixture source;
        ImmutableSaveArchive archive;
        SaveContentProjectPolicy policy;
        SaveParticipantId owner = SaveParticipantId::Parse("project.optional").Value();
        SaveRecordId record = SceneTest::Id<SaveRecordId>(31);
        std::uint64_t decodedBytes{};
        std::vector<std::byte> stored;

        RetainedFixture(SaveSceneCanonicalRepresentation representation, SaveChunkCodec codec,
                        std::optional<SaveContentNecessity> declaredDlc = {}) {
            auto original = SaveArchiveReader{}.Read(source.archive.bytes);
            REQUIRE(original.HasValue());
            const auto header = original.Value().Header();
            auto declaration = original.Value().SelectChunk(SaveContentRequirementsRecord());
            REQUIRE(declaration.HasValue());
            REQUIRE(declaration.Value());
            auto declarationBytes = std::move(*declaration.Value());
            if (declaredDlc) {
                auto decodedRequirements = DecodeSaveContentRequirements(declarationBytes);
                REQUIRE(decodedRequirements.HasValue());
                auto requirements = std::move(decodedRequirements).Value();
                requirements.push_back(
                    {owner, *declaredDlc,
                     SaveChunkContentRequirement{Assets::AssetChunkId::Parse("missing_dlc").Value(), Assets::AssetChunkKind::Dlc}});
                auto encodedRequirements = EncodeSaveContentRequirements(requirements);
                REQUIRE(encodedRequirements.HasValue());
                declarationBytes.assign(encodedRequirements.Value().Bytes().begin(), encodedRequirements.Value().Bytes().end());
            }
            std::vector<std::byte> decoded(65'536);
            std::uint32_t state = 17;
            for (std::size_t index = 0; index < 4096; ++index) {
                state = state * 1664525U + 1013904223U;
                decoded[index] = static_cast<std::byte>(state >> 24U);
            }
            for (std::size_t index = 4096; index < decoded.size(); ++index)
                decoded[index] = decoded[index % 4096];
            CanonicalValueWriter payloadWriter;
            REQUIRE(payloadWriter.WriteBytes(decoded).HasValue());
            auto payload = std::move(payloadWriter).Finalize();
            REQUIRE(payload.HasValue());
            decoded.assign(payload.Value().Bytes().begin(), payload.Value().Bytes().end());
            decodedBytes = decoded.size();
            const auto digest = ComputeSha256(decoded);
            if (codec == SaveChunkCodec::Deflate) {
                auto compressed = EncodeSaveChunk(decoded, {.preferred = codec, .required = true}, {});
                REQUIRE(compressed.HasValue());
                stored = std::move(compressed).Value().stored;
                REQUIRE(stored.size() < decoded.size());
            } else {
                stored = decoded;
            }
            std::vector tags{SaveSceneCanonicalLayoutEntry{SaveContentRequirementsRecord(), SaveSceneCanonicalRepresentation::Known},
                             SaveSceneCanonicalLayoutEntry{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known},
                             SaveSceneCanonicalLayoutEntry{record, representation}};
            std::ranges::sort(tags, {}, &SaveSceneCanonicalLayoutEntry::record);
            auto layout = EncodeSaveSceneCanonicalLayout(tags);
            REQUIRE(layout.HasValue());
            std::vector<std::byte> layoutBytes(layout.Value().Bytes().begin(), layout.Value().Bytes().end());
            const auto version = SceneContentTest::V<ParticipantSchemaVersion>();
            const std::array declarationRecords{
                SaveSceneCanonicalRecord{SaveContentRequirementsRecord(), std::span<const std::byte>{declarationBytes}}};
            const std::array layoutRecords{
                SaveSceneCanonicalRecord{SaveSceneCanonicalLayoutRecord(), std::span<const std::byte>{layoutBytes}}};
            const SaveSceneCanonicalOpaque opaque{codec, stored.size(), decoded.size(), 1, digest, stored};
            const std::array dataRecords{
                SaveSceneCanonicalRecord{record, representation == SaveSceneCanonicalRepresentation::Known
                                                     ? decltype(SaveSceneCanonicalRecord::payload){std::span<const std::byte>{decoded}}
                                                     : decltype(SaveSceneCanonicalRecord::payload){opaque}}};
            const std::array owners{SaveSceneCanonicalParticipant{SaveContentRequirementsParticipant(), version, declarationRecords},
                                    SaveSceneCanonicalParticipant{SaveSceneCanonicalLayoutParticipant(), version, layoutRecords},
                                    SaveSceneCanonicalParticipant{owner, version, dataRecords}};
            auto canonical = EncodeSaveSceneCanonicalState(header, owners);
            REQUIRE(canonical.HasValue());
            const SaveGameManifest manifest{SceneContentTest::V<SaveSchemaVersion>(2),
                                            ComputeCanonicalStateHash(canonical.Value().Bytes()),
                                            {{SaveContentRequirementsParticipant(), version, true, {SaveContentRequirementsRecord()}},
                                             {SaveSceneCanonicalLayoutParticipant(), version, true, {SaveSceneCanonicalLayoutRecord()}},
                                             {owner, version, false, {record}}}};
            std::array chunks{PreservedSaveChunk{{SaveContentRequirementsRecord(), SaveContentRequirementsParticipant(), 0,
                                                  declarationBytes.size(), declarationBytes.size(), 1, SaveChunkCodec::Raw,
                                                  ComputeSha256(declarationBytes)},
                                                 declarationBytes},
                              PreservedSaveChunk{{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalLayoutParticipant(), 0,
                                                  layoutBytes.size(), layoutBytes.size(), 1, SaveChunkCodec::Raw,
                                                  ComputeSha256(layoutBytes)},
                                                 layoutBytes},
                              PreservedSaveChunk{{record, owner, 0, stored.size(), decoded.size(), 1, codec, digest}, stored}};
            std::ranges::sort(chunks, [](const PreservedSaveChunk &left, const PreservedSaveChunk &right) {
                return left.entry.record < right.entry.record;
            });
            auto written = SaveArchiveContainerWriter::Write(header, manifest, chunks, SceneContentTest::V<ArchiveFormatVersion>(2));
            if (written.HasError()) {
                INFO("retained fixture codec=" << static_cast<unsigned>(codec) << " stored=" << stored.size()
                                               << " decoded=" << decoded.size() << " error=" << written.ErrorValue().code.Value());
                std::string diagnostics;
                for (const auto &diagnostic : written.ErrorValue().diagnostics)
                    diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
                INFO("writer/reader diagnostics: " << diagnostics);
                REQUIRE(written.HasValue());
            }
            REQUIRE(written.HasValue());
            archive = written.Value().Archive();
            policy = source.policy;
            policy.compatibility.archiveVersions.direct.maximum = SceneContentTest::V<ArchiveFormatVersion>(2);
            policy.compatibility.saveSchemaVersions.direct = {SceneContentTest::V<SaveSchemaVersion>(2),
                                                              SceneContentTest::V<SaveSchemaVersion>(2)};
        }
    };
}  // namespace

TEST_CASE("Compressed known retention rejects new decoded limit before decompression and admits its exact aggregate boundary",
          "[scene][save][content][allocation]") {
    RetainedFixture retained{SaveSceneCanonicalRepresentation::Known, SaveChunkCodec::Deflate};
    WorldFixture fixture{{}, {}, retained.archive, retained.policy};
    auto accepted = fixture.Capture(12, SaveDegradedWorldPolicy::PreserveOpaque);
    auto header = fixture.Header();
    SaveArchiveReaderLimits tight;
    tight.maximumDecodedBytes = retained.decodedBytes - 1;
    tight.chunks.maximumDecodedChunkBytes = tight.maximumDecodedBytes;
    Horo::Tests::AllocationProbe::Measurement measured;
    bool rejected{};
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement observe;
        rejected = accepted.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>(2), tight).HasError();
        measured = observe.Snapshot();
    }
    CHECK(rejected);
    CHECK(measured.largestRequest < retained.decodedBytes);
    auto written = accepted.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>(2));
    REQUIRE(written.HasValue());
    auto readback = SaveArchiveReader{}.Read(written.Value().Archive().bytes);
    REQUIRE(readback.HasValue());
    auto encodedHeader = EncodeSaveArchiveHeader(readback.Value().Header());
    auto encodedManifest = EncodeSaveGameManifest(readback.Value().Manifest());
    REQUIRE(encodedHeader.HasValue());
    REQUIRE(encodedManifest.HasValue());
    tight.maximumDecodedBytes = encodedHeader.Value().size() + encodedManifest.Value().size();
    for (const auto &entry : readback.Value().Directory().Entries())
        tight.maximumDecodedBytes += entry.decodedByteLength;
    tight.chunks.maximumDecodedChunkBytes = retained.decodedBytes;
    REQUIRE(tight.chunks.maximumDecodedChunkBytes <= tight.maximumDecodedBytes);
    auto boundary = accepted.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>(2), tight);
    if (boundary.HasError()) {
        INFO("exact decoded boundary=" << tight.maximumDecodedBytes << " error=" << boundary.ErrorValue().code.Value() << ": "
                                       << boundary.ErrorValue().message);
        std::string diagnostics;
        for (const auto &diagnostic : boundary.ErrorValue().diagnostics)
            diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
        INFO("boundary diagnostics: " << diagnostics);
        CHECK(boundary.HasValue());
    }
    CHECK(boundary.HasValue());
}

TEST_CASE("Opaque codec retention survives a release recognizing the owner without interpreting or changing stored frames",
          "[scene][save][content]") {
    RetainedFixture retained{SaveSceneCanonicalRepresentation::Opaque, static_cast<SaveChunkCodec>(99)};
    // The upgraded release knows this owner; authenticated source layout still requires byte-only opaque retention.
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    retained.policy.compatibility.participants = {{retained.owner, {{schema, schema}, {}}, false, {}}};
    WorldFixture fixture{{}, {}, retained.archive, retained.policy};
    auto accepted = fixture.Capture(12, SaveDegradedWorldPolicy::PreserveOpaque);
    auto written = accepted.ReSave(fixture.Header(), SceneContentTest::V<ArchiveFormatVersion>(2));
    REQUIRE(written.HasValue());
    auto readback = SaveArchiveReader{}.Read(written.Value().Archive().bytes);
    REQUIRE(readback.HasValue());
    auto tags = ValidateSaveSceneCanonicalArchive(readback.Value());
    REQUIRE(tags.HasValue());
    const auto tag = std::ranges::find(tags.Value(), retained.record, &SaveSceneCanonicalLayoutEntry::record);
    REQUIRE(tag != tags.Value().end());
    CHECK(tag->representation == SaveSceneCanonicalRepresentation::Opaque);
    const auto entry = std::ranges::find(readback.Value().Directory().Entries(), retained.record, &SaveChunkDirectoryEntry::record);
    REQUIRE(entry != readback.Value().Directory().Entries().end());
    CHECK(entry->codec == static_cast<SaveChunkCodec>(99));
    const auto stored =
        readback.Value().Payload().subspan(static_cast<std::size_t>(entry->offset), static_cast<std::size_t>(entry->storedByteLength));
    CHECK(std::ranges::equal(stored, retained.stored));
    CHECK(entry->decodedByteLength == retained.decodedBytes);
    CHECK(readback.Value().SelectChunk(retained.record).HasError());
}

TEST_CASE("Canonical preservation authority pins owned opaque bytes and rejects borrowed or moved authority",
          "[scene][save][content][lifetime]") {
    RetainedFixture retained{SaveSceneCanonicalRepresentation::Opaque, static_cast<SaveChunkCodec>(99)};
    auto owned = std::make_shared<const std::vector<std::byte>>(*retained.archive.bytes);
    auto read = SaveArchiveReader{}.Read(owned);
    REQUIRE(read.HasValue());
    auto borrowed = SaveArchiveReader{}.Read(std::span<const std::byte>{*owned});
    REQUIRE(borrowed.HasValue());
    CHECK(ValidatedSaveSceneCanonicalPreservation::Create(borrowed.Value()).HasError());
    auto proof = [&] {
        auto created = ValidatedSaveSceneCanonicalPreservation::Create(read.Value());
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }();
    read = Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::OperationCancelled));
    owned.reset();
    auto policy = retained.policy.compatibility;
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    policy.participants = {{SaveContentRequirementsParticipant(), {{schema, schema}, {}}, true, {}},
                           {SaveSceneCanonicalLayoutParticipant(), {{schema, schema}, {}}, true, {}},
                           {retained.owner, {{schema, schema}, {}}, false, {}}};
    std::ranges::sort(policy.participants, {}, &SaveParticipantCompatibility::participant);
    auto moved = std::move(proof);
    CHECK(proof.InspectUnknownData(policy, retained.stored.size()).HasError());
    auto preserved = moved.InspectUnknownData(policy, retained.stored.size());
    REQUIRE(preserved.HasValue());
    REQUIRE(preserved.Value().preserved.size() == 1);
    CHECK(preserved.Value().preserved.front().storedBytes == retained.stored);
    CHECK(moved.InspectUnknownData(policy, retained.stored.size() - 1).HasError());
    policy.droppableUnknownParticipants.push_back(retained.owner);
    CHECK(moved.InspectUnknownData(policy, retained.stored.size()).HasError());
}

TEST_CASE("Capture preparation allocation failure preserves the published world and runs no participant callbacks",
          "[scene][save][content][allocation]") {
    WorldFixture fixture;
    const auto scene = fixture.service->ActiveScene()->RuntimeId();
    REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
    bool preflightThrew{};
    {
        Horo::Tests::AllocationProbe::ScopedFailure fail;
        try {
            (void)fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                    SceneTest::Id<CapturedStateId>(32), {.value = 41}, fixture.participants,
                                                    SaveDegradedWorldPolicy::Reject);
        } catch (const std::bad_alloc &) {
            preflightThrew = true;
        }
    }
    CHECK(preflightThrew);
    CHECK(fixture.adapter->calls == 0);
    CHECK(fixture.service->ActiveScene()->RuntimeId() == scene);
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
        auto attempt = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                         SceneTest::Id<CapturedStateId>(32), {.value = 41}, fixture.participants,
                                                         SaveDegradedWorldPolicy::Reject);
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
    auto recovered = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                       SceneTest::Id<CapturedStateId>(32), {.value = 41}, fixture.participants,
                                                       SaveDegradedWorldPolicy::Reject);
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
        CanonicalStateParticipantRegistry incomplete;
        const auto *declaration = fixture.participants.Find(SaveContentRequirementsParticipant());
        REQUIRE(declaration);
        REQUIRE(incomplete.Register(declaration->Descriptor(), declaration->Adapter()).HasValue());
        auto snapshot = incomplete.Snapshot();
        REQUIRE(snapshot.HasValue());
        auto generation = fixture.generation;
        generation.registry = snapshot.Value().Generation();
        REQUIRE(fixture.barrier->Request(91, generation).HasValue());
        auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, generation,
                                                          SceneTest::Id<CapturedStateId>(33), {.value = 41}, std::move(snapshot).Value(),
                                                          SaveDegradedWorldPolicy::Reject);
        CHECK(rejected.HasError());
        CHECK(fixture.adapter->calls == 0);
        REQUIRE(fixture.barrier->Cancel(91).HasValue());
        REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
    }
    SECTION("required owner callback skips its state") {
        fixture.adapter->skip = true;
        REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
        auto rejected = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges,
                                                          fixture.generation, SceneTest::Id<CapturedStateId>(33), {.value = 41},
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

TEST_CASE("Missing optional DLC has explicit preserve or quarantine disposition and retains bytes across real capture and next admission",
          "[scene][save][content][dlc]") {
    RetainedFixture retained{SaveSceneCanonicalRepresentation::Opaque, SaveChunkCodec::Raw, SaveContentNecessity::Optional};
    SaveOptionalContentAbsence absence{};
    SECTION("preservable owner") {
        absence = SaveOptionalContentAbsence::Preserve;
    }
    SECTION("quarantined owner") {
        absence = SaveOptionalContentAbsence::Quarantine;
    }
    retained.policy.optionalOwners = {{retained.owner, absence}};
    auto proof =
        ReconciledSaveContent::Prepare(*retained.source.installed, retained.archive, retained.policy, retained.source.cancellation.Token());
    REQUIRE(proof.HasValue());
    const auto decision = std::ranges::find(proof.Value().Diagnostics(), retained.owner, [](const SaveContentDiagnostic &value) {
        return value.requirement.owner;
    });
    REQUIRE(decision != proof.Value().Diagnostics().end());
    CHECK(decision->disposition ==
          (absence == SaveOptionalContentAbsence::Preserve ? SaveContentDisposition::Preservable : SaveContentDisposition::Quarantined));
    WorldFixture fixture{{}, {}, retained.archive, retained.policy};
    REQUIRE(fixture.world->Diagnostics().size() == 2);
    CHECK(decision->remedy == SaveContentRemedy::PreserveOpaqueData);
    REQUIRE(fixture.barrier->Request(91, fixture.generation).HasValue());
    auto denied = fixture.world->CaptureAtSafePoint(*fixture.barrier, RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                    SceneTest::Id<CapturedStateId>(35), {.value = 41}, fixture.participants,
                                                    SaveDegradedWorldPolicy::Reject);
    CHECK(denied.HasError());
    CHECK(fixture.adapter->calls == 0);
    REQUIRE(fixture.barrier->Cancel(91).HasValue());
    REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
    auto capture = fixture.Capture(35, SaveDegradedWorldPolicy::PreserveOpaque);
    const auto header = fixture.Header();
    fixture.service->Shutdown();
    fixture.world.reset();
    auto worker = std::async(std::launch::async, [capture, header, owner = retained.owner, absence]() {
        const auto decision = std::ranges::find(capture.Diagnostics(), owner, [](const SaveContentDiagnostic &value) {
            return value.requirement.owner;
        });
        const bool retainedDecision =
            decision != capture.Diagnostics().end() && decision->remedy == SaveContentRemedy::PreserveOpaqueData &&
            decision->disposition == (absence == SaveOptionalContentAbsence::Preserve ? SaveContentDisposition::Preservable
                                                                                      : SaveContentDisposition::Quarantined);
        return std::pair{retainedDecision, capture.ReSave(header, SceneContentTest::V<ArchiveFormatVersion>(2))};
    });
    auto encoded = worker.get();
    CHECK(encoded.first);
    auto written = std::move(encoded.second);
    REQUIRE(written.HasValue());
    auto readback = SaveArchiveReader{}.Read(written.Value().Archive().bytes);
    REQUIRE(readback.HasValue());
    const auto entry = std::ranges::find(readback.Value().Directory().Entries(), retained.record, &SaveChunkDirectoryEntry::record);
    REQUIRE(entry != readback.Value().Directory().Entries().end());
    const auto stored =
        readback.Value().Payload().subspan(static_cast<std::size_t>(entry->offset), static_cast<std::size_t>(entry->storedByteLength));
    CHECK(std::ranges::equal(stored, retained.stored));
    CHECK(entry->codec == SaveChunkCodec::Raw);
    auto policy = retained.policy;
    const auto schema = SceneContentTest::V<ParticipantSchemaVersion>();
    policy.compatibility.participants = {{SaveParticipantId::Parse("project.state").Value(), {{schema, schema}, {}}, true, {}}};
    auto next =
        ReconciledSaveContent::Prepare(*fixture.source.installed, written.Value().Archive(), policy, fixture.source.cancellation.Token());
    REQUIRE(next.HasValue());
    CHECK(next.Value().IsDegraded());
    const auto nextDecision = std::ranges::find(next.Value().Diagnostics(), retained.owner, [](const SaveContentDiagnostic &value) {
        return value.requirement.owner;
    });
    REQUIRE(nextDecision != next.Value().Diagnostics().end());
    CHECK(nextDecision->remedy == SaveContentRemedy::PreserveOpaqueData);
    CHECK(nextDecision->disposition == decision->disposition);
    CHECK(fixture.adapter->calls == 1);
}

TEST_CASE("Missing required DLC and optional DLC without explicit project disposition reject before the host decoder",
          "[scene][save][content][dlc]") {
    const auto necessity = GENERATE(SaveContentNecessity::Required, SaveContentNecessity::Optional);
    RetainedFixture retained{SaveSceneCanonicalRepresentation::Opaque, SaveChunkCodec::Raw, necessity};
    if (necessity == SaveContentNecessity::Required)
        retained.policy.optionalOwners = {{retained.owner, SaveOptionalContentAbsence::Preserve}};
    retained.source.archive = retained.archive;
    retained.source.policy = retained.policy;
    auto prepared = retained.source.Prepare();
    REQUIRE(prepared.HasError());
    REQUIRE(prepared.ErrorValue().diagnostics.size() == 1);
    CHECK(prepared.ErrorValue().diagnostics.front().code.Value() == "scene.save_content.install_compatible");
    CHECK(prepared.ErrorValue().diagnostics.front().path == "contentRequirements/project.optional");
    CHECK(prepared.ErrorValue().diagnostics.front().message.find("chunk=missing_dlc") != std::string::npos);
    CHECK(prepared.ErrorValue().diagnostics.front().message.find("install compatible") != std::string::npos);
    CHECK(retained.source.decoder.calls == 0);
}
