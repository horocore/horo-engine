#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"
#include "PCGPointFilteringInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <type_traits>

namespace Horo::PCG::detail {
    namespace {
        /** @brief Counts an immutable disjoint density range with the legacy cancellation cadence. */
        [[nodiscard]] Result<std::size_t> CountPartition(std::span<const float> densities, double threshold, std::size_t begin,
                                                         std::size_t end, const CancellationToken &cancellation) {
            std::size_t selected{};
            for (std::size_t index = begin; index < end; ++index) {
                if ((index - begin) % 256 == 0 && cancellation.IsCancellationRequested())
                    return Result<std::size_t>::Failure(MakeError(PCGErrors::CpuEvaluationClosed));
                selected += densities[index] >= threshold ? 1U : 0U;
            }
            return Result<std::size_t>::Success(selected);
        }

        /** @brief Retains a typed evaluation failure without losing its error domain. */
        template <class T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Compares every exact component; integer widths remain unchanged. */
        template <class V> bool ValuesEqual(const V &a, const V &b) {
            if constexpr (std::is_arithmetic_v<V>)
                return a == b;
            else {
                bool equal = a.x == b.x && a.y == b.y;
                if constexpr (requires { a.z; })
                    equal = equal && a.z == b.z;
                if constexpr (requires { a.w; })
                    equal = equal && a.w == b.w;
                return equal;
            }
        }

        /** @brief Executes only admitted comparisons; vector ordering is never inferred. */
        template <class V> bool Compare(const V &actual, const V &expected, PCGAttributeComparison op) {
            using enum PCGAttributeComparison;
            if (op == Equal)
                return ValuesEqual(actual, expected);
            if (op == NotEqual)
                return !ValuesEqual(actual, expected);
            if constexpr (std::is_arithmetic_v<V> && !std::is_same_v<V, bool>) {
                switch (op) {
                    case Less:
                        return actual < expected;
                    case LessEqual:
                        return actual <= expected;
                    case Greater:
                        return actual > expected;
                    case GreaterEqual:
                        return actual >= expected;
                    default:
                        return false;
                }
            }
            return false;
        }
        /** @brief Maps exact predicate value types onto existing workspace column ownership. */
        template <class V> struct ColumnFor;

        template <> struct ColumnFor<bool> {
            using Type = PCGBoolColumn;
        };

        template <> struct ColumnFor<std::int64_t> {
            using Type = PCGSignedIntegerColumn;
        };

        template <> struct ColumnFor<std::uint64_t> {
            using Type = PCGUnsignedIntegerColumn;
        };

        template <> struct ColumnFor<double> {
            using Type = PCGScalarColumn;
        };

        template <> struct ColumnFor<Math::Vec2> {
            using Type = PCGVector2Column;
        };

        template <> struct ColumnFor<Math::Vec3> {
            using Type = PCGVector3Column;
        };

        template <> struct ColumnFor<Math::Vec4> {
            using Type = PCGVector4Column;
        };

        /** @brief Compares the exact typed source column after schema admission, retaining integer precision. */
        Result<bool> AttributeMatch(const PCGAttributeFilterSettings &settings, const PCGPointReadView &source, std::size_t index) {
            return std::visit([&]<class V>(const V &expected) -> Result<bool> {
                const auto values = source.FindColumn<typename ColumnFor<V>::Type>(settings.key.Value());
                if (values.size() != source.PointCount())
                    return Reject<bool>(PCGErrors::CpuEvaluationInvalid);
                if constexpr (std::is_same_v<V, bool>)
                    return Result<bool>::Success(Compare(values[index] != 0, expected, settings.comparison));
                else
                    return Result<bool>::Success(Compare(values[index], expected, settings.comparison));
            }, settings.value);
        }

