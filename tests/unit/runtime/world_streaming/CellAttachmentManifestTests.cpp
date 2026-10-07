#include "Horo/WorldStreaming/CellAttachmentManifest.h"
#include "StreamingCellCandidateTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::WorldStreaming {
    namespace {
        CellAttachmentReference Reference(StreamingCellProvider provider = StreamingCellProvider::Terrain) {
            return {provider,
                    TestSupport::Asset(7),
                    TestSupport::IdentityFrom<CellAttachmentSubresource>(1),
                    TestSupport::IdentityFrom<CellAttachmentRevision>(1),
                    3,
                    StreamingCellPayloadRequirement::Optional,
                    CandidateTestSupport::Hash(),
                    32};
        }

        auto Manifest(std::span<const CellAttachmentReference> references, CellAttachmentManifestLimits limits = {8, 256}) {
            const auto world = CandidateTestSupport::Manifest();
            const auto candidate = CandidateTestSupport::Candidate(world);
            return CellAttachmentManifest::Create(candidate, TestSupport::IdentityFrom<CellAttachmentRevision>(1), references, limits);
        }
    }  // namespace

    TEST_CASE("Attachment manifests own exact feature references and required policy", "[world_streaming][attachment][manifest]") {
        std::array references{Reference()};
        auto manifest = Manifest(references);
        REQUIRE(manifest.HasValue());
        references[0].bytes = 99;
        REQUIRE(manifest.Value().References()[0].bytes == 32);
        REQUIRE(manifest.Value().Operation() == CandidateTestSupport::Operation());
        REQUIRE(manifest.Value().CellDigest() == CandidateTestSupport::Hash());
        REQUIRE(manifest.Value().Revision() == TestSupport::IdentityFrom<CellAttachmentRevision>(1));
        auto required = Reference();
        required.requirement = StreamingCellPayloadRequirement::Required;
        REQUIRE(Manifest(std::span{&required, 1}).HasValue());
    }

    TEST_CASE("Attachment manifests reject invalid stale duplicate and oversized references", "[world_streaming][attachment][admission]") {
        auto reference = Reference();
        reference.version = 2;
        REQUIRE(Manifest(std::span{&reference, 1}).ErrorValue().code.Value() == CellAttachmentErrors::Stale.code.Value());
        reference = Reference();
        reference.asset = {};
        REQUIRE(Manifest(std::span{&reference, 1}).ErrorValue().code.Value() == CellAttachmentErrors::Invalid.code.Value());
        reference = Reference();
        std::array duplicates{reference, reference};
        duplicates[1].revision = TestSupport::IdentityFrom<CellAttachmentRevision>(2);
        REQUIRE(Manifest(duplicates).ErrorValue().code.Value() == CellAttachmentErrors::Invalid.code.Value());
        REQUIRE(Manifest(duplicates, {1, 256}).ErrorValue().code.Value() == CellAttachmentErrors::CapacityExceeded.code.Value());
        REQUIRE(Manifest(std::span{&reference, 1}, {8, 31}).ErrorValue().code.Value() ==
                CellAttachmentErrors::CapacityExceeded.code.Value());
        REQUIRE(Manifest({}).ErrorValue().code.Value() == CellAttachmentErrors::Invalid.code.Value());
    }

    TEST_CASE("Attachment manifests preserve all five owning feature domains and forbid wire required downgrades",
              "[world_streaming][attachment][policy]") {
        constexpr std::array providers{StreamingCellProvider::Terrain, StreamingCellProvider::Foliage, StreamingCellProvider::PhysicsMesh,
                                       StreamingCellProvider::Audio, StreamingCellProvider::NavigationMesh};
        for (const auto provider : providers) {
            const auto world = CandidateTestSupport::Manifest();
            auto rows = CandidateTestSupport::Payloads();
            rows[1].provider = provider;
            rows[1].requirement = StreamingCellPayloadRequirement::Required;
            const auto candidate =
                PrepareStreamingCellCandidate(world, CandidateTestSupport::Context(), CandidateTestSupport::Header(rows)).Value();
            auto reference = Reference(provider);
            REQUIRE(CellAttachmentManifest::Create(candidate, TestSupport::IdentityFrom<CellAttachmentRevision>(1),
                                                   std::span{&reference, 1}, {8, 256})
                        .ErrorValue()
                        .code.Value() == CellAttachmentErrors::Invalid.code.Value());
            reference.requirement = StreamingCellPayloadRequirement::Required;
            REQUIRE(CellAttachmentManifest::Create(candidate, TestSupport::IdentityFrom<CellAttachmentRevision>(1),
                                                   std::span{&reference, 1}, {8, 256})
                        .HasValue());
        }
    }
}  // namespace Horo::WorldStreaming
