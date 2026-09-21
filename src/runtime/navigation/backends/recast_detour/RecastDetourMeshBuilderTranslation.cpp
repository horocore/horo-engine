#include "RecastDetourMeshBuilderInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation::RecastDetourMeshBuilderInternal {
    namespace {
        [[nodiscard]] bool IsKnown(const NavigationTileBuildWarningCode code) noexcept {
            return code >= NavigationTileBuildWarningCode::NonWalkableTrianglesDiscarded && code < NavigationTileBuildWarningCode::Count;
        }

        [[nodiscard]] float SignedAreaXZ(const std::vector<Math::Vec3> &vertices, const std::vector<std::uint32_t> &indices,
                                         const std::size_t first, const std::size_t count) {
            double area{};
            for (std::size_t index = 0; index < count; ++index) {
                const Math::Vec3 &current = vertices[indices[first + index]];
                const Math::Vec3 &next = vertices[indices[first + ((index + 1U) % count)]];
                area += (static_cast<double>(current.x) * next.z) - (static_cast<double>(next.x) * current.z);
            }
            return static_cast<float>(area * 0.5);
        }

        [[nodiscard]] bool PointInTriangleXZ(const Math::Vec3 point, const std::array<Math::Vec3, 3> &triangle) noexcept {
            const auto cross = [](const Math::Vec3 first, const Math::Vec3 second, const Math::Vec3 third) {
                return (static_cast<double>(second.x) - first.x) * (static_cast<double>(third.z) - first.z) -
                       (static_cast<double>(second.z) - first.z) * (static_cast<double>(third.x) - first.x);
            };
            const double first = cross(triangle[0], triangle[1], point);
            const double second = cross(triangle[1], triangle[2], point);
            const double third = cross(triangle[2], triangle[0], point);
            constexpr double epsilon = 1.0e-5;
            return (first >= -epsilon && second >= -epsilon && third >= -epsilon) ||
                   (first <= epsilon && second <= epsilon && third <= epsilon);
        }

        [[nodiscard]] double SquaredDistanceXZ(const Math::Vec3 left, const Math::Vec3 right) noexcept {
            const double x = static_cast<double>(left.x) - right.x;
            const double z = static_cast<double>(left.z) - right.z;
            return (x * x) + (z * z);
        }

        struct SourceCellRange final {
            std::uint32_t minimumX{};
            std::uint32_t maximumX{};
            std::uint32_t minimumZ{};
            std::uint32_t maximumZ{};
        };

        struct ProvenanceIndex final {
            std::map<std::uint64_t, std::vector<const NavigationTileBuildTriangle *>> buckets;
            std::vector<const NavigationTileBuildTriangle *> overflow;
            std::vector<const NavigationTileBuildTriangle *> representatives;
            float minimumX{};
            float minimumZ{};
            float cellSize{};
        };

        [[nodiscard]] std::uint32_t CellIndex(const float coordinate, const float minimum, const float cellSize) noexcept {
            const auto index = std::floor((static_cast<double>(coordinate) - minimum) / cellSize);
            constexpr auto maximum = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
            if (!std::isfinite(index) || index <= 0.0)
                return 0;
            return index >= maximum ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(index);
        }

        [[nodiscard]] std::uint64_t CellKey(const std::uint32_t x, const std::uint32_t z) noexcept {
            return (static_cast<std::uint64_t>(x) << 32U) | z;
        }

        [[nodiscard]] SourceCellRange MakeSourceCellRange(const NavigationTileBuildTriangle &source,
                                                          const NavigationTileBuildRequest &request) noexcept {
            Math::Vec3 minimum = source.vertices[0];
            Math::Vec3 maximum = source.vertices[0];
            for (std::size_t index = 1; index < source.vertices.size(); ++index) {
                minimum.x = std::min(minimum.x, source.vertices[index].x);
                maximum.x = std::max(maximum.x, source.vertices[index].x);
                minimum.z = std::min(minimum.z, source.vertices[index].z);
                maximum.z = std::max(maximum.z, source.vertices[index].z);
            }
            return {.minimumX = CellIndex(minimum.x, request.bounds.minimum.x, request.buildGeometry.cellSizeMeters),
                    .maximumX = CellIndex(maximum.x, request.bounds.minimum.x, request.buildGeometry.cellSizeMeters),
                    .minimumZ = CellIndex(minimum.z, request.bounds.minimum.z, request.buildGeometry.cellSizeMeters),
                    .maximumZ = CellIndex(maximum.z, request.bounds.minimum.z, request.buildGeometry.cellSizeMeters)};
        }

        void AddRepresentative(ProvenanceIndex &index, const NavigationTileBuildTriangle *source) {
            for (const NavigationTileBuildTriangle *representative : index.representatives) {
                if (representative->area == source->area)
                    return;
            }
            index.representatives.push_back(source);
        }

        [[nodiscard]] bool IndexSource(ProvenanceIndex &index, const NavigationTileBuildTriangle *source,
                                       const NavigationTileBuildRequest &request) {
            const SourceCellRange range = MakeSourceCellRange(*source, request);
            const std::uint64_t width = static_cast<std::uint64_t>(range.maximumX) - range.minimumX + 1U;
            const std::uint64_t height = static_cast<std::uint64_t>(range.maximumZ) - range.minimumZ + 1U;
            if (std::uint64_t cellCount{}; width > 8U || height > 8U || !TryMultiply(width, height, cellCount) || cellCount > 64U)
                return false;
            for (std::uint64_t x = range.minimumX; x <= range.maximumX; ++x) {
                for (std::uint64_t z = range.minimumZ; z <= range.maximumZ; ++z)
                    index.buckets[CellKey(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z))].push_back(source);
            }
            return true;
        }

        [[nodiscard]] ProvenanceIndex BuildProvenanceIndex(const NavigationTileBuildRequest &request, const PreparedTriangles &prepared) {
            ProvenanceIndex index{.minimumX = request.bounds.minimum.x,
                                  .minimumZ = request.bounds.minimum.z,
                                  .cellSize = request.buildGeometry.cellSizeMeters};
            for (const NavigationTileBuildTriangle *source : prepared.sources) {
                if (source == nullptr)
                    continue;
                AddRepresentative(index, source);
                if (!IndexSource(index, source, request))
                    index.overflow.push_back(source);
            }
            return index;
        }

        [[nodiscard]] std::vector<const NavigationTileBuildTriangle *> Candidates(const ProvenanceIndex &index, const Math::Vec3 center) {
            std::vector<const NavigationTileBuildTriangle *> candidates;
            if (const auto bucket = index.buckets.find(
                    CellKey(CellIndex(center.x, index.minimumX, index.cellSize), CellIndex(center.z, index.minimumZ, index.cellSize)));
                bucket != index.buckets.end()) {
                candidates.insert(candidates.end(), bucket->second.begin(), bucket->second.end());
            } else {
                candidates.insert(candidates.end(), index.overflow.begin(), index.overflow.end());
            }
            candidates.insert(candidates.end(), index.representatives.begin(), index.representatives.end());
            return candidates;
        }

        [[nodiscard]] const NavigationTileBuildTriangle *FindProvenanceTriangle(const NavigationTileBuildResult &result,
                                                                                const std::size_t polygonIndex,
                                                                                const ProvenanceIndex &index, const NavigationAreaId area) {
            const NavMeshPolygon &polygon = result.polygons[polygonIndex];
            const auto indices = std::span<const std::uint32_t>(result.polygonVertexIndices)
                                     .subspan(polygon.vertexIndices.first, polygon.vertexIndices.count);
            Math::Vec3 center{};
            for (const std::uint32_t vertexIndex : indices)
                center += result.vertices[vertexIndex];
            center /= static_cast<float>(indices.size());

            const NavigationTileBuildTriangle *nearestPreferred{};
            const NavigationTileBuildTriangle *nearestFallback{};
            double nearestPreferredDistance = std::numeric_limits<double>::max();
            double nearestFallbackDistance = std::numeric_limits<double>::max();
            const auto candidates = Candidates(index, center);
            for (const NavigationTileBuildTriangle *triangle : candidates) {
                if (triangle == nullptr)
                    continue;
                if (PointInTriangleXZ(center, triangle->vertices)) {
                    if (triangle->area == area)
                        return triangle;
                    if (nearestFallback == nullptr)
                        nearestFallback = triangle;
                    continue;
                }
                Math::Vec3 triangleCenter{};
                for (const Math::Vec3 vertex : triangle->vertices)
                    triangleCenter += vertex;
                triangleCenter /= 3.0F;
                const double distance = SquaredDistanceXZ(center, triangleCenter);
                if (triangle->area == area) {
                    if (distance < nearestPreferredDistance) {
                        nearestPreferredDistance = distance;
                        nearestPreferred = triangle;
                    }
                } else if (distance < nearestFallbackDistance) {
                    nearestFallbackDistance = distance;
                    nearestFallback = triangle;
                }
            }
            return nearestFallback != nullptr ? nearestFallback : nearestPreferred;
        }

        void AppendWarning(std::vector<NavigationTileBuildWarning> &warnings, const NavigationTileBuildWarningCode code,
                           const std::uint32_t occurrences) {
            if (occurrences == 0 || !IsKnown(code))
                return;
            const auto found = std::ranges::find(warnings, code, &NavigationTileBuildWarning::code);
            if (found == warnings.end())
                warnings.push_back({.code = code, .occurrences = occurrences});
            else if (std::numeric_limits<std::uint32_t>::max() - found->occurrences < occurrences)
                found->occurrences = std::numeric_limits<std::uint32_t>::max();
            else
                found->occurrences += occurrences;
        }

        [[nodiscard]] Result<void> TranslateVertices(NavigationTileBuildResult &result, const NavigationTileBuildRequest &request,
                                                     const rcPolyMesh &mesh) {
            result.vertices.reserve(static_cast<std::size_t>(mesh.nverts));
            for (int index = 0; index < mesh.nverts; ++index) {
                const std::size_t offset = static_cast<std::size_t>(index) * 3U;
                Math::Vec3 vertex{mesh.bmin[0] + (mesh.verts[offset] * mesh.cs), mesh.bmin[1] + (mesh.verts[offset + 1U] * mesh.ch),
                                  mesh.bmin[2] + (mesh.verts[offset + 2U] * mesh.cs)};
                if (vertex.x < request.bounds.minimum.x - TileBoundsEpsilon || vertex.x > request.bounds.maximum.x + TileBoundsEpsilon ||
                    vertex.y < request.bounds.minimum.y - TileBoundsEpsilon || vertex.y > request.bounds.maximum.y + TileBoundsEpsilon ||
                    vertex.z < request.bounds.minimum.z - TileBoundsEpsilon || vertex.z > request.bounds.maximum.z + TileBoundsEpsilon)
                    return Failure<void>(NavigationErrors::ProviderFailed);
                vertex.x = std::clamp(vertex.x, request.bounds.minimum.x, request.bounds.maximum.x);
                vertex.y = std::clamp(vertex.y, request.bounds.minimum.y, request.bounds.maximum.y);
                vertex.z = std::clamp(vertex.z, request.bounds.minimum.z, request.bounds.maximum.z);
                result.vertices.push_back(vertex);
            }
            return Result<void>::Success();
        }

        struct PolygonData final {
            std::vector<std::uint32_t> vertexIndices;
            std::vector<std::uint32_t> neighbors;
            NavigationAreaId area;
        };

        [[nodiscard]] Result<PolygonData> ReadPolygon(const NavigationTileBuildResult &result, const NavigationTileBuildRequest &request,
                                                      const PreparedTriangles &prepared, const rcPolyMesh &mesh, const int polygonIndex) {
            const int nvp = mesh.nvp;
            const std::size_t polygonOffset = static_cast<std::size_t>(polygonIndex) * static_cast<std::size_t>(nvp) * 2U;
            PolygonData output;
            for (int corner = 0; corner < nvp; ++corner) {
                const unsigned short vertex = mesh.polys[polygonOffset + static_cast<std::size_t>(corner)];
                if (vertex == RC_MESH_NULL_IDX)
                    break;
                if (vertex >= result.vertices.size())
                    return Failure<PolygonData>(NavigationErrors::ProviderFailed);
                output.vertexIndices.push_back(vertex);
                const unsigned short neighbor = mesh.polys[polygonOffset + static_cast<std::size_t>(nvp + corner)];
                output.neighbors.push_back(neighbor == RC_MESH_NULL_IDX ? NavMeshBoundaryAdjacency : neighbor);
            }
            if (output.vertexIndices.size() < 3 || output.vertexIndices.size() > request.limits.maximumVerticesPerPolygon)
                return Failure<PolygonData>(NavigationErrors::ProviderFailed);
            const float signedArea = SignedAreaXZ(result.vertices, output.vertexIndices, 0, output.vertexIndices.size());
            if (!std::isfinite(signedArea) || std::abs(signedArea) <= TileBoundsEpsilon)
                return Failure<PolygonData>(NavigationErrors::ProviderFailed);
            if (signedArea < 0.0F) {
                std::ranges::reverse(output.vertexIndices);
                std::ranges::reverse(output.neighbors);
                // The reversed edge sequence starts at the old penultimate edge, not the old last edge.
                std::ranges::rotate(output.neighbors, output.neighbors.begin() + 1);
            }
            const unsigned char areaCode = mesh.areas[polygonIndex];
            const auto area = std::ranges::find(prepared.areaCodes, areaCode, &AreaCode::code);
            if (area == prepared.areaCodes.end())
                return Failure<PolygonData>(NavigationErrors::ProviderFailed);
            output.area = area->id;
            return Result<PolygonData>::Success(std::move(output));
        }

        [[nodiscard]] Result<void> TranslatePolygons(NavigationTileBuildResult &result, const NavigationTileBuildRequest &request,
                                                     const PreparedTriangles &prepared, const rcPolyMesh &mesh) {
            for (int polygonIndex = 0; polygonIndex < mesh.npolys; ++polygonIndex) {
                auto polygon = ReadPolygon(result, request, prepared, mesh, polygonIndex);
                if (polygon.HasError())
                    return Result<void>::Failure(polygon.ErrorValue());
                const auto &data = polygon.Value();
                const auto firstVertex = static_cast<std::uint32_t>(result.polygonVertexIndices.size());
                const auto firstAdjacency = static_cast<std::uint32_t>(result.polygonAdjacencies.size());
                result.polygonVertexIndices.insert(result.polygonVertexIndices.end(), data.vertexIndices.begin(), data.vertexIndices.end());
                result.polygonAdjacencies.insert(result.polygonAdjacencies.end(), data.neighbors.begin(), data.neighbors.end());
                result.polygons.push_back({.vertexIndices = {firstVertex, static_cast<std::uint32_t>(data.vertexIndices.size())},
                                           .adjacencies = {firstAdjacency, static_cast<std::uint32_t>(data.neighbors.size())},
                                           .area = data.area});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAdjacencies(const NavigationTileBuildResult &result) {
            for (const NavMeshPolygon &polygon : result.polygons) {
                for (const std::uint32_t neighbor : std::span<const std::uint32_t>(result.polygonAdjacencies)
                                                        .subspan(polygon.adjacencies.first, polygon.adjacencies.count)) {
                    if (neighbor != NavMeshBoundaryAdjacency && neighbor >= result.polygons.size())
                        return Failure<void>(NavigationErrors::ProviderFailed);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendProvenance(NavigationTileBuildResult &result, const NavigationTileBuildRequest &request,
                                                    const PreparedTriangles &prepared) {
            const ProvenanceIndex index = BuildProvenanceIndex(request, prepared);
            for (std::size_t polygonIndex = 0; polygonIndex < result.polygons.size(); ++polygonIndex) {
                const NavigationTileBuildTriangle *source =
                    FindProvenanceTriangle(result, polygonIndex, index, result.polygons[polygonIndex].area);
                if (source == nullptr)
                    return Failure<void>(NavigationErrors::ProviderFailed);
                const NavMeshSourceProvenance provenance{.kind = source->provenance.kind,
                                                         .producer = source->provenance.producer,
                                                         .contribution = source->provenance.contribution,
                                                         .revision = source->provenance.revision,
                                                         .sourceDigest = source->provenance.contentDigest,
                                                         .polygons = {static_cast<std::uint32_t>(polygonIndex), 1}};
                if (!result.provenance.empty()) {
                    NavMeshSourceProvenance &previous = result.provenance.back();
                    if (previous.kind == provenance.kind && previous.producer == provenance.producer &&
                        previous.contribution == provenance.contribution && previous.revision == provenance.revision &&
                        previous.sourceDigest == provenance.sourceDigest &&
                        previous.polygons.first + previous.polygons.count == provenance.polygons.first) {
                        ++previous.polygons.count;
                        continue;
                    }
                }
                result.provenance.push_back(provenance);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MeasureOutput(NavigationTileBuildResult &result, const NavigationTileBuildRequest &request) {
            result.statistics.vertexCount = result.vertices.size();
            result.statistics.polygonCount = result.polygons.size();
            result.statistics.polygonVertexIndexCount = result.polygonVertexIndices.size();
            result.statistics.polygonAdjacencyCount = result.polygonAdjacencies.size();
            std::uint64_t outputBytes{};
            if (const auto addOutput =
                    [&outputBytes](const std::uint64_t count, const std::uint64_t size) {
                std::uint64_t bytes{};
                return TryMultiply(count, size, bytes) && TryAdd(outputBytes, bytes, outputBytes);
            };
                !addOutput(result.vertices.capacity(), sizeof(Math::Vec3)) ||
                !addOutput(result.polygons.capacity(), sizeof(NavMeshPolygon)) ||
                !addOutput(result.polygonVertexIndices.capacity(), sizeof(std::uint32_t)) ||
                !addOutput(result.polygonAdjacencies.capacity(), sizeof(std::uint32_t)) ||
                !addOutput(result.offMeshLinks.capacity(), sizeof(NavMeshOffMeshLink)) ||
                !addOutput(result.provenance.capacity(), sizeof(NavMeshSourceProvenance)) ||
                !addOutput(result.warnings.capacity(), sizeof(NavigationTileBuildWarning)) ||
                outputBytes > request.limits.maximumOwnedBytes)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            result.statistics.outputOwnedBytes = outputBytes;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MeasureEmptyOutput(NavigationTileBuildResult &result, const NavigationTileBuildRequest &request) {
            std::uint64_t outputBytes{};
            if (!TryMultiply(result.warnings.capacity(), sizeof(NavigationTileBuildWarning), outputBytes) ||
                outputBytes > request.limits.maximumOwnedBytes)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            result.statistics.outputOwnedBytes = outputBytes;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc TranslateResult */
    Result<NavigationTileBuildResult> TranslateResult(const NavigationTileBuildRequest &request, const PreparedTriangles &prepared,
                                                      RecastPipelineResult pipeline) {
        NavigationTileBuildResult result;
        result.state = pipeline.empty ? NavigationTileBuildState::Empty : NavigationTileBuildState::Built;
        result.key = request.key;
        result.bounds = request.bounds;
        result.statistics.inputTriangleCount = request.triangles.size();
        result.statistics.walkableTriangleCount = pipeline.walkableTriangleCount;
        result.statistics.rasterizedSpanCount = pipeline.rasterizedSpanCount;
        result.statistics.regionCount = pipeline.regionCount;
        result.statistics.contourCount = pipeline.contourCount;
        AppendWarning(result.warnings, NavigationTileBuildWarningCode::NonWalkableTrianglesDiscarded,
                      static_cast<std::uint32_t>(
                          std::min<std::uint64_t>(pipeline.discardedTriangleCount, std::numeric_limits<std::uint32_t>::max())));
        AppendWarning(result.warnings, NavigationTileBuildWarningCode::RegionsDiscarded, pipeline.discardedRegionSpanCount);
        AppendWarning(result.warnings, NavigationTileBuildWarningCode::ProviderWarning, pipeline.providerWarnings);

        if (pipeline.empty) {
            AppendWarning(result.warnings, NavigationTileBuildWarningCode::EmptyTile, 1);
            if (const auto measured = MeasureEmptyOutput(result, request); measured.HasError())
                return Result<NavigationTileBuildResult>::Failure(measured.ErrorValue());
            return Result<NavigationTileBuildResult>::Success(std::move(result));
        }
        if (!pipeline.mesh || pipeline.mesh->nverts <= 0 || pipeline.mesh->npolys <= 0 ||
            static_cast<std::uint32_t>(pipeline.mesh->nverts) > request.limits.maximumVertices ||
            static_cast<std::uint32_t>(pipeline.mesh->npolys) > request.limits.maximumPolygons)
            return Failure<NavigationTileBuildResult>(NavigationErrors::CapacityExceeded);

        if (const auto vertices = TranslateVertices(result, request, *pipeline.mesh); vertices.HasError())
            return Result<NavigationTileBuildResult>::Failure(vertices.ErrorValue());
        if (const auto polygons = TranslatePolygons(result, request, prepared, *pipeline.mesh); polygons.HasError())
            return Result<NavigationTileBuildResult>::Failure(polygons.ErrorValue());
        if (const auto adjacencies = ValidateAdjacencies(result); adjacencies.HasError())
            return Result<NavigationTileBuildResult>::Failure(adjacencies.ErrorValue());
        if (const auto provenance = AppendProvenance(result, request, prepared); provenance.HasError())
            return Result<NavigationTileBuildResult>::Failure(provenance.ErrorValue());
        if (const auto measured = MeasureOutput(result, request); measured.HasError())
            return Result<NavigationTileBuildResult>::Failure(measured.ErrorValue());
        if (result.offMeshLinks.size() > request.limits.maximumOffMeshLinks)
            return Failure<NavigationTileBuildResult>(NavigationErrors::CapacityExceeded);
        return Result<NavigationTileBuildResult>::Success(std::move(result));
    }
}  // namespace Horo::Navigation::RecastDetourMeshBuilderInternal
