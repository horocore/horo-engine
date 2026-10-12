#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"
#include "PCGPointGenerationInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>

namespace Horo::PCG::detail {
    namespace {
        /** @brief Preserves the owning typed error domain at the kernel boundary. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief A defined integer draw stream rooted solely in canonical per-sample provenance. */
        double Draw(std::uint64_t &state) noexcept {
            state += 0x9e3779b97f4a7c15ULL;
            std::uint64_t word = state;
            word = (word ^ (word >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            word = (word ^ (word >> 27U)) * 0x94d049bb133111ebULL;
            word ^= word >> 31U;
            return static_cast<double>(word >> 11U) * 0x1.0p-53;
        }

        /** @brief Rejects intermediate overflow before conversion to the captured float representation. */
        Result<Math::Vec3> Position(const std::array<double, 3> &value) {
            if (std::ranges::any_of(value, [](const double coordinate) {
                return !std::isfinite(coordinate) || std::abs(coordinate) > std::numeric_limits<float>::max();
            }))
                return Reject<Math::Vec3>(PCGErrors::CpuEvaluationInvalid);
            return Result<Math::Vec3>::Success({static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])});
        }

        /** @brief Chooses within the exact captured box using double interpolation and fixed axis draw order. */
        Result<Math::Vec3> BoxPosition(const PCGBoxVolume &box, std::uint64_t seed) {
            const std::array<double, 3> minimum{box.bounds.minimum.x, box.bounds.minimum.y, box.bounds.minimum.z};
            const std::array<double, 3> maximum{box.bounds.maximum.x, box.bounds.maximum.y, box.bounds.maximum.z};
            std::array<double, 3> coordinates{};
            for (std::size_t axis = 0; axis < coordinates.size(); ++axis)
                coordinates[axis] = minimum[axis] + (maximum[axis] - minimum[axis]) * Draw(seed);
            return Position(coordinates);
        }

