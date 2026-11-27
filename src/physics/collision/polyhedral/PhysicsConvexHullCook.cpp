#include "Horo/Physics/PhysicsConvexHullCook.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsConvexHullCookInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <tuple>
#include <utility>

namespace Horo::Physics {
    namespace {
        constexpr std::array<std::uint8_t, 4> CookKeyMagic{'P', 'H', 'C', 'K'};
        constexpr std::size_t MaximumSourceContextBytes = 256;

        struct Face final {
            std::uint32_t a{};
            std::uint32_t b{};
            std::uint32_t c{};
            double nx{};
            double ny{};
            double nz{};
        };

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] std::string SourceMessage(const PhysicsConvexHullCookRequest &request, const std::string_view reason) {
            const auto context = request.sourceContext.empty() ? std::string{"<unnamed>"}
                                                               : std::string{request.sourceContext.substr(0, MaximumSourceContextBytes)};
            return std::format("Physics convex source '{}' (asset {}, subresource {}): {}", context, request.asset.ToString(),
                               request.subresource.Value(), reason);
        }

        using Writer = Detail::ConvexPayloadWriter;

        [[nodiscard]] bool KnownSettings(const PhysicsConvexHullCookSettings &settings) noexcept {
            const auto &limits = settings.limits;
            return settings.schemaVersion == PhysicsConvexHullCookSettings::CurrentSchemaVersion &&
                   settings.algorithmVersion == PhysicsConvexHullCookSettings::CurrentAlgorithmVersion &&
                   settings.limitPolicy == PhysicsConvexHullLimitPolicy::Fail && Detail::ConvexLimitsAreBounded(limits);
        }

        [[nodiscard]] double SquaredDistance(const Math::Vec3 left, const Math::Vec3 right) noexcept {
            const double x = static_cast<double>(left.x) - right.x;
            const double y = static_cast<double>(left.y) - right.y;
            const double z = static_cast<double>(left.z) - right.z;
            return x * x + y * y + z * z;
        }

        [[nodiscard]] double DotFrom(const Face &face, const Math::Vec3 origin, const Math::Vec3 point) noexcept {
            return face.nx * (static_cast<double>(point.x) - origin.x) + face.ny * (static_cast<double>(point.y) - origin.y) +
                   face.nz * (static_cast<double>(point.z) - origin.z);
        }

        [[nodiscard]] Face MakeFace(std::uint32_t a, std::uint32_t b, std::uint32_t c, const Math::Vec3 interior,
                                    const std::span<const Math::Vec3> points) noexcept {
            auto normal = Detail::ConvexCross(points[a], points[b], points[c]);
            Face face{a, b, c, normal[0], normal[1], normal[2]};
            if (DotFrom(face, points[a], interior) > 0.0) {
                std::swap(face.b, face.c);
                normal = Detail::ConvexCross(points[face.a], points[face.b], points[face.c]);
                face.nx = normal[0];
                face.ny = normal[1];
                face.nz = normal[2];
            }
            return face;
        }

        [[nodiscard]] bool IsOutside(const Face &face, const Math::Vec3 point, const std::span<const Math::Vec3> points) noexcept {
            const double normalLength = std::sqrt(face.nx * face.nx + face.ny * face.ny + face.nz * face.nz);
            return DotFrom(face, points[face.a], point) > Detail::ConvexHullPlaneToleranceMeters * normalLength;
        }

        struct InitialHull final {
            std::array<std::uint32_t, 4> points{};
            Math::Vec3 interior;
        };

        [[nodiscard]] std::pair<std::uint32_t, double> FarthestPoint(const std::span<const Math::Vec3> points,
                                                                     const std::uint32_t origin) noexcept {
            std::pair<std::uint32_t, double> result{origin, 0.0};
            for (std::uint32_t index = 0; index < points.size(); ++index) {
                const double distance = SquaredDistance(points[origin], points[index]);
                if (distance > result.second)
                    result = {index, distance};
            }
            return result;
        }

        [[nodiscard]] std::pair<std::uint32_t, double> FarthestFromLine(const std::span<const Math::Vec3> points, const std::uint32_t first,
                                                                        const std::uint32_t second) noexcept {
            std::pair<std::uint32_t, double> result{first, 0.0};
            for (std::uint32_t index = 0; index < points.size(); ++index) {
                const auto cross = Detail::ConvexCross(points[first], points[second], points[index]);
                const double area = cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
                if (area > result.second)
                    result = {index, area};
            }
            return result;
        }

