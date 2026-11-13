#include "Horo/AI/EnvironmentQueryGenerators.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

namespace Horo::AI {
    namespace {
        using Generation = Result<QueryGenerationResult>;

        /** @brief Checks that the complete configured output fits caller-owned storage. */
        [[nodiscard]] bool ValidOutput(const QueryGeneratorOutput &output) {
            const auto &origin = output.provenance;
            return origin.stage.IsValid() && origin.generator.IsValid() && origin.provider.IsValid() && origin.revision > 0 &&
                   output.maximumItems > 0 && output.maximumItems <= EnvironmentQuerySchemaLimits::MaximumResults &&
                   output.maximumItems <= output.items.size();
        }

        /** @brief Adds a bounded lattice offset while preserving the full global millimeter range. */
        [[nodiscard]] std::optional<std::int64_t> Offset(const std::int64_t base, const std::int64_t delta) {
            if (delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta)
                return std::nullopt;
            if (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)
                return std::nullopt;
            return base + delta;
        }

        /** @brief Evaluates a bounded square/annulus in canonical lattice order, with an optional publication destination. */
        [[nodiscard]] QueryGenerationResult SampleLattice(const Math::WorldCoordinate64 center, const QueryPointLattice &lattice,
                                                          const QueryGeneratorProvenance &origin,
                                                          const std::span<QueryGeneratedItem> destination) {
            QueryGenerationResult result{.provenance = origin};
            const auto point = center.Millimeters();
            const auto outer = static_cast<std::int64_t>(lattice.outerSteps);
            const auto inner = static_cast<std::int64_t>(lattice.innerSteps.value_or(0));
            for (auto z = -outer; z <= outer; ++z) {
                for (auto x = -outer; x <= outer; ++x) {
                    const auto squared = x * x + z * z;
                    if (lattice.innerSteps && (squared < inner * inner || squared > outer * outer))
                        continue;
                    const auto horizontal = Offset(point[0], x * lattice.stepMillimeters);
                    const auto depth = Offset(point[2], z * lattice.stepMillimeters);
                    if (!horizontal || !depth) {
                        ++result.invalidCoordinateCount;
                        continue;
                    }
                    if (!destination.empty())
                        destination[result.itemCount] = Math::WorldCoordinate64::FromMillimeters(*horizontal, point[1], *depth);
                    ++result.itemCount;
                }
            }
            return result;
        }

        /** @brief Uses an explicit canonical order rather than discovery or sense order. */
        [[nodiscard]] auto SourceOrder(const PerceptionSourceRef &source) noexcept {
            return std::tuple{source.slot, source.generation};
        }
    }  // namespace

    /** @copydoc GenerateQueryLattice */
    Result<QueryGenerationResult> GenerateQueryLattice(const QueryContextSnapshot &contexts, const QueryContextId center,
                                                       const QueryPointLattice &lattice, const QueryGeneratorOutput &output) {
        constexpr std::uint32_t maxOuterSteps = 31;  // (2 * 31 + 1)^2 bounded lattice visits per pass.
        if (!ValidOutput(output) || lattice.stepMillimeters == 0 || lattice.outerSteps > maxOuterSteps ||
            lattice.innerSteps.value_or(0) > lattice.outerSteps)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (contexts.Revision() != output.provenance.revision)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto *context = contexts.Find(center);
        if (!context)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
        const auto *position = std::get_if<Math::WorldCoordinate64>(&context->value);
        if (!position)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        const auto measured = SampleLattice(*position, lattice, output.provenance, {});
        if (measured.itemCount > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        return Generation::Success(SampleLattice(*position, lattice, output.provenance, output.items.first(output.maximumItems)));
    }

    /** @copydoc GenerateQueryPoints */
    Result<QueryGenerationResult> GenerateQueryPoints(const std::span<const Math::WorldCoordinate64> points,
                                                      const QueryGeneratorOutput &output) {
        if (!ValidOutput(output))
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (points.size() > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::ranges::copy(points, output.items.begin());
        return Generation::Success({.provenance = output.provenance, .itemCount = points.size()});
    }

    /** @copydoc GenerateQueryPerceivedEntities */
    Result<QueryGenerationResult> GenerateQueryPerceivedEntities(const QueryPerceivedEntitySource &source,
                                                                 const QueryGeneratorOutput &output) {
        if (!ValidOutput(output))
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto current = source.publication.Current(source.scene, source.filtering, source.blackboard);
        if (current.HasError())
            return Generation::Failure(current.ErrorValue());
        if (current.Value().revision != output.provenance.revision)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto &memory = current.Value().memory;
        std::array<PerceptionSourceRef, MaximumPerceptionMemoryEntries> records{};
        auto candidates = std::span{records}.first(memory.count);
        std::ranges::transform(std::span{memory.entries}.first(memory.count), candidates.begin(), [](const PerceivedStimulus &entry) {
            return entry.key.source;
        });
        std::ranges::sort(candidates, {}, SourceOrder);
        const auto duplicates = std::ranges::unique(candidates);
        const auto count = static_cast<std::size_t>(duplicates.begin() - candidates.begin());
        if (count > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::ranges::transform(candidates.first(count), output.items.begin(), [&source](const PerceptionSourceRef &record) {
            return Runtime::EntityRef{source.scene.RuntimeId(), {record.slot, record.generation}};
        });
        return Generation::Success({.provenance = output.provenance, .itemCount = count});
    }
}  // namespace Horo::AI