        /** @brief Samples finite captured surface geometry in stable identity order, with no provider callbacks. */
        Result<bool> SlopeMatch(const PCGSlopeFilterSettings &settings, Math::Vec3 point, const PCGSpatialSnapshot &spatial,
                                CancellationToken cancellation) {
            const PCGSurfaceTriangle *nearest{};
            double nearestDistance = std::numeric_limits<double>::infinity();
            for (const auto &triangle : spatial.Surfaces()) {
                if (cancellation.IsCancellationRequested())
                    return Reject<bool>(PCGErrors::CpuEvaluationClosed);
                const auto &a = triangle.vertices[0];
                const auto &b = triangle.vertices[1];
                const auto &c = triangle.vertices[2];
                const double bx = static_cast<double>(b.x) - a.x;
                const double bz = static_cast<double>(b.z) - a.z;
                const double cx = static_cast<double>(c.x) - a.x;
                const double cz = static_cast<double>(c.z) - a.z;
                const double px = static_cast<double>(point.x) - a.x;
                const double pz = static_cast<double>(point.z) - a.z;
                const double determinant = bx * cz - cx * bz;
                if (determinant == 0.0)
                    continue;
                const double u = (px * cz - cx * pz) / determinant;
                const double v = (bx * pz - px * bz) / determinant;
                if (u < 0.0 || v < 0.0 || u + v > 1.0)
                    continue;
                const double height = a.y + u * (static_cast<double>(b.y) - a.y) + v * (static_cast<double>(c.y) - a.y);
                const double distance = std::abs(static_cast<double>(point.y) - height);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = &triangle;
                }
            }
            if (nearest == nullptr)
                return Reject<bool>(PCGErrors::SpatialCoverageUnavailable);
            const double degrees = std::acos(std::clamp(static_cast<double>(nearest->normal.y), -1.0, 1.0)) * 180.0 / std::numbers::pi;
            return Result<bool>::Success(degrees >= settings.minimumDegrees && degrees <= settings.maximumDegrees);
        }

        /** @brief Avoids squared-threshold overflow/underflow over finite stored coordinates. */
        double Distance(Math::Vec3 a, Math::Vec3 b) {
            const double x = static_cast<double>(a.x) - b.x;
            const double y = static_cast<double>(a.y) - b.y;
            const double z = static_cast<double>(a.z) - b.z;
            return std::hypot(x, y, z);
        }

        /** @brief Tests separation against earlier accepted points in stable order with bounded cancellation checkpoints. */
        Result<bool> DistanceMatch(const PCGDistanceFilterSettings &settings, const PCGPointReadView &source, Math::Vec3 point,
                                   std::span<const std::size_t> accepted, CancellationToken cancellation) {
            for (const auto prior : accepted) {
                if (cancellation.IsCancellationRequested())
                    return Reject<bool>(PCGErrors::CpuEvaluationClosed);
                if (Distance(point, source.Transforms()[prior].translation) < settings.minimumDistance)
                    return Result<bool>::Success(false);
            }
            return Result<bool>::Success(true);
        }