        [[nodiscard]] std::pair<std::uint32_t, double> FarthestFromPlane(const std::span<const Math::Vec3> points,
                                                                         const std::array<std::uint32_t, 3> base) noexcept {
            const auto normal = Detail::ConvexCross(points[base[0]], points[base[1]], points[base[2]]);
            const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            std::pair<std::uint32_t, double> result{base[0], 0.0};
            for (std::uint32_t index = 0; index < points.size(); ++index) {
                const double x = static_cast<double>(points[index].x) - points[base[0]].x;
                const double y = static_cast<double>(points[index].y) - points[base[0]].y;
                const double z = static_cast<double>(points[index].z) - points[base[0]].z;
                const double distance = std::abs(normal[0] * x + normal[1] * y + normal[2] * z) / length;
                if (distance > result.second)
                    result = {index, distance};
            }
            return result;
        }

        [[nodiscard]] Result<InitialHull> SelectInitialHull(const PhysicsConvexHullCookRequest &request,
                                                            const std::span<const Math::Vec3> points) {
            if (points.size() < 4)
                return Failure<InitialHull>(PhysicsErrors::ShapeCookSourceInvalid,
                                            SourceMessage(request, "fewer than four distinct finite vertices form no volume."));
            const auto [p1Index, p1Distance] = FarthestPoint(points, 0);
            if (p1Distance <= Detail::ConvexHullPlaneToleranceMeters * Detail::ConvexHullPlaneToleranceMeters)
                return Failure<InitialHull>(PhysicsErrors::ShapeCookSourceInvalid,
                                            SourceMessage(request, "vertices collapse to one point at the cook tolerance."));
            const auto [p2Index, p2Distance] = FarthestFromLine(points, 0, p1Index);
            if (p2Distance <= Detail::ConvexHullPlaneToleranceMeters * Detail::ConvexHullPlaneToleranceMeters * p1Distance)
                return Failure<InitialHull>(PhysicsErrors::ShapeCookSourceInvalid,
                                            SourceMessage(request, "vertices are collinear at the cook tolerance."));
            const auto [p3Index, p3Distance] = FarthestFromPlane(points, {0, p1Index, p2Index});
            if (p3Distance <= Detail::ConvexHullPlaneToleranceMeters)
                return Failure<InitialHull>(PhysicsErrors::ShapeCookSourceInvalid,
                                            SourceMessage(request, "vertices are coplanar at the cook tolerance."));
            const auto quarter = [](const float a, const float b, const float c, const float d) {
                return a * 0.25F + b * 0.25F + c * 0.25F + d * 0.25F;
            };
            return Result<InitialHull>::Success(
                {.points = {0, p1Index, p2Index, p3Index},
                 .interior = {quarter(points[0].x, points[p1Index].x, points[p2Index].x, points[p3Index].x),
                              quarter(points[0].y, points[p1Index].y, points[p2Index].y, points[p3Index].y),
                              quarter(points[0].z, points[p1Index].z, points[p2Index].z, points[p3Index].z)}});
        }

        [[nodiscard]] std::vector<Face> CreateInitialFaces(const InitialHull &initial, const std::span<const Math::Vec3> points,
                                                           const std::uint32_t maximumVertices) {
            const auto [p0, p1, p2, p3] = initial.points;
            std::vector<Face> faces;
            faces.reserve(maximumVertices * 2U - 4U);
            faces.push_back(MakeFace(p0, p1, p2, initial.interior, points));
            faces.push_back(MakeFace(p0, p3, p1, initial.interior, points));
            faces.push_back(MakeFace(p0, p2, p3, initial.interior, points));
            faces.push_back(MakeFace(p1, p3, p2, initial.interior, points));
            return faces;
        }

        [[nodiscard]] bool FindVisibleFaces(const std::span<const Face> faces, const Math::Vec3 point,
                                            const std::span<const Math::Vec3> points, std::vector<bool> &visible) {
            visible.assign(faces.size(), false);
            bool any = false;
            for (std::size_t index = 0; index < faces.size(); ++index) {
                visible[index] = IsOutside(faces[index], point, points);
                any = any || visible[index];
            }
            return any;
        }

