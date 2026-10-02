#include "Horo/PCGTerrain/TerrainInputSnapshot.h"

#include "Horo/PCG/PCGErrors.h"

#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::PCGTerrain {
    struct TerrainInputSnapshot::State final {
        State(TerrainInputCandidate candidate, const Terrain::TerrainFoliageRegistryBinding publication)
            : input(std::move(candidate)), binding(publication) {}

        TerrainInputCandidate input;
        Terrain::TerrainFoliageRegistryBinding binding;
    };

    namespace {
        constexpr std::size_t MaximumSamples = 16'641;  // One complete 129x129 default Terrain tile.
        constexpr float NormalTolerance = 0.001F;
        constexpr float SlopeTolerance = 0.001F;
        constexpr double CellMeters = 1'024.0;

        /** @brief Validates finite, upward-facing height/slope/material values before publication. */
        [[nodiscard]] bool ValidSample(const TerrainInputSample &sample, const TerrainInputCandidate &candidate,
                                       const std::uint8_t layers) noexcept {
            if (!std::isfinite(sample.height) || !Math::IsFinite(sample.normal) || !std::isfinite(sample.slopeRadians) ||
                sample.materialLayer >= layers || sample.height < candidate.bounds.minimum.y ||
                sample.height > candidate.bounds.maximum.y || sample.normal.y < 0.0F)
                return false;
            if (const float squaredLength = Math::LengthSquared(sample.normal);
                !std::isfinite(squaredLength) || std::abs(squaredLength - 1.0F) > NormalTolerance)
                return false;
            const float horizontal = std::hypot(sample.normal.x, sample.normal.z);
            const float slope = std::atan2(horizontal, sample.normal.y);
            return std::abs(sample.slopeRadians - slope) <= SlopeTolerance;
        }

        /** @brief Converts canonical descriptor millimeters into the candidate's local origin-cell frame. */
        [[nodiscard]] bool CoveredByDescriptor(const TerrainInputCandidate &candidate,
                                               const Terrain::TerrainRevisionedBounds &descriptorBounds) noexcept {
            const auto minimum = descriptorBounds.minimum.Millimeters();
            const auto maximum = descriptorBounds.maximum.Millimeters();
            const std::array candidateMin{candidate.bounds.minimum.x, candidate.bounds.minimum.y, candidate.bounds.minimum.z};
            const std::array candidateMax{candidate.bounds.maximum.x, candidate.bounds.maximum.y, candidate.bounds.maximum.z};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double offset = static_cast<double>(candidate.originCell[axis]) * CellMeters;
                const double low = static_cast<double>(minimum[axis]) / 1'000.0 - offset;
                const double high = static_cast<double>(maximum[axis]) / 1'000.0 - offset;
                if (static_cast<double>(candidateMin[axis]) < low || static_cast<double>(candidateMax[axis]) > high)
                    return false;
            }
            return true;
        }

        /** @brief Checks bounded complete source values without retaining registry storage. */
        [[nodiscard]] Result<void> ValidateCandidate(const TerrainInputCandidate &candidate,
                                                     const Terrain::TerrainDatasetDescriptorData &descriptor) {
            if (!candidate.snapshot.IsValid() || !candidate.provenance.provider.IsValid() || !candidate.provenance.source.IsValid() ||
                !candidate.provenance.revision.IsValid() || !candidate.dataset.IsValid() || !candidate.revision.IsValid() ||
                !candidate.boundsRevision.IsValid() || candidate.originEpoch == 0 || !candidate.bounds.IsValid() ||
                candidate.bounds.minimum.x >= candidate.bounds.maximum.x || candidate.bounds.minimum.y >= candidate.bounds.maximum.y ||
                candidate.bounds.minimum.z >= candidate.bounds.maximum.z || candidate.samplesX < 2 || candidate.samplesZ < 2)
                return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
            if (candidate.coverage != PCG::PCGSpatialCoverage::Complete)
                return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialCoverageUnavailable));
            const std::uint64_t count = static_cast<std::uint64_t>(candidate.samplesX) * candidate.samplesZ;
            if (count > MaximumSamples)
                return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialCapacityExceeded));
            if (candidate.samples.size() != count || candidate.samplesX > descriptor.grid.samplesX ||
                candidate.samplesZ > descriptor.grid.samplesZ || candidate.dataset != descriptor.dataset ||
                candidate.revision.content != descriptor.content || candidate.boundsRevision != descriptor.bounds.revision ||
                !CoveredByDescriptor(candidate, descriptor.bounds))
                return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
            for (const auto &sample : candidate.samples) {
                if (!ValidSample(sample, candidate, descriptor.grid.layersPerTile))
                    return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc TerrainInputSnapshot::Id */
    PCG::SpatialSnapshotId TerrainInputSnapshot::Id() const noexcept {
        return state_->input.snapshot;
    }

    /** @copydoc TerrainInputSnapshot::Provenance */
    const PCG::PCGSpatialProvenance &TerrainInputSnapshot::Provenance() const noexcept {
        return state_->input.provenance;
    }

    /** @copydoc TerrainInputSnapshot::Revision */
    const Terrain::TerrainSnapshotRevision &TerrainInputSnapshot::Revision() const noexcept {
        return state_->input.revision;
    }

    /** @copydoc TerrainInputSnapshot::RegistryBinding */
    Terrain::TerrainFoliageRegistryBinding TerrainInputSnapshot::RegistryBinding() const noexcept {
        return state_->binding;
    }

    /** @copydoc TerrainInputSnapshot::Dataset */
    Terrain::TerrainDatasetId TerrainInputSnapshot::Dataset() const noexcept {
        return state_->input.dataset;
    }

    /** @copydoc TerrainInputSnapshot::Bounds */
    const Math::Aabb &TerrainInputSnapshot::Bounds() const noexcept {
        return state_->input.bounds;
    }

    /** @copydoc TerrainInputSnapshot::Samples */
    std::span<const TerrainInputSample> TerrainInputSnapshot::Samples() const noexcept {
        return state_->input.samples;
    }

    /** @copydoc TerrainInputSnapshot::SampleAt */
    Result<TerrainInputPoint> TerrainInputSnapshot::SampleAt(const std::uint32_t x, const std::uint32_t z) const {
        const auto &input = state_->input;
        if (x >= input.samplesX || z >= input.samplesZ)
            return Result<TerrainInputPoint>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
        const std::size_t index = static_cast<std::size_t>(z) * input.samplesX + x;
        const auto &sample = input.samples[index];
        const auto coordinate = [](const float minimum, const float maximum, const std::uint32_t value, const std::uint32_t count) {
            return static_cast<float>(static_cast<double>(minimum) + (static_cast<double>(maximum) - minimum) * value / (count - 1U));
        };
        const Math::Vec3 position{coordinate(input.bounds.minimum.x, input.bounds.maximum.x, x, input.samplesX), sample.height,
                                  coordinate(input.bounds.minimum.z, input.bounds.maximum.z, z, input.samplesZ)};
        return Result<TerrainInputPoint>::Success({position, sample});
    }

    /** @copydoc TerrainInputSnapshot::SamplesX */
    std::uint32_t TerrainInputSnapshot::SamplesX() const noexcept {
        return state_->input.samplesX;
    }

    /** @copydoc TerrainInputSnapshot::SamplesZ */
    std::uint32_t TerrainInputSnapshot::SamplesZ() const noexcept {
        return state_->input.samplesZ;
    }

    /** @copydoc TerrainInputSnapshot::OriginEpoch */
    std::uint64_t TerrainInputSnapshot::OriginEpoch() const noexcept {
        return state_->input.originEpoch;
    }

    /** @copydoc CaptureTerrainInput */
    Result<TerrainInputSnapshot> CaptureTerrainInput(const Terrain::TerrainFoliageRegistry &registry, TerrainInputCandidate candidate) {
        auto publication = registry.Snapshot();
        if (publication.HasError())
            return Result<TerrainInputSnapshot>::Failure(publication.ErrorValue());
        const auto &snapshot = publication.Value();
        const std::array required{Terrain::TerrainFoliageCapability::TerrainQuery};
        auto requiredSet = Terrain::TerrainFoliageCapabilitySet::Create(required);
        if (requiredSet.HasError())
            return Result<TerrainInputSnapshot>::Failure(requiredSet.ErrorValue());
        if (auto grant = snapshot.ProjectCapabilities(requiredSet.Value()); grant.HasError())
            return Result<TerrainInputSnapshot>::Failure(grant.ErrorValue());
        auto handle = snapshot.FindDataset(candidate.dataset);
        if (handle.HasError())
            return Result<TerrainInputSnapshot>::Failure(handle.ErrorValue());
        auto registration = snapshot.Resolve(handle.Value());
        if (registration.HasError())
            return Result<TerrainInputSnapshot>::Failure(registration.ErrorValue());
        if (auto validation = ValidateCandidate(candidate, registration.Value()->descriptor.Data()); validation.HasError())
            return Result<TerrainInputSnapshot>::Failure(validation.ErrorValue());
        try {
            auto state = std::make_shared<const TerrainInputSnapshot::State>(std::move(candidate), snapshot.Binding());
            auto latest = registry.Snapshot();
            if (latest.HasError())
                return Result<TerrainInputSnapshot>::Failure(latest.ErrorValue());
            if (latest.Value().Binding() != state->binding)
                return Result<TerrainInputSnapshot>::Failure(MakeError(PCG::PCGErrors::SpatialSnapshotStale));
            return Result<TerrainInputSnapshot>::Success(TerrainInputSnapshot{TerrainInputSnapshot::ConstructionKey{}, std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<TerrainInputSnapshot>::Failure(MakeError(PCG::PCGErrors::SpatialCapacityExceeded));
        }
    }

    /** @copydoc ReplaceTerrainInput */
    Result<TerrainInputSnapshot> ReplaceTerrainInput(const Terrain::TerrainFoliageRegistry &registry, const TerrainInputSnapshot &previous,
                                                     TerrainInputCandidate candidate) {
        if (candidate.dataset != previous.Dataset() || candidate.provenance.provider != previous.Provenance().provider ||
            candidate.provenance.source != previous.Provenance().source || !candidate.snapshot.IsValid() ||
            candidate.snapshot == previous.Id() || !candidate.provenance.revision.IsValid() ||
            candidate.provenance.revision.Value() <= previous.Provenance().revision.Value())
            return Result<TerrainInputSnapshot>::Failure(MakeError(PCG::PCGErrors::SpatialReplacementInvalid));
        return CaptureTerrainInput(registry, std::move(candidate));
    }

    /** @copydoc ValidateTerrainInputCurrent */
    Result<void> ValidateTerrainInputCurrent(const Terrain::TerrainFoliageRegistry &registry, const TerrainInputSnapshot &snapshot,
                                             const Terrain::TerrainSnapshotRevision &current,
                                             const PCG::PCGSpatialCurrentness &currentSpatial) {
        if (!current.IsValid())
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
        auto publication = registry.Snapshot();
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        if (publication.Value().Binding() != snapshot.RegistryBinding() || current != snapshot.Revision())
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialSnapshotStale));
        auto handle = publication.Value().FindDataset(snapshot.Dataset());
        if (handle.HasError())
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialSnapshotStale));
        if (handle.Value().content != current.content)
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialSnapshotStale));
        if (!currentSpatial.provider.IsValid() || !currentSpatial.source.IsValid() || !currentSpatial.revision.IsValid() ||
            currentSpatial.originEpoch == 0)
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialInputInvalid));
        if (currentSpatial.provider != snapshot.Provenance().provider || currentSpatial.source != snapshot.Provenance().source)
            return Result<void>::Failure(MakeError(PCG::PCGErrors::IdentityUnknown));
        if (currentSpatial.revision != snapshot.Provenance().revision || currentSpatial.originEpoch != snapshot.OriginEpoch())
            return Result<void>::Failure(MakeError(PCG::PCGErrors::SpatialSnapshotStale));
        return Result<void>::Success();
    }
}  // namespace Horo::PCGTerrain
