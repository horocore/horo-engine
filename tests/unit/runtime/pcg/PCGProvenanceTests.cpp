#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGProvenance.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

namespace Horo::PCG {
    namespace {
        template <typename T> T Id(const std::uint64_t value) {
            auto result = T::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Sha256Digest Digest(const std::uint8_t value) {
            Sha256Digest digest;
            digest.bytes.fill(value);
            return digest;
        }

        PCGProvenanceCandidate Candidate() {
            PCGProvenanceCandidate candidate;
            candidate.graph = {Id<GraphId>(1), Id<GraphRevision>(2)};
            candidate.graphContent = Digest(3);
            candidate.graphSeed = 4;
            candidate.world = Id<PCGWorldId>(5);
            candidate.cell = {-1, 0, 7};
            candidate.node = Id<NodeId>(8);
            candidate.inputs = {{Id<PCGInputId>(11), 12, Digest(13), PCGInputDeterminism::Deterministic},
                                {Id<PCGInputId>(9), 10, Digest(11), PCGInputDeterminism::Deterministic}};
            candidate.providers = {{Id<SpatialProviderId>(21), Id<SpatialSourceId>(22), Id<SpatialSnapshotId>(23), Id<SpatialRevision>(24),
                                    25, Digest(26), PCGInputDeterminism::Deterministic},
                                   {Id<SpatialProviderId>(17), Id<SpatialSourceId>(18), Id<SpatialSnapshotId>(19), Id<SpatialRevision>(20),
                                    21, Digest(22), PCGInputDeterminism::Deterministic}};
            return candidate;
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        PCGOutputStamp Output(const std::uint64_t sample, const std::uint64_t execution = 30) {
            return {{{{Id<GraphId>(1), Id<GraphRevision>(2)}, Id<ExecutionValue>(execution)},
                     Id<NodeId>(8),
                     Id<PinId>(31),
                     Id<SourceSampleId>(sample),
                     0},
                    Digest(static_cast<std::uint8_t>(sample))};
        }
    }  // namespace

    TEST_CASE("PCG provenance canonicalizes order and derives stable sample streams", "[unit][pcg][provenance]") {
        const PCGProvenance first = CapturePCGProvenance(Candidate()).Value();
        auto reordered = Candidate();
        std::ranges::reverse(reordered.inputs);
        std::ranges::reverse(reordered.providers);
        const PCGProvenance second = CapturePCGProvenance(std::move(reordered)).Value();
        CHECK(first.Key() == second.Key());
        CHECK(first.Data().inputs.front().id == Id<PCGInputId>(9));
        CHECK(first.Data().providers.front().provider == Id<SpatialProviderId>(17));
        CHECK(first.Seed(Id<SourceSampleId>(40)).Value() == second.Seed(Id<SourceSampleId>(40)).Value());
        CHECK(first.Seed(Id<SourceSampleId>(40)).Value() != first.Seed(Id<SourceSampleId>(41)).Value());
        CHECK(ValidatePCGProvenanceReuse(first, second).HasValue());
    }

    TEST_CASE("PCG provenance invalidates reuse for every authoritative dimension", "[unit][pcg][provenance]") {
        const PCGProvenance original = CapturePCGProvenance(Candidate()).Value();
        std::array<PCGProvenanceCandidate, 13> changed;
        changed.fill(Candidate());
        changed[0].graph.revision = Id<GraphRevision>(3);
        changed[1].graphContent = Digest(4);
        changed[2].graphSeed++;
        changed[3].world = Id<PCGWorldId>(6);
        changed[4].cell[0]++;
        changed[5].node = Id<NodeId>(9);
        changed[6].numericPolicyVersion++;
        changed[7].inputs[0].revision++;
        changed[8].inputs[0].content = Digest(14);
        changed[9].providers[0].snapshot = Id<SpatialSnapshotId>(29);
        changed[10].providers[0].revision = Id<SpatialRevision>(29);
        changed[11].providers[0].originEpoch++;
        changed[12].providers[0].content = Digest(29);
        for (auto &candidate : changed)
            CheckError(ValidatePCGProvenanceReuse(original, CapturePCGProvenance(std::move(candidate)).Value()),
                       PCGErrors::ProvenanceStale);
    }

    TEST_CASE("PCG determinism and numeric support fail closed", "[unit][pcg][provenance]") {
        auto nondeterministic = Candidate();
        nondeterministic.inputs[0].determinism = PCGInputDeterminism::NonDeterministic;
        CheckError(CapturePCGProvenance(nondeterministic), PCGErrors::ProvenanceTierUnsupported);
        nondeterministic.determinism = PCGDeterminismClass::BestEffortPreview;
        nondeterministic.numeric = PCGNumericSupport::PreviewOnly;
        const PCGProvenance preview = CapturePCGProvenance(nondeterministic).Value();
        CheckError(preview.Seed(Id<SourceSampleId>(40)), PCGErrors::ProvenanceTierUnsupported);
        CheckError(ValidatePCGProvenanceReuse(preview, preview), PCGErrors::ProvenanceTierUnsupported);

        auto profile = Candidate();
        profile.determinism = PCGDeterminismClass::ProfileDeterministic;
        profile.numeric = PCGNumericSupport::CertifiedProfileFloat;
        CheckError(CapturePCGProvenance(profile), PCGErrors::ProvenanceTierUnsupported);
        profile.profile = Digest(42);
        const PCGProvenance certified = CapturePCGProvenance(profile).Value();
        profile.profile = Digest(43);
        CheckError(ValidatePCGProvenanceReuse(certified, CapturePCGProvenance(profile).Value()), PCGErrors::ProvenanceStale);
        profile.numeric = PCGNumericSupport::PortableInteger;
        CheckError(CapturePCGProvenance(profile), PCGErrors::ProvenanceTierUnsupported);
        auto provider = Candidate();
        provider.providers[0].determinism = PCGInputDeterminism::NonDeterministic;
        CheckError(CapturePCGProvenance(provider), PCGErrors::ProvenanceTierUnsupported);
    }

    TEST_CASE("PCG provenance rejects malformed, duplicate and excessive inputs", "[unit][pcg][provenance]") {
        auto invalid = Candidate();
        invalid.graph = {};
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceInvalid);
        invalid = Candidate();
        invalid.inputs[0].revision = 0;
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceInvalid);
        invalid = Candidate();
        invalid.providers[0].originEpoch = 0;
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceInvalid);
        invalid = Candidate();
        invalid.numericPolicyVersion = 0;
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceInvalid);
        invalid = Candidate();
        invalid.inputs[0].id = invalid.inputs[1].id;
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceDuplicate);
        invalid = Candidate();
        invalid.providers[0].provider = invalid.providers[1].provider;
        invalid.providers[0].source = invalid.providers[1].source;
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceDuplicate);
        invalid = Candidate();
        invalid.inputs.clear();
        for (std::uint64_t id = 1; id <= 64; ++id)
            invalid.inputs.push_back({Id<PCGInputId>(id), 1, Digest(1), PCGInputDeterminism::Deterministic});
        CHECK(CapturePCGProvenance(invalid).HasValue());
        invalid.inputs.push_back({Id<PCGInputId>(65), 1, Digest(1), PCGInputDeterminism::Deterministic});
        CheckError(CapturePCGProvenance(invalid), PCGErrors::ProvenanceCapacityExceeded);
    }

    TEST_CASE("PCG output hashes ignore execution attempt and input order", "[unit][pcg][provenance]") {
        const PCGProvenance root = CapturePCGProvenance(Candidate()).Value();
        const std::array first{Output(41), Output(40)};
        const std::array reordered{Output(40, 31), Output(41, 31)};
        CHECK(HashPCGOutputs(root, first).Value() == HashPCGOutputs(root, reordered).Value());
        auto changed = reordered;
        changed[0].content = Digest(99);
        CHECK(HashPCGOutputs(root, first).Value() != HashPCGOutputs(root, changed).Value());
        const std::array duplicate{Output(40), Output(40)};
        CheckError(HashPCGOutputs(root, duplicate), PCGErrors::ProvenanceDuplicate);
        const std::array mixedExecutions{Output(40), Output(41, 31)};
        CheckError(HashPCGOutputs(root, mixedExecutions), PCGErrors::ProvenanceInvalid);
        auto wrongGraph = Output(40);
        wrongGraph.id.execution.generation.revision = Id<GraphRevision>(99);
        CheckError(HashPCGOutputs(root, std::span<const PCGOutputStamp>(&wrongGraph, 1)), PCGErrors::ProvenanceInvalid);
        CHECK(HashPCGOutputs(root, std::span<const PCGOutputStamp>{}).HasValue());
        CheckError(root.Seed({}), PCGErrors::ProvenanceInvalid);
        std::vector<PCGOutputStamp> oversized(16'385, Output(40));
        CheckError(HashPCGOutputs(root, oversized), PCGErrors::ProvenanceCapacityExceeded);
    }

    TEST_CASE("PCG provenance lifecycle rejection preserves retained readers", "[unit][pcg][provenance]") {
        const PCGProvenance oldRoot = CapturePCGProvenance(Candidate()).Value();
        for (const auto state :
             {PCGProvenanceAdmission::CancellationRequested, PCGProvenanceAdmission::ShuttingDown, PCGProvenanceAdmission::Count})
            CheckError(CapturePCGProvenance(Candidate(), state), PCGErrors::ProvenanceLifecycleUnavailable);
        auto next = Candidate();
        next.graph.revision = Id<GraphRevision>(3);
        const PCGProvenance replacement = CapturePCGProvenance(next).Value();
        CHECK(oldRoot.Data().graph.revision == Id<GraphRevision>(2));
        CHECK(replacement.Data().graph.revision == Id<GraphRevision>(3));
        CheckError(ValidatePCGProvenanceReuse(oldRoot, replacement), PCGErrors::ProvenanceStale);
        CHECK(oldRoot.Seed(Id<SourceSampleId>(40)).HasValue());
    }
}  // namespace Horo::PCG