        using Horizon = std::map<std::pair<std::uint32_t, std::uint32_t>, bool>;

        struct HullScratch final {
            std::vector<bool> visible;
            Horizon horizon;
            std::vector<Face> retained;
        };

        void AddHorizonEdge(Horizon &horizon, const std::uint32_t from, const std::uint32_t to) {
            if (const auto reverse = horizon.find({to, from}); reverse != horizon.end())
                horizon.erase(reverse);
            else
                horizon.emplace(std::pair{from, to}, true);
        }

        void BuildHorizon(const std::span<const Face> faces, const std::vector<bool> &visible, Horizon &horizon) {
            horizon.clear();
            for (std::size_t index = 0; index < faces.size(); ++index) {
                if (!visible[index])
                    continue;
                AddHorizonEdge(horizon, faces[index].a, faces[index].b);
                AddHorizonEdge(horizon, faces[index].b, faces[index].c);
                AddHorizonEdge(horizon, faces[index].c, faces[index].a);
            }
        }

        void ReplaceVisibleFaces(const std::span<const Face> faces, const std::vector<bool> &visible, const Horizon &horizon,
                                 const std::uint32_t point, const Math::Vec3 interior, const std::span<const Math::Vec3> points,
                                 std::vector<Face> &retained) {
            retained.clear();
            for (std::size_t index = 0; index < faces.size(); ++index) {
                if (!visible[index])
                    retained.push_back(faces[index]);
            }
            for (const auto &[edge, unused] : horizon) {
                static_cast<void>(unused);
                retained.push_back(MakeFace(edge.first, edge.second, point, interior, points));
            }
        }