        /** @brief Uses lattice identity before jitter; sampling windows intersect complete captured bounds. */
        Result<Math::Vec3> GridPosition(const PCGGrid &grid, const Math::Aabb &bounds, const std::size_t index, const double fraction,
                                        std::uint64_t seed) {
            const std::size_t xy = static_cast<std::size_t>(grid.dimensions[0]) * grid.dimensions[1];
            const std::array<std::size_t, 3> cell{index % grid.dimensions[0], (index / grid.dimensions[0]) % grid.dimensions[1],
                                                  index / xy};
            const std::array<double, 3> origin{grid.origin.x, grid.origin.y, grid.origin.z};
            const std::array<double, 3> spacing{grid.spacing.x, grid.spacing.y, grid.spacing.z};
            const std::array<double, 3> minimum{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
            const std::array<double, 3> maximum{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
            std::array<double, 3> position{};
            for (std::size_t axis = 0; axis < position.size(); ++axis) {
                const double center = origin[axis] + spacing[axis] * static_cast<double>(cell[axis]);
                const double radius = spacing[axis] * fraction * 0.5;
                const double begin = std::max(minimum[axis], center - radius);
                const double end = std::min(maximum[axis], center + radius);
                if (!std::isfinite(center) || begin > end)
                    return Reject<Math::Vec3>(PCGErrors::CpuEvaluationInvalid);
                position[axis] = begin + (end - begin) * Draw(seed);
            }
            return Position(position);
        }

        /** @brief One semantic row-major stratum produces one point independently of worker scheduling. */
        Result<Math::Vec3> StratifiedPosition(const PCGBoxVolume &box, const std::array<std::uint32_t, 3> &strata, const std::size_t index,
                                              std::uint64_t seed) {
            const std::size_t xy = static_cast<std::size_t>(strata[0]) * strata[1];
            const std::array<std::size_t, 3> cell{index % strata[0], (index / strata[0]) % strata[1], index / xy};
            const std::array<double, 3> minimum{box.bounds.minimum.x, box.bounds.minimum.y, box.bounds.minimum.z};
            const std::array<double, 3> maximum{box.bounds.maximum.x, box.bounds.maximum.y, box.bounds.maximum.z};
            std::array<double, 3> position{};
            for (std::size_t axis = 0; axis < position.size(); ++axis) {
                const double offset = (static_cast<double>(cell[axis]) + Draw(seed)) / strata[axis];
                position[axis] = minimum[axis] + (maximum[axis] - minimum[axis]) * offset;
            }
            return Position(position);
        }

        /** @brief Checks stored float positions using normalized deltas, avoiding distance-square overflow/underflow. */
        bool Separated(const Math::Vec3 &candidate, const Math::Vec3 &previous, const double distance) noexcept {
            const std::array<double, 3> delta{std::abs(static_cast<double>(candidate.x) - previous.x),
                                              std::abs(static_cast<double>(candidate.y) - previous.y),
                                              std::abs(static_cast<double>(candidate.z) - previous.z)};
            double squared{};
            for (const double axis : delta) {
                if (axis >= distance)
                    return true;
                const double normalized = axis / distance;
                squared += normalized * normalized;
            }
            return squared >= 1.0;
        }

        /** @brief One canonical generated record before insertion into the bounded point prefix. */
        struct GeneratedSample final {
            Math::Vec3 position{};
            std::uint64_t sample{};
            std::uint64_t seed{};
            Sha256Digest root{};
        };

        /** @brief Writes core and semantic identity together; acceptance index never becomes sample identity. */
        Result<void> WriteSample(const PCGPointWriteView &writer, const std::size_t index, const GeneratedSample &record,
                                 const bool identity) {
            Math::Transform transform;
            transform.translation = record.position;
            if (const auto core = writer.SetPoint(index, transform, {record.position, record.position}, 1.0F, record.seed); core.HasError())
                return core;
            if (!identity)
                return Result<void>::Success();
            if (const auto written = writer.SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute, index, record.sample);
                written.HasError())
                return written;
            for (std::size_t word = 0; word < PCGSourceRootAttributes.size(); ++word) {
                std::uint64_t value{};
                for (std::size_t byte = 0; byte < 8; ++byte)
                    value = (value << 8U) | record.root.bytes[word * 8 + byte];
                if (const auto written = writer.SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word], index, value);
                    written.HasError())
                    return written;
            }
            return Result<void>::Success();
        }

        /** @brief Stable attempted-sample identity drives both point seed and coordinate draws. */
        Result<std::uint64_t> SampleSeed(const NodeExecutionContext &context, const std::uint32_t node, const std::uint64_t identity) {
            const auto sample = SourceSampleId::Create(identity);
            if (sample.HasError())
                return Result<std::uint64_t>::Failure(sample.ErrorValue());
            return context.provenance[node].Seed(sample.Value());
        }

        /** @brief Writes exact lattice/stratum count in semantic order; failure never seals a partial prefix. */
        Result<void> GenerateCells(const NodeExecutionContext &context, const std::uint32_t node, const PointGenerationSettings &settings,
                                   const PointGenerationDomain &domain, const PCGPointWriteView &writer, const bool identity) {
            for (std::size_t index = 0; index < domain.count; ++index) {
                if (index % 256 == 0 && context.cancellation.IsCancellationRequested())
                    return Reject<void>(PCGErrors::CpuEvaluationClosed);
                const auto seed = SampleSeed(context, node, static_cast<std::uint64_t>(index) + 1);
                if (seed.HasError())
                    return Result<void>::Failure(seed.ErrorValue());
                const auto position = std::visit([&]<typename T>(const T &value) -> Result<Math::Vec3> {
                    if constexpr (std::is_same_v<T, PCGGridGenerationSettings>)
                        return GridPosition(*domain.grid, context.spatial.Bounds(), index, 0.0, seed.Value());
                    else if constexpr (std::is_same_v<T, PCGJitterGenerationSettings>)
                        return GridPosition(*domain.grid, context.spatial.Bounds(), index, value.fraction, seed.Value());
                    else if constexpr (std::is_same_v<T, PCGStratifiedGenerationSettings>)
                        return StratifiedPosition(*domain.box, value.strata, index, seed.Value());
                    else
                        return Reject<Math::Vec3>(PCGErrors::CpuEvaluationUnsupported);
                }, settings);
                if (position.HasError())
                    return Result<void>::Failure(position.ErrorValue());
                if (const auto written =
                        WriteSample(writer, index,
                                    {position.Value(), static_cast<std::uint64_t>(index) + 1, seed.Value(), context.provenance[node].Key()},
                                    identity);
                    written.HasError())
                    return written;
            }
            return Result<void>::Success();
        }

