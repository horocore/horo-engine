#pragma once

#include "Horo/Destruction/OfflineVoronoi.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace Horo::Destruction::VoronoiDetail {
    using Point = std::array<double, 3>;

    struct Face final {
        std::vector<Point> vertices;
        std::uint32_t material{};
        std::uint32_t neighbor{};  // One-based site index; zero denotes source exterior.
        bool visible{true};        // Convex decomposition seams are collision-only.
    };

    [[nodiscard]] inline Point Add(const Point &a, const Point &b) {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }

    [[nodiscard]] inline Point Sub(const Point &a, const Point &b) {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }

    [[nodiscard]] inline Point Scale(const Point &a, const double s) {
        return {a[0] * s, a[1] * s, a[2] * s};
    }

    [[nodiscard]] inline double Dot(const Point &a, const Point &b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    [[nodiscard]] inline Point Cross(const Point &a, const Point &b) {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }

    [[nodiscard]] inline double Length(const Point &a) {
        return std::sqrt(Dot(a, a));
    }

    [[nodiscard]] inline Point Position(const std::array<float, 3> &value) {
        return {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2])};
    }

    [[nodiscard]] inline bool NonzeroDigest(const Sha256Digest &digest) {
        return std::ranges::any_of(digest.bytes, [](const std::uint8_t value) {
            return value != 0;
        });
    }

    [[nodiscard]] inline bool Finite(const Point &point) {
        return std::ranges::all_of(point, [](const double value) {
            return std::isfinite(value);
        });
    }

    [[nodiscard]] inline bool Near(const Point &a, const Point &b, const double tolerance) {
        return Length(Sub(a, b)) <= tolerance;
    }

    struct Budget final {
        std::uint64_t work{};
        std::uint64_t bytes{};
        std::uint64_t vertices{};
        std::uint64_t triangles{};
        std::uint64_t workLimit{};
        std::uint64_t byteLimit{};
        std::uint64_t vertexLimit{};
        std::uint64_t triangleLimit{};

        [[nodiscard]] inline bool ChargeWork(const std::uint64_t amount = 1) {
            if (amount > workLimit - work)
                return false;
            work += amount;
            return true;
        }

        [[nodiscard]] inline bool ChargeGeometry(const std::uint64_t newVertices, const std::uint64_t newTriangles) {
            const std::uint64_t cost = newVertices * sizeof(std::array<float, 3>) + newTriangles * sizeof(OfflineVoronoiTriangle);
            if (newVertices > vertexLimit - vertices || newTriangles > triangleLimit - triangles || cost > byteLimit - bytes)
                return false;
            vertices += newVertices;
            triangles += newTriangles;
            bytes += cost;
            return true;
        }

        [[nodiscard]] inline bool ChargeBytes(const std::uint64_t amount) {
            if (amount > byteLimit - bytes)
                return false;
            bytes += amount;
            return true;
        }
    };

    [[nodiscard]] bool ValidLimits(const OfflineVoronoiRecipe &recipe);
    [[nodiscard]] Result<std::vector<Face>> ValidateSource(const OfflineVoronoiSource &source, Budget &budget,
                                                           const CancellationToken &cancellation);
    [[nodiscard]] bool ConvexSource(const OfflineVoronoiSource &source, const std::vector<Face> &faces, double tolerance, Budget &budget);
    [[nodiscard]] bool InsideSource(const Point &point, const std::vector<Face> &faces, double tolerance);
    [[nodiscard]] Result<std::vector<Point>> Sites(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                   const std::vector<Face> &faces, Budget &budget, const CancellationToken &cancellation);
    void RemoveAdjacentDuplicates(std::vector<Point> &vertices, double tolerance);
    [[nodiscard]] Result<void> Clip(std::vector<Face> &faces, const Point &site, const Point &other, std::uint32_t otherIndex,
                                    std::uint32_t interiorMaterial, Budget &budget, const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<std::vector<Face>>> ConvexRegions(const OfflineVoronoiSource &source, const std::vector<Face> &surface,
                                                                       const OfflineVoronoiRecipe &recipe, Budget &budget,
                                                                       const CancellationToken &cancellation);
    [[nodiscard]] Result<OfflineVoronoiChunk> MakeChunk(const std::vector<Face> &faces, const Point &site,
                                                        const std::vector<DestructionChunkId> &siteIds, std::uint32_t index,
                                                        Budget &budget);
    void MergeChunk(OfflineVoronoiChunk &target, OfflineVoronoiChunk piece);
    [[nodiscard]] Result<void> AppendExterior(OfflineVoronoiChunk &chunk, const std::vector<Face> &sourceFaces,
                                              const std::vector<Point> &sites, std::uint32_t index, Budget &budget,
                                              const CancellationToken &cancellation);
}  // namespace Horo::Destruction::VoronoiDetail