        [[nodiscard]] Result<void> ValidateWorkingHull(const PhysicsConvexHullCookRequest &request, const std::span<const Face> faces) {
            std::set<std::uint32_t> activeVertices;
            for (const auto &face : faces) {
                activeVertices.insert(face.a);
                activeVertices.insert(face.b);
                activeVertices.insert(face.c);
            }
            const auto maximumTriangles = 2U * request.settings.limits.maxHullVertices - 4U;
            if (activeVertices.size() <= request.settings.limits.maxHullVertices && faces.size() <= maximumTriangles)
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(PhysicsErrors::ShapeCookLimitExceeded,
                          SourceMessage(request,
                                        std::format("incremental hull reached {} vertices and {} triangles beyond configured limits "
                                                    "({}, {}); fail policy forbids simplification.",
                                                    activeVertices.size(), faces.size(), request.settings.limits.maxHullVertices,
                                                    maximumTriangles))));
        }

        [[nodiscard]] Result<void> AddHullPoint(const PhysicsConvexHullCookRequest &request, const std::uint32_t point,
                                                const Math::Vec3 interior, const std::span<const Math::Vec3> points,
                                                const CancellationToken &cancellation, HullScratch &scratch, std::vector<Face> &faces) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookCancelled, SourceMessage(request, "cook was cancelled before hull completion.")));
            if (!FindVisibleFaces(faces, points[point], points, scratch.visible))
                return Result<void>::Success();
            BuildHorizon(faces, scratch.visible, scratch.horizon);
            ReplaceVisibleFaces(faces, scratch.visible, scratch.horizon, point, interior, points, scratch.retained);
            if (const auto budget = ValidateWorkingHull(request, scratch.retained); budget.HasError())
                return budget;
            faces.swap(scratch.retained);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<Face>> BuildHull(const PhysicsConvexHullCookRequest &request, std::vector<Math::Vec3> &points,
                                                          const CancellationToken &cancellation) {
            std::ranges::sort(points, [](const Math::Vec3 left, const Math::Vec3 right) {
                return std::tuple{left.x, left.y, left.z} < std::tuple{right.x, right.y, right.z};
            });
            points.erase(std::ranges::unique(points).begin(), points.end());
            auto initialResult = SelectInitialHull(request, points);
            if (initialResult.HasError())
                return Result<std::vector<Face>>::Failure(initialResult.ErrorValue());
            const auto initial = std::move(initialResult).Value();
            auto faces = CreateInitialFaces(initial, points, request.settings.limits.maxHullVertices);
            const auto maximumTriangles = 2U * request.settings.limits.maxHullVertices - 4U;
            HullScratch scratch;
            scratch.visible.reserve(maximumTriangles);
            scratch.retained.reserve(maximumTriangles);
            for (std::uint32_t point = 0; point < points.size(); ++point) {
                if (std::ranges::find(initial.points, point) != initial.points.end())
                    continue;
                if (const auto added = AddHullPoint(request, point, initial.interior, points, cancellation, scratch, faces);
                    added.HasError())
                    return Result<std::vector<Face>>::Failure(added.ErrorValue());
            }
            return Result<std::vector<Face>>::Success(std::move(faces));
        }

        [[nodiscard]] Sha256Digest DigestWriter(const Writer &writer) noexcept {
            return ComputeSha256(std::as_bytes(std::span{writer.View()}));
        }

        [[nodiscard]] Sha256Digest ComputeCookKey(const PhysicsConvexHullCookRequest &request, const Sha256Digest &sourceDigest) {
            Writer writer;
            writer.Bytes(CookKeyMagic);
            writer.U32(1);
            writer.Bytes(request.asset.Bytes());
            writer.U64(request.subresource.Value());
            writer.Bytes(sourceDigest.bytes);
            writer.U32(request.settings.schemaVersion);
            writer.U32(request.settings.algorithmVersion);
            writer.U8(static_cast<std::uint8_t>(request.settings.limitPolicy));
            writer.U32(request.settings.limits.maxSourceVertices);
            writer.U32(request.settings.limits.maxHullVertices);
            writer.U64(request.settings.limits.maxPayloadBytes);
            writer.Bytes(request.target.digest.bytes);
            return DigestWriter(writer);
        }

        [[nodiscard]] std::array<std::uint32_t, 3> CanonicalTriangle(std::array<std::uint32_t, 3> triangle) noexcept {
            const auto minimum = std::ranges::min_element(triangle);
            std::ranges::rotate(triangle, minimum);
            return triangle;
        }

        struct ValidatedSource final {
            std::vector<Math::Vec3> points;
            Sha256Digest digest;
        };

        struct CookedHull final {
            std::vector<Math::Vec3> vertices;
            std::vector<std::array<std::uint32_t, 3>> triangles;
            Math::Aabb bounds;
        };

        [[nodiscard]] Result<void> ValidateRequest(const PhysicsConvexHullCookRequest &request, const CancellationToken &cancellation) {
            if (!request.asset.IsValid() || !request.subresource.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                                                       SourceMessage(request, "asset and subresource identities must be non-zero.")));
            if (request.sourceContext.size() > MaximumSourceContextBytes)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                                                       SourceMessage(request, "diagnostic source context exceeds 256 bytes.")));
            if (!KnownSettings(request.settings))
                return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported,
                                                       SourceMessage(request, "cook settings or fail-only limit policy are unsupported.")));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookCancelled, SourceMessage(request, "cook was cancelled before validation.")));
            if (request.vertices.empty())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid, SourceMessage(request, "vertex input is empty.")));
            if (request.vertices.size() <= request.settings.limits.maxSourceVertices)
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(PhysicsErrors::ShapeCookLimitExceeded,
                          SourceMessage(request, std::format("{} source vertices exceed the configured limit of {}.",
                                                             request.vertices.size(), request.settings.limits.maxSourceVertices))));
        }

        [[nodiscard]] Result<ValidatedSource> CaptureSource(const PhysicsConvexHullCookRequest &request,
                                                            const CancellationToken &cancellation) {
            ValidatedSource source;
            source.points.reserve(request.vertices.size());
            Writer sourceBytes;
            sourceBytes.Reserve(sizeof(std::uint32_t) + request.vertices.size() * 3U * sizeof(float));
            sourceBytes.U32(static_cast<std::uint32_t>(request.vertices.size()));
            for (std::size_t index = 0; index < request.vertices.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return Failure<ValidatedSource>(PhysicsErrors::ShapeCookCancelled,
                                                    SourceMessage(request, "cook was cancelled during source validation."));
                auto vertex = request.vertices[index];
                if (!Math::IsFinite(vertex))
                    return Failure<ValidatedSource>(PhysicsErrors::ShapeCookSourceInvalid,
                                                    SourceMessage(request,
                                                                  std::format("vertex {} contains a non-finite coordinate.", index)));
                if (vertex.x == 0.0F)
                    vertex.x = 0.0F;
                if (vertex.y == 0.0F)
                    vertex.y = 0.0F;
                if (vertex.z == 0.0F)
                    vertex.z = 0.0F;
                sourceBytes.Float(vertex.x);
                sourceBytes.Float(vertex.y);
                sourceBytes.Float(vertex.z);
                source.points.push_back(vertex);
            }
            source.digest = DigestWriter(sourceBytes);
            return Result<ValidatedSource>::Success(std::move(source));
        }

        [[nodiscard]] std::set<std::uint32_t> CollectUsedVertices(const std::span<const Face> faces) {
            std::set<std::uint32_t> used;
            for (const auto &face : faces) {
                used.insert(face.a);
                used.insert(face.b);
                used.insert(face.c);
            }
            return used;
        }

        [[nodiscard]] std::vector<Math::Vec3> RemapVertices(const std::span<const Math::Vec3> points, const std::set<std::uint32_t> &used,
                                                            std::map<std::uint32_t, std::uint32_t> &remap) {
            std::vector<Math::Vec3> vertices;
            vertices.reserve(used.size());
            for (const auto sourceIndex : used) {
                remap.emplace(sourceIndex, static_cast<std::uint32_t>(vertices.size()));
                vertices.push_back(points[sourceIndex]);
            }
            return vertices;
        }

        [[nodiscard]] std::vector<std::array<std::uint32_t, 3>> RemapTriangles(const std::span<const Face> faces,
                                                                               const std::map<std::uint32_t, std::uint32_t> &remap) {
            std::vector<std::array<std::uint32_t, 3>> triangles;
            triangles.reserve(faces.size());
            for (const auto &face : faces)
                triangles.push_back(CanonicalTriangle({remap.at(face.a), remap.at(face.b), remap.at(face.c)}));
            std::ranges::sort(triangles);
            return triangles;
        }

        [[nodiscard]] Math::Aabb ComputeBounds(const std::span<const Math::Vec3> vertices) noexcept {
            Math::Aabb bounds{.minimum = vertices.front(), .maximum = vertices.front()};
            for (const auto vertex : vertices) {
                bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
                bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
                bounds.minimum.z = std::min(bounds.minimum.z, vertex.z);
                bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
                bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
                bounds.maximum.z = std::max(bounds.maximum.z, vertex.z);
            }
            return bounds;
        }

        [[nodiscard]] Result<CookedHull> MakeCookedHull(const PhysicsConvexHullCookRequest &request,
                                                        const std::span<const Math::Vec3> points, const std::span<const Face> faces) {
            const auto used = CollectUsedVertices(faces);
            if (used.size() > request.settings.limits.maxHullVertices)
                return Failure<
                    CookedHull>(PhysicsErrors::ShapeCookLimitExceeded,
                                SourceMessage(request,
                                              std::format("{} hull vertices exceed the configured limit of {}; fail policy forbids "
                                                          "simplification.",
                                                          used.size(), request.settings.limits.maxHullVertices)));
            std::map<std::uint32_t, std::uint32_t> remap;
            auto vertices = RemapVertices(points, used, remap);
            auto triangles = RemapTriangles(faces, remap);
            const auto bounds = ComputeBounds(vertices);
            return Result<CookedHull>::Success({.vertices = std::move(vertices), .triangles = std::move(triangles), .bounds = bounds});
        }

        [[nodiscard]] std::size_t PayloadSize(const CookedHull &hull) noexcept {
            return Detail::ConvexPayloadHeaderBytes + hull.vertices.size() * 3U * sizeof(float) +
                   hull.triangles.size() * 3U * sizeof(std::uint32_t);
        }

        void WritePayloadHeader(Writer &payload, const PhysicsConvexHullCookRequest &request, const Sha256Digest &sourceDigest,
                                const Sha256Digest &cookKey, const CookedHull &hull) {
            payload.Bytes(Detail::ConvexPayloadMagic);
            payload.U32(Detail::ConvexPayloadVersion);
            payload.U32(request.settings.schemaVersion);
            payload.U32(request.settings.algorithmVersion);
            payload.Bytes(request.asset.Bytes());
            payload.U64(request.subresource.Value());
            payload.Bytes(sourceDigest.bytes);
            payload.Bytes(cookKey.bytes);
            payload.Bytes(request.target.digest.bytes);
            payload.U32(static_cast<std::uint32_t>(request.vertices.size()));
            payload.U32(static_cast<std::uint32_t>(hull.vertices.size()));
            payload.U32(static_cast<std::uint32_t>(hull.triangles.size()));
            payload.Float(hull.bounds.minimum.x);
            payload.Float(hull.bounds.minimum.y);
            payload.Float(hull.bounds.minimum.z);
            payload.Float(hull.bounds.maximum.x);
            payload.Float(hull.bounds.maximum.y);
            payload.Float(hull.bounds.maximum.z);
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePayload(const PhysicsConvexHullCookRequest &request,
                                                                      const Sha256Digest &sourceDigest, const Sha256Digest &cookKey,
                                                                      const CookedHull &hull) {
            const auto payloadBytes = PayloadSize(hull);
            if (payloadBytes > request.settings.limits.maxPayloadBytes)
                return Failure<std::vector<std::uint8_t>>(PhysicsErrors::ShapeCookLimitExceeded,
                                                          SourceMessage(request,
                                                                        std::format("{} artifact bytes exceed the configured limit of {}.",
                                                                                    payloadBytes,
                                                                                    request.settings.limits.maxPayloadBytes)));
            Writer payload;
            payload.Reserve(payloadBytes);
            WritePayloadHeader(payload, request, sourceDigest, cookKey, hull);
            for (const auto vertex : hull.vertices) {
                payload.Float(vertex.x);
                payload.Float(vertex.y);
                payload.Float(vertex.z);
            }
            for (const auto triangle : hull.triangles) {
                payload.U32(triangle[0]);
                payload.U32(triangle[1]);
                payload.U32(triangle[2]);
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(payload).Take());
        }

        [[nodiscard]] PhysicsConvexHullCookResult MakeCookResult(const PhysicsConvexHullCookRequest &request,
                                                                 const Sha256Digest &sourceDigest, const Sha256Digest &cookKey,
                                                                 const CookedHull &hull, std::vector<std::uint8_t> payload) noexcept {
            const auto payloadDigest = ComputeSha256(std::as_bytes(std::span{payload}));
            return {.descriptor = {.asset = request.asset,
                                   .subresource = request.subresource,
                                   .kind = PhysicsCookedShapeKind::ConvexHull,
                                   .cacheKeyDigest = cookKey,
                                   .payloadDigest = payloadDigest,
                                   .target = request.target},
                    .sourceDigest = sourceDigest,
                    .bounds = hull.bounds,
                    .sourceVertexCount = static_cast<std::uint32_t>(request.vertices.size()),
                    .hullVertexCount = static_cast<std::uint32_t>(hull.vertices.size()),
                    .triangleCount = static_cast<std::uint32_t>(hull.triangles.size()),
                    .payload = std::move(payload)};
        }

    }  // namespace

    /** @copydoc CookPhysicsConvexHull */
    Result<PhysicsConvexHullCookResult> CookPhysicsConvexHull(const PhysicsConvexHullCookRequest &request,
                                                              const CancellationToken &cancellation) {
        if (const auto valid = ValidateRequest(request, cancellation); valid.HasError())
            return Result<PhysicsConvexHullCookResult>::Failure(valid.ErrorValue());
        try {
            auto sourceResult = CaptureSource(request, cancellation);
            if (sourceResult.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(sourceResult.ErrorValue());
            auto source = std::move(sourceResult).Value();
            auto hullResult = BuildHull(request, source.points, cancellation);
            if (hullResult.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(hullResult.ErrorValue());
            auto cookedResult = MakeCookedHull(request, source.points, std::move(hullResult).Value());
            if (cookedResult.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(cookedResult.ErrorValue());
            auto cooked = std::move(cookedResult).Value();
            const auto cookKey = ComputeCookKey(request, source.digest);
            auto payloadResult = EncodePayload(request, source.digest, cookKey, cooked);
            if (payloadResult.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(payloadResult.ErrorValue());
            return Result<PhysicsConvexHullCookResult>::Success(
                MakeCookResult(request, source.digest, cookKey, cooked, std::move(payloadResult).Value()));
        } catch (const std::bad_alloc &) {
            return Failure<PhysicsConvexHullCookResult>(PhysicsErrors::ShapeCookLimitExceeded,
                                                        SourceMessage(request, "bounded cook storage could not be allocated."));
        }
    }

}  // namespace Horo::Physics
