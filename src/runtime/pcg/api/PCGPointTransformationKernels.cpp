#include "Horo/PCG/PCGErrors.h"
#include "PCGPointTransformationInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::PCG::detail {
    namespace {
        template <class T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Fixed SplitMix64 expansion of an existing semantic sample seed, never a row index. */
        double Draw(std::uint64_t &state) {
            std::uint64_t value = (state += 0x9e3779b97f4a7c15ULL);
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return static_cast<double>((value ^ (value >> 31)) >> 11) * 0x1.0p-53;
        }

        /** @brief Selects captured surface coverage with an explicit stable-ID tie break. */
        Result<Math::Vec3> SurfaceNormal(const PCGPointAlignmentSettings &settings, Math::Vec3 point, const PCGSpatialSnapshot &spatial,
                                         CancellationToken cancellation) {
            const PCGSurfaceTriangle *selected{};
            double distance = std::numeric_limits<double>::infinity();
            for (const auto &t : spatial.Surfaces()) {
                if (cancellation.IsCancellationRequested())
                    return Reject<Math::Vec3>(PCGErrors::CpuEvaluationClosed);
                const auto a = t.vertices[0];
                const auto b = t.vertices[1];
                const auto c = t.vertices[2];
                const double bx = static_cast<double>(b.x) - a.x;
                const double bz = static_cast<double>(b.z) - a.z;
                const double cx = static_cast<double>(c.x) - a.x;
                const double cz = static_cast<double>(c.z) - a.z;
                const double px = static_cast<double>(point.x) - a.x;
                const double pz = static_cast<double>(point.z) - a.z;
                const double determinant = bx * cz - cx * bz;
                if (determinant == 0)
                    continue;
                const double u = (px * cz - cx * pz) / determinant;
                const double v = (bx * pz - px * bz) / determinant;
                if (u < 0 || v < 0 || u + v > 1)
                    continue;
                const double height = a.y + u * (static_cast<double>(b.y) - a.y) + v * (static_cast<double>(c.y) - a.y);
                const double candidate = std::abs(static_cast<double>(point.y) - height);
                if (candidate < distance || (candidate == distance && selected != nullptr && t.id < selected->id)) {
                    distance = candidate;
                    selected = &t;
                }
            }
            if (selected == nullptr || distance > settings.maximumVerticalDistance)
                return Reject<Math::Vec3>(PCGErrors::SpatialCoverageUnavailable);
            return Math::TryNormalize(selected->normal);
        }

        /** @brief Shortest rotation from current up to surface normal; antiparallel axes use a deterministic tangent. */
        Result<Math::Quaternion> Align(Math::Quaternion rotation, Math::Vec3 normal) {
            const auto up = rotation.TryRotate({0, 1, 0});
            if (up.HasError())
                return Result<Math::Quaternion>::Failure(up.ErrorValue());
            const auto unit = Math::TryNormalize(up.Value());
            if (unit.HasError())
                return Result<Math::Quaternion>::Failure(unit.ErrorValue());
            const auto a = unit.Value();
            const std::array<double, 3> cross{static_cast<double>(a.y) * normal.z - static_cast<double>(a.z) * normal.y,
                                              static_cast<double>(a.z) * normal.x - static_cast<double>(a.x) * normal.z,
                                              static_cast<double>(a.x) * normal.y - static_cast<double>(a.y) * normal.x};
            const double length = std::hypot(cross[0], cross[1], cross[2]);
            const double dot =
                static_cast<double>(a.x) * normal.x + static_cast<double>(a.y) * normal.y + static_cast<double>(a.z) * normal.z;
            Math::Quaternion delta;
            if (length == 0) {
                if (dot >= 0)
                    return rotation.TryNormalized();
                const Math::Vec3 reference = std::abs(a.x) < 0.9F ? Math::Vec3{1, 0, 0} : Math::Vec3{0, 0, 1};
                const auto axis = Math::TryNormalize(Math::Cross(a, reference));
                if (axis.HasError())
                    return Result<Math::Quaternion>::Failure(axis.ErrorValue());
                delta = {axis.Value().x, axis.Value().y, axis.Value().z, 0};
            } else {
                const double halfAngle = 0.5 * std::atan2(length, dot);
                const double factor = std::sin(halfAngle) / length;
                delta = {static_cast<float>(cross[0] * factor), static_cast<float>(cross[1] * factor),
                         static_cast<float>(cross[2] * factor), static_cast<float>(std::cos(halfAngle))};
            }
            return (delta * rotation).TryNormalized();
        }

        /** @brief Performs checked displacement in the declared frame without scaling a local offset. */
        Result<Math::Vec3> Displace(const Math::Transform &source, Math::Vec3 offset, PCGPointCoordinateSpace space) {
            if (space == PCGPointCoordinateSpace::PointLocal) {
                const auto rotated = source.rotation.TryRotate(offset);
                if (rotated.HasError())
                    return Result<Math::Vec3>::Failure(rotated.ErrorValue());
                offset = rotated.Value();
            }
            const std::array<double, 3> values{static_cast<double>(source.translation.x) + offset.x,
                                               static_cast<double>(source.translation.y) + offset.y,
                                               static_cast<double>(source.translation.z) + offset.z};
            for (const auto value : values)
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    return Reject<Math::Vec3>(PCGErrors::PointDataInvalid);
            return Result<Math::Vec3>::Success(
                {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])});
        }

        /** @brief Applies component transform semantics while retaining local bounds and original identity. */
        Result<Math::Transform> Components(const PCGPointTransformSettings &s, Math::Transform source) {
            const auto position = Displace(source, s.delta.translation, s.space);
            if (position.HasError())
                return Result<Math::Transform>::Failure(position.ErrorValue());
            const auto delta = s.delta.rotation.TryNormalized();
            const auto original = source.rotation.TryNormalized();
            if (delta.HasError() || original.HasError())
                return Reject<Math::Transform>(PCGErrors::PointDataInvalid);
            const auto composed =
                (s.space == PCGPointCoordinateSpace::Snapshot ? delta.Value() * original.Value() : original.Value() * delta.Value())
                    .TryNormalized();
            if (composed.HasError())
                return Result<Math::Transform>::Failure(composed.ErrorValue());
            source.translation = position.Value();
            source.rotation = composed.Value();
            source.scale = {source.scale.x * s.delta.scale.x, source.scale.y * s.delta.scale.y, source.scale.z * s.delta.scale.z};
            return Result<Math::Transform>::Success(source);
        }

        /** @brief Synchronous closed-settings visitor; borrows remain inside TransformPoint and never reach a worker. */
        struct PointTransformer {
            const PCGSpatialSnapshot &spatial;
            const Math::Transform &source;
            std::uint64_t seed;
            CancellationToken cancellation;

            Result<Math::Transform> operator()(const PCGPointTransformSettings &settings) const {
                return Components(settings, source);
            }

            Result<Math::Transform> operator()(const PCGPointAlignmentSettings &settings) const {
                const auto normal = SurfaceNormal(settings, source.translation, spatial, cancellation);
                if (normal.HasError())
                    return Result<Math::Transform>::Failure(normal.ErrorValue());
                const auto rotation = Align(source.rotation, normal.Value());
                if (rotation.HasError())
                    return Result<Math::Transform>::Failure(rotation.ErrorValue());
                auto value = source;
                value.rotation = rotation.Value();
                return Result<Math::Transform>::Success(value);
            }

            Result<Math::Transform> operator()(const PCGPointRandomOffsetSettings &settings) const {
                auto state = seed ^ settings.salt;
                const Math::Vec3 offset{static_cast<float>((2 * Draw(state) - 1) * settings.halfExtent.x),
                                        static_cast<float>((2 * Draw(state) - 1) * settings.halfExtent.y),
                                        static_cast<float>((2 * Draw(state) - 1) * settings.halfExtent.z)};
                const auto position = Displace(source, offset, settings.space);
                if (position.HasError())
                    return Result<Math::Transform>::Failure(position.ErrorValue());
                auto value = source;
                value.translation = position.Value();
                return Result<Math::Transform>::Success(value);
            }

            Result<Math::Transform> operator()(const PCGPointNoiseSettings &) const {
                return Result<Math::Transform>::Success(source);
            }
        };
    }  // namespace

    /** @copydoc TransformPoint */
    Result<Math::Transform> TransformPoint(const PointTransformationSettings &settings, const PCGSpatialSnapshot &spatial,
                                           const Math::Transform &source, std::uint64_t seed, CancellationToken cancellation) {
        if (cancellation.IsCancellationRequested())
            return Reject<Math::Transform>(PCGErrors::CpuEvaluationClosed);
        auto result = std::visit(PointTransformer{spatial, source, seed, cancellation}, settings);
        if (result.HasError())
            return result;
        if (const auto matrix = result.Value().TryToMatrix(); matrix.HasError() || !Math::IsFinite(matrix.Value()))
            return Reject<Math::Transform>(PCGErrors::PointDataInvalid);
        return result;
    }
}  // namespace Horo::PCG::detail
