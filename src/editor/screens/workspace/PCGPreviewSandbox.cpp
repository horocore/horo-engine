#include "editor/screens/workspace/PCGPreviewWorkspace.h"

#include <bit>

namespace Horo::Editor {
    namespace {
        /** @brief Appends fixed-width network-order semantic values without hashing padding or addresses. */
        void Word(Sha256Builder &hash, std::uint64_t value) {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[7 - index] = static_cast<std::byte>((value >> (index * 8)) & 0xffU);
            static_cast<void>(hash.Update(bytes));
        }

        /** @brief Hashes canonical signed-zero vector values from the exact sandbox candidate. */
        void Vector(Sha256Builder &hash, const Math::Vec3 &value) {
            for (const float component : {value.x, value.y, value.z})
                Word(hash, std::bit_cast<std::uint32_t>(component == 0 ? 0.0F : component));
        }

        /** @brief Binds sandbox v1's fixed coordinate defaults and complete grid/volume/surface content. */
        Sha256Digest Content(const PCG::PCGSpatialSnapshotCandidate &candidate) {
            Sha256Builder hash;
            constexpr std::string_view domain = "horo.editor.pcg.sandbox.v1.rh_yup.fp32.meters.identity_origin";
            static_cast<void>(hash.Update(std::as_bytes(std::span(domain))));
            Vector(hash, candidate.bounds.minimum);
            Vector(hash, candidate.bounds.maximum);
            for (const auto &grid : candidate.grids) {
                Word(hash, grid.id.Value());
                Vector(hash, grid.origin);
                Vector(hash, grid.spacing);
                for (const auto count : grid.dimensions)
                    Word(hash, count);
            }
            for (const auto &value : candidate.volumes) {
                const auto &box = std::get<PCG::PCGBoxVolume>(value);
                Word(hash, box.id.Value());
                Vector(hash, box.bounds.minimum);
                Vector(hash, box.bounds.maximum);
            }
            for (const auto &surface : candidate.surfaces) {
                Word(hash, surface.id.Value());
                for (const auto &vertex : surface.vertices)
                    Vector(hash, vertex);
                Vector(hash, surface.normal);
            }
            return hash.Finalize();
        }
    }  // namespace

    /** @copydoc CreatePCGPreviewSandbox */
    Result<PCGPreviewInputs> CreatePCGPreviewSandbox() {
        using namespace PCG;
        PCGSpatialSnapshotCandidate candidate;
        candidate.snapshot = SpatialSnapshotId::Create(1).Value();
        candidate.provenance = {SpatialProviderId::Create(1).Value(), SpatialSourceId::Create(1).Value(),
                                SpatialRevision::Create(1).Value()};
        candidate.bounds = {{-8, -8, -8}, {8, 8, 8}};
        candidate.coverage = PCGSpatialCoverage::Complete;
        candidate.grids = {{SpatialElementId::Create(1).Value(), {-7.5F, 0, -7.5F}, {1, 1, 1}, {16, 1, 16}}};
        candidate.volumes = {PCGBoxVolume{SpatialElementId::Create(2).Value(), {{-4, -1, -4}, {4, 1, 4}}}};
        candidate.surfaces = {{SpatialElementId::Create(3).Value(), {{{-8, 0, -8}, {-8, 0, 8}, {8, 0, -8}}}, {0, 1, 0}}};
        const auto digest = Content(candidate);
        auto spatial = CapturePCGSpatialSnapshot(std::move(candidate));
        if (spatial.HasError())
            return Result<PCGPreviewInputs>::Failure(spatial.ErrorValue());
        const std::array fields{PCGAttributeDescriptor{PCGAttributeKey::Create("pcg.density").Value(), PCGAttributeType::Scalar},
                                PCGAttributeDescriptor{PCGAttributeKey::Create("pcg.noise").Value(), PCGAttributeType::Scalar}};
        auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
        if (schema.HasError())
            return Result<PCGPreviewInputs>::Failure(schema.ErrorValue());
        const auto tier = LimitsForTier(PCGOperationalTier::Baseline).Value();
        PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
        limits.providerContent = digest;
        constexpr std::string_view profile = "horo.editor.pcg.preview.numeric.v1";
        limits.numericProfile = ComputeSha256(std::as_bytes(std::span(profile)));
        return Result<PCGPreviewInputs>::Success(
            {spatial.Value(), std::make_shared<const PCGPointSchema>(std::move(schema).Value()), limits});
    }
}  // namespace Horo::Editor