        /** @brief Tests one point against admitted immutable inputs and previously accepted indices. */
        Result<bool> Selected(const PCGPointFilterSettings &settings, const PCGPointReadView &source, const PCGSpatialSnapshot &spatial,
                              std::size_t index, std::span<const std::size_t> accepted, CancellationToken cancellation) {
            return std::visit([&]<class T>(const T &s) -> Result<bool> {
                const auto point = source.Transforms()[index].translation;
                if constexpr (std::is_same_v<T, PCGDensityRangeFilterSettings>)
                    return Result<bool>::Success(source.Densities()[index] >= s.minimum && source.Densities()[index] <= s.maximum);
                else if constexpr (std::is_same_v<T, PCGBoundsFilterSettings>) {
                    const auto &b = s.bounds;
                    return Result<bool>::Success(point.x >= b.minimum.x && point.x <= b.maximum.x && point.y >= b.minimum.y &&
                                                 point.y <= b.maximum.y && point.z >= b.minimum.z && point.z <= b.maximum.z);
                } else if constexpr (std::is_same_v<T, PCGSlopeFilterSettings>)
                    return SlopeMatch(s, point, spatial, cancellation);
                else if constexpr (std::is_same_v<T, PCGAttributeFilterSettings>)
                    return AttributeMatch(s, source, index);
                else
                    return DistanceMatch(s, source, point, accepted, cancellation);
            }, settings);
        }
    }  // namespace

    /** @copydoc ChargePointFilter */
    Result<PCGNodeCost> ChargePointFilter(PCGCpuNodeKind kind, std::size_t points, std::size_t spatialElements) {
        const auto base = CheckedPCGMultiply(points, 2);
        const auto bytes = CheckedPCGMultiply(points, sizeof(std::size_t));
        if (base.HasError())
            return Result<PCGNodeCost>::Failure(base.ErrorValue());
        if (bytes.HasError())
            return Result<PCGNodeCost>::Failure(bytes.ErrorValue());
        const auto scratch = CheckedPCGAdd(bytes.Value(), 1024);
        if (scratch.HasError())
            return Result<PCGNodeCost>::Failure(scratch.ErrorValue());
        const auto visits = kind == PCGCpuNodeKind::DistanceFilter ? CheckedPCGMultiply(points, points) : Result<std::size_t>::Success(0);
        if (visits.HasError())
            return Result<PCGNodeCost>::Failure(visits.ErrorValue());
        const auto total = CheckedPCGAdd(base.Value(), visits.Value());
        if (total.HasError())
            return Result<PCGNodeCost>::Failure(total.ErrorValue());
        const auto snapshot =
            kind == PCGCpuNodeKind::SlopeFilter ? CheckedPCGMultiply(points, spatialElements) : Result<std::size_t>::Success(0);
        if (snapshot.HasError())
            return Result<PCGNodeCost>::Failure(snapshot.ErrorValue());
        return Result<PCGNodeCost>::Success({total.Value(), snapshot.Value(), scratch.Value()});
    }

    /** @copydoc ValidatePointFilter */
    Result<void> ValidatePointFilter(const PCGPointFilterSettings &settings, const PCGPointSchema &schema,
                                     const PCGSpatialSnapshot &spatial) {
        if (const auto *attribute = std::get_if<PCGAttributeFilterSettings>(&settings)) {
            const auto *found = schema.Find(attribute->key.Value());
            if (found == nullptr)
                return Reject<void>(PCGErrors::PointAttributeUnknown);
            if (static_cast<std::size_t>(found->type) != attribute->value.index())
                return Reject<void>(PCGErrors::PointAttributeTypeMismatch);
        }
        if (std::holds_alternative<PCGSlopeFilterSettings>(settings) && spatial.Surfaces().empty())
            return Reject<void>(PCGErrors::SpatialCoverageUnavailable);
        return Result<void>::Success();
    }

    /** @copydoc SelectPointFilter */
    Result<std::vector<std::size_t>> SelectPointFilter(const PCGPointFilterSettings &settings, const PCGPointReadView &source,
                                                       const PCGSpatialSnapshot &spatial, CancellationToken cancellation) {
        try {
            std::vector<std::size_t> accepted;
            accepted.reserve(source.PointCount());
            for (std::size_t index = 0; index < source.PointCount(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Reject<std::vector<std::size_t>>(PCGErrors::CpuEvaluationClosed);
                const auto selected = Selected(settings, source, spatial, index, accepted, cancellation);
                if (selected.HasError())
                    return Result<std::vector<std::size_t>>::Failure(selected.ErrorValue());
                if (selected.Value())
                    accepted.push_back(index);
            }
            return Result<std::vector<std::size_t>>::Success(std::move(accepted));
        } catch (const std::bad_alloc &) {
            return Reject<std::vector<std::size_t>>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
    }

    /** @copydoc CountSelected */
    Result<std::size_t> CountSelected(const PCGPointReadView &source, double threshold, std::uint32_t workers, JobSystem *jobs,
                                      const CancellationToken &cancellation) {
        const auto densities = source.Densities();
        if (workers == 1)
            return CountPartition(densities, threshold, 0, densities.size(), cancellation);
        std::array<std::size_t, 8> counts{};
        // Each child owns one disjoint count slot; the group drains before counts or source storage retire.
        TaskGroup group(*jobs, TaskGroupFailurePolicy::FailFast, cancellation);
        for (std::uint32_t partition = 0; partition < workers; ++partition) {
            const std::size_t begin = partition == 0 ? 0 : PartitionEnd(densities.size(), workers, partition - 1);
            const std::size_t end = PartitionEnd(densities.size(), workers, partition);
            auto submitted = group.Spawn({}, [densities, threshold, begin, end, partition, &counts](const CancellationToken &child) {
                const auto selected = CountPartition(densities, threshold, begin, end, child);
                if (selected.HasError())
                    return Result<void>::Failure(selected.ErrorValue());
                counts[partition] = selected.Value();
                return Result<void>::Success();
            });
            if (submitted.HasError()) {
                group.RequestCancel();
                static_cast<void>(group.Join());
                return Result<std::size_t>::Failure(submitted.ErrorValue());
            }
        }
        if (const auto joined = group.Join(); joined.HasError())
            return Result<std::size_t>::Failure(joined.ErrorValue());
        std::size_t total{};
        for (std::uint32_t partition = 0; partition < workers; ++partition)
            total += counts[partition];
        return Result<std::size_t>::Success(total);
    }
}  // namespace Horo::PCG::detail
