#include "../../../support/AllocationProbe.h"
#include "SaveContentRetentionTestHelpers.h"

#include <catch2/generators/catch_generators.hpp>
#include <future>

using namespace Horo;
using namespace Horo::Runtime;
using namespace Horo::Runtime::SceneContentWorldTest;
using namespace Horo::Runtime::SceneContentRetentionTest;

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

namespace {
    /** @brief Verifies exact retained storage and the next source's independently re-admitted project disposition. */
    SaveContentDisposition RequireRetainedNextAdmission(const FinalizedSaveArchive &written, const RetainedFixture &retained,
                                                        WorldFixture &fixture) {
        auto readback = SaveArchiveReader{}.Read(written.Archive().bytes);
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
            ReconciledSaveContent::Prepare(*fixture.source.installed, written.Archive(), policy, fixture.source.cancellation.Token());
        REQUIRE(next.HasValue());
        CHECK(next.Value().IsDegraded());
        const auto nextDecision = std::ranges::find(next.Value().Diagnostics(), retained.owner, [](const SaveContentDiagnostic &value) {
            return value.requirement.owner;
        });
        REQUIRE(nextDecision != next.Value().Diagnostics().end());
        CHECK(nextDecision->remedy == SaveContentRemedy::PreserveOpaqueData);
        return nextDecision->disposition;
    }
}  // namespace

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
    auto denied = fixture.world->CaptureAtSafePoint(*fixture.barrier,
                                                    SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                              .generation = fixture.generation,
                                                                              .capturedState = SceneTest::Id<CapturedStateId>(35),
                                                                              .epoch = {.value = 41}},
                                                    fixture.participants, SaveDegradedWorldPolicy::Reject);
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
    const auto nextDisposition = RequireRetainedNextAdmission(written.Value(), retained, fixture);
    CHECK(nextDisposition == decision->disposition);
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
