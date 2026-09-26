#include "OfflineVoronoiInternal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <utility>

namespace Horo::Destruction::VoronoiDetail {
    [[nodiscard]] Result<void> ValidateCollisionEdges(const OfflineVoronoiCollisionPiece &piece, Budget &budget) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<unsigned, int>> edges;
        for (const auto &triangle : piece.triangles) {
            for (const auto [from, to] :
                 {std::pair{triangle[0], triangle[1]}, std::pair{triangle[1], triangle[2]}, std::pair{triangle[2], triangle[0]}}) {
                if (!budget.ChargeWork())
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                auto &[count, winding] = edges[std::minmax(from, to)];
                ++count;
                winding += from < to ? 1 : -1;
            }
        }
        for (const auto &[edge, incidence] : edges) {
            (void)edge;
            if (incidence.first != 2 || incidence.second != 0)
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        }
        return Result<void>::Success();
    }

    [[nodiscard]] Result<std::uint32_t> FirstEdgeVertex(const OfflineVoronoiCollisionPiece &piece, const std::uint32_t startIndex,
                                                        const std::uint32_t endIndex, Budget &budget) {
        const Point start = Position(piece.positions[startIndex]);
        const Point direction = Sub(Position(piece.positions[endIndex]), start);
        const double squared = Dot(direction, direction);
        if (!(squared > 0.0))
            return Result<std::uint32_t>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        std::uint32_t splitVertex = std::numeric_limits<std::uint32_t>::max();
        double firstFraction = 1.0;
        for (std::uint32_t candidate = 0; candidate < piece.positions.size(); ++candidate) {
            if (!budget.ChargeWork())
                return Result<std::uint32_t>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (candidate == startIndex || candidate == endIndex)
                continue;
            const Point delta = Sub(Position(piece.positions[candidate]), start);
            const double fraction = Dot(delta, direction) / squared;
            if (fraction <= 1.0e-6 || fraction >= firstFraction)
                continue;
            const double distance = Length(Sub(delta, Scale(direction, fraction)));
            if (distance <= 1.0e-6 * Length(direction)) {
                splitVertex = candidate;
                firstFraction = fraction;
            }
        }
        return Result<std::uint32_t>::Success(splitVertex);
    }

    [[nodiscard]] Result<bool> SplitCollisionTriangle(OfflineVoronoiCollisionPiece &piece, const std::size_t triangleIndex,
                                                      Budget &budget) {
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto triangle = piece.triangles[triangleIndex];
            const auto start = triangle[edge];
            const auto end = triangle[(edge + 1U) % 3U];
            const auto opposite = triangle[(edge + 2U) % 3U];
            auto vertex = FirstEdgeVertex(piece, start, end, budget);
            if (vertex.HasError())
                return Result<bool>::Failure(vertex.ErrorValue());
            if (vertex.Value() == std::numeric_limits<std::uint32_t>::max())
                continue;
            if (!budget.ChargeGeometry(0, 1))
                return Result<bool>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            piece.triangles[triangleIndex] = {start, vertex.Value(), opposite};
            piece.triangles.push_back({vertex.Value(), end, opposite});
            return Result<bool>::Success(true);
        }
        return Result<bool>::Success(false);
    }

    [[nodiscard]] Result<void> CloseCollisionPiece(OfflineVoronoiCollisionPiece &piece, Budget &budget) {
        for (std::size_t triangleIndex = 0; triangleIndex < piece.triangles.size(); ++triangleIndex) {
            bool split = false;
            do {
                auto result = SplitCollisionTriangle(piece, triangleIndex, budget);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                split = result.Value();
            } while (split);
        }
        return ValidateCollisionEdges(piece, budget);
    }

    [[nodiscard]] std::size_t FaceRoot(const Face &face) {
        for (std::size_t vertex = 0; vertex < face.vertices.size(); ++vertex) {
            const Point before = face.vertices[(vertex + face.vertices.size() - 1U) % face.vertices.size()];
            const Point after = face.vertices[(vertex + 1U) % face.vertices.size()];
            if (Length(Cross(Sub(before, face.vertices[vertex]), Sub(after, face.vertices[vertex]))) > 1.0e-14)
                return vertex;
        }
        return face.vertices.size();
    }

    struct ChunkMassState final {
        Point reference{};
        double sixVolume{};
        Point weightedCenter{};
    };

    [[nodiscard]] Result<void> AppendChunkTriangle(const std::array<Point, 3> &triangle, const Face &face, ChunkMassState &mass,
                                                   OfflineVoronoiChunk &chunk, OfflineVoronoiCollisionPiece &piece,
                                                   std::map<std::array<float, 3>, std::uint32_t> &collisionVertices, Budget &budget) {
        const double volume6 =
            Dot(Sub(triangle[0], mass.reference), Cross(Sub(triangle[1], mass.reference), Sub(triangle[2], mass.reference)));
        if (!std::isfinite(volume6) || volume6 < -1.0e-12)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        if (volume6 <= 1.0e-12)
            return Result<void>::Success();
        if (!budget.ChargeWork())
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        mass.sixVolume += volume6;
        const Point tetraCenter = Scale(Add(Add(Add(mass.reference, triangle[0]), triangle[1]), triangle[2]), 0.25);
        mass.weightedCenter = Add(mass.weightedCenter, Scale(tetraCenter, volume6));
        const std::uint32_t renderFirst = static_cast<std::uint32_t>(chunk.positions.size());
        std::array<std::uint32_t, 3> collisionIndices{};
        std::size_t vertexIndex{};
        for (const auto &point : triangle) {
            std::array<float, 3> value{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                value[axis] = static_cast<float>(point[axis]);
            if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2]))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            if (const auto existing = collisionVertices.find(value); existing == collisionVertices.end()) {
                if (!budget.ChargeGeometry(1, 0))
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                collisionIndices[vertexIndex] = static_cast<std::uint32_t>(piece.positions.size());
                collisionVertices.emplace(value, collisionIndices[vertexIndex]);
                piece.positions.push_back(value);
            } else {
                collisionIndices[vertexIndex] = existing->second;
            }
            if (face.visible)
                chunk.positions.push_back(value);
            ++vertexIndex;
        }
        if (!budget.ChargeGeometry(face.visible ? 3 : 0, face.visible ? 2 : 1))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        piece.triangles.push_back(collisionIndices);
        if (face.visible)
            chunk.triangles.push_back({{renderFirst, renderFirst + 1U, renderFirst + 2U}, face.material, face.neighbor != 0});
        return Result<void>::Success();
    }

    [[nodiscard]] Point FaceReference(const std::vector<Face> &faces, std::size_t &count) {
        Point reference{};
        for (const auto &face : faces) {
            for (const auto &point : face.vertices) {
                reference = Add(reference, point);
                ++count;
            }
        }
        return count == 0 ? reference : Scale(reference, 1.0 / static_cast<double>(count));
    }

    [[nodiscard]] Result<OfflineVoronoiChunk> MakeChunk(const std::vector<Face> &faces, const Point &site,
                                                        const std::vector<DestructionChunkId> &siteIds, const std::uint32_t index,
                                                        Budget &budget) {
        OfflineVoronoiChunk chunk;
        chunk.id = siteIds[index];
        chunk.site = site;
        OfflineVoronoiCollisionPiece piece;
        if (!budget.ChargeBytes(sizeof(OfflineVoronoiCollisionPiece)))
            return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        std::map<std::array<float, 3>, std::uint32_t> collisionVertices;
        std::size_t referenceCount{};
        ChunkMassState mass;
        mass.reference = FaceReference(faces, referenceCount);
        if (referenceCount == 0)
            return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
        for (const auto &face : faces) {
            if (face.vertices.size() < 3)
                continue;
            if (face.neighbor != 0) {
                if (!budget.ChargeBytes(sizeof(DestructionChunkId)))
                    return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                chunk.neighbors.push_back(siteIds[face.neighbor - 1U]);
            }
            const std::size_t root = FaceRoot(face);
            if (root == face.vertices.size())
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            for (std::size_t offset = 1; offset + 1 < face.vertices.size(); ++offset) {
                const std::array<Point, 3> triangle{face.vertices[root], face.vertices[(root + offset) % face.vertices.size()],
                                                    face.vertices[(root + offset + 1U) % face.vertices.size()]};
                if (auto appended = AppendChunkTriangle(triangle, face, mass, chunk, piece, collisionVertices, budget); appended.HasError())
                    return Result<OfflineVoronoiChunk>::Failure(appended.ErrorValue());
            }
        }
        if (!(mass.sixVolume > 0.0) || !std::isfinite(mass.sixVolume))
            return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
        chunk.volume = mass.sixVolume / 6.0;
        chunk.centerOfMass = Scale(mass.weightedCenter, 1.0 / mass.sixVolume);
        piece.volume = chunk.volume;
        if (auto closed = CloseCollisionPiece(piece, budget); closed.HasError())
            return Result<OfflineVoronoiChunk>::Failure(closed.ErrorValue());
        chunk.collisionPieces.push_back(std::move(piece));
        std::ranges::sort(chunk.neighbors);
        chunk.neighbors.erase(std::ranges::unique(chunk.neighbors).begin(), chunk.neighbors.end());
        return Result<OfflineVoronoiChunk>::Success(std::move(chunk));
    }

    void MergeChunk(OfflineVoronoiChunk &target, OfflineVoronoiChunk piece) {
        const double combined = target.volume + piece.volume;
        target.centerOfMass =
            Scale(Add(Scale(target.centerOfMass, target.volume), Scale(piece.centerOfMass, piece.volume)), 1.0 / combined);
        target.volume = combined;
        const auto offset = static_cast<std::uint32_t>(target.positions.size());
        target.positions.insert(target.positions.end(), piece.positions.begin(), piece.positions.end());
        for (auto triangle : piece.triangles) {
            for (auto &index : triangle.indices)
                index += offset;
            target.triangles.push_back(triangle);
        }
        target.collisionPieces.push_back(std::move(piece.collisionPieces.front()));
        target.neighbors.insert(target.neighbors.end(), piece.neighbors.begin(), piece.neighbors.end());
    }

    [[nodiscard]] Result<std::vector<Point>> ClipExteriorToSite(std::vector<Point> polygon, const std::vector<Point> &sites,
                                                                const std::uint32_t index, Budget &budget) {
        for (std::uint32_t other = 0; other < sites.size(); ++other) {
            if (polygon.size() < 3)
                break;
            if (other == index)
                continue;
            const Point normal = Sub(sites[other], sites[index]);
            const double planeOffset = Dot(normal, Scale(Add(sites[other], sites[index]), 0.5));
            const double tolerance = std::max(1.0e-10, Length(normal) * 1.0e-9);
            std::vector<Point> next;
            next.reserve(polygon.size() + 1);
            for (std::size_t vertex = 0; vertex < polygon.size(); ++vertex) {
                if (!budget.ChargeWork())
                    return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                const Point &start = polygon[vertex];
                const Point &end = polygon[(vertex + 1U) % polygon.size()];
                const double da = Dot(normal, start) - planeOffset;
                const double db = Dot(normal, end) - planeOffset;
                const bool insideA = da <= tolerance;
                const bool insideB = db <= tolerance;
                if (insideA)
                    next.push_back(start);
                if (insideA != insideB)
                    next.push_back(Add(start, Scale(Sub(end, start), std::clamp(da / (da - db), 0.0, 1.0))));
            }
            RemoveAdjacentDuplicates(next, tolerance);
            polygon = std::move(next);
        }
        return Result<std::vector<Point>>::Success(std::move(polygon));
    }

    [[nodiscard]] std::array<float, 3> RenderPosition(const Point &point) {
        return {static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])};
    }

    [[nodiscard]] Result<void> AppendExterior(OfflineVoronoiChunk &chunk, const std::vector<Face> &sourceFaces,
                                              const std::vector<Point> &sites, const std::uint32_t index, Budget &budget,
                                              const CancellationToken &cancellation) {
        for (const auto &sourceFace : sourceFaces) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            auto clipped = ClipExteriorToSite(sourceFace.vertices, sites, index, budget);
            if (clipped.HasError())
                return Result<void>::Failure(clipped.ErrorValue());
            const auto &polygon = clipped.Value();
            for (std::size_t offset = 1; offset + 1 < polygon.size(); ++offset) {
                const std::array<Point, 3> triangle{polygon[0], polygon[offset], polygon[offset + 1U]};
                if (const double area = Length(Cross(Sub(triangle[1], triangle[0]), Sub(triangle[2], triangle[0]))); area <= 1.0e-14)
                    continue;
                if (!budget.ChargeGeometry(3, 1))
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                const auto first = static_cast<std::uint32_t>(chunk.positions.size());
                for (const auto &point : triangle)
                    chunk.positions.push_back(RenderPosition(point));
                chunk.triangles.push_back({{first, first + 1U, first + 2U}, sourceFace.material, false});
            }
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Destruction::VoronoiDetail