        /** @brief Bounded sequential rejection sampling; every pair check participates in cancellation. */
        Result<void> GeneratePoisson(const NodeExecutionContext &context, const std::uint32_t node,
                                     const PCGPoissonGenerationSettings &settings, const PCGBoxVolume &box,
                                     const PCGPointWriteView &writer) {
            std::vector<Math::Vec3> accepted;
            accepted.reserve(settings.count);  // Charged in the complete node scratch envelope before workspace creation.
            for (std::uint64_t attempt = 1; attempt <= settings.attempts && accepted.size() < settings.count; ++attempt) {
                if (context.cancellation.IsCancellationRequested())
                    return Reject<void>(PCGErrors::CpuEvaluationClosed);
                const auto seed = SampleSeed(context, node, attempt);
                if (seed.HasError())
                    return Result<void>::Failure(seed.ErrorValue());
                const auto position = BoxPosition(box, seed.Value());
                if (position.HasError())
                    return Result<void>::Failure(position.ErrorValue());
                bool separated = true;
                for (std::size_t previous = 0; previous < accepted.size(); ++previous) {
                    if (previous % 256 == 0 && context.cancellation.IsCancellationRequested())
                        return Reject<void>(PCGErrors::CpuEvaluationClosed);
                    if (!Separated(position.Value(), accepted[previous], settings.minimumDistance)) {
                        separated = false;
                        break;
                    }
                }
                if (!separated)
                    continue;
                if (const auto written = WriteSample(writer, accepted.size(),
                                                     {position.Value(), attempt, seed.Value(), context.provenance[node].Key()}, true);
                    written.HasError())
                    return written;
                accepted.push_back(position.Value());
            }
            return accepted.size() == settings.count ? Result<void>::Success() : Reject<void>(PCGErrors::GenerationAttemptsExhausted);
        }
    }  // namespace

    /** @copydoc ExecutePointGeneration */
    Result<void> ExecutePointGeneration(const NodeExecutionContext &context, const std::uint32_t node,
                                        const PointGenerationSettings &settings) {
        const auto output = std::ranges::find(context.bounds, node, &PCGPointOutputBound::node);
        if (output == context.bounds.end())
            return Reject<void>(PCGErrors::CpuEvaluationInvalid);
        const auto domain = ResolvePointGeneration(settings, context.spatial);
        if (domain.HasError())
            return Result<void>::Failure(domain.ErrorValue());
        auto opened = context.workspace.BeginOutput(node, output->pin, domain.Value().count);
        if (opened.HasError())
            return Result<void>::Failure(opened.ErrorValue());
        try {
            const auto result =
                std::get_if<PCGPoissonGenerationSettings>(&settings)
                    ? GeneratePoisson(context, node, std::get<PCGPoissonGenerationSettings>(settings), *domain.Value().box, opened.Value())
                    : GenerateCells(context, node, settings, domain.Value(), opened.Value(),
                                    output->schema->Find(PCGSourceSampleAttribute) != nullptr);
            if (result.HasError())
                return result;
        } catch (const std::bad_alloc &) {
            return Reject<void>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
        return context.workspace.SealOutput(node, output->pin);
    }
}  // namespace Horo::PCG::detail
