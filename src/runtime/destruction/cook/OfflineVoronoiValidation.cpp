#include "OfflineVoronoiInternal.h"
#include "PreFracturedIntersection.h"

#include <algorithm>
#include <map>
#include <numbers>
#include <utility>

namespace Horo::Destruction::VoronoiDetail {
    [[nodiscard]] bool ValidLimits(const OfflineVoronoiRecipe &recipe) {
        const auto tier = GetDestructionTierProfile(recipe.tier);
        if (tier.HasError())
            return false;
        const auto &limit = recipe.limits;
        const auto &ceiling = tier.Value().limits;
        return recipe.siteCount > 0 && recipe.siteCount <= limit.maximumChunksPerDestructible && limit.maximumChunksPerDestructible > 0 &&
               limit.maximumChunksPerDestructible <= ceiling.maximumChunksPerDestructible && limit.maximumArtifactBytes > 0 &&
               limit.maximumArtifactBytes <= ceiling.maximumArtifactBytes && limit.maximumTransitionBytes >= limit.maximumArtifactBytes &&
               limit.maximumTransitionBytes <= ceiling.maximumTransitionBytes &&
               limit.maximumResidentBytes >= limit.maximumTransitionBytes && limit.maximumResidentBytes <= ceiling.maximumResidentBytes &&
               limit.maximumWorkItemsPerTransition > 0 && limit.maximumWorkItemsPerTransition <= ceiling.maximumWorkItemsPerTransition &&
               recipe.maximumVertices > 0 && recipe.maximumTriangles > 0 && recipe.maximumWorkItems > 0 &&
               recipe.maximumWorkItems <= limit.maximumWorkItemsPerTransition &&
               recipe.maximumVertices <= limit.maximumArtifactBytes / sizeof(std::array<float, 3>) &&
               recipe.maximumTriangles <= limit.maximumArtifactBytes / sizeof(OfflineVoronoiTriangle) && recipe.maximumConvexRegions > 0 &&
               recipe.maximumConvexRegions <= DestructionHardLimits::ChunksPerDestructible;
    }

    [[nodiscard]] Result<std::vector<Face>> ValidateSourceTriangles(const OfflineVoronoiSource &source, const double tolerance,
                                                                    Budget &budget, const CancellationToken &cancellation) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<unsigned, int>> edges;
        std::vector<Face> faces;
        faces.reserve(source.indices.size() / 3);
        for (std::size_t offset = 0; offset < source.indices.size(); offset += 3) {
            if (cancellation.IsCancellationRequested())
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            if (!budget.ChargeWork(3 + source.positions.size()))
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            const auto a = source.indices[offset];
            const auto b = source.indices[offset + 1];
            const auto c = source.indices[offset + 2];
            if (a >= source.positions.size() || b >= source.positions.size() || c >= source.positions.size() || a == b || b == c || c == a)
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            const Point pa = Position(source.positions[a]);
            const Point pb = Position(source.positions[b]);
            const Point pc = Position(source.positions[c]);
            const Point normal = Cross(Sub(pb, pa), Sub(pc, pa));
            if (Length(normal) <= tolerance * tolerance)
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            for (const auto [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                auto &edge = edges[std::minmax(from, to)];
                ++edge.first;
                edge.second += from < to ? 1 : -1;
            }
            faces.push_back(Face{{pa, pb, pc}, source.materialSlots[offset / 3], 0, true});
        }
        for (const auto &[key, incidence] : edges) {
            (void)key;
            if (incidence.first != 2 || incidence.second != 0)
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        }
        return Result<std::vector<Face>>::Success(std::move(faces));
    }

    [[nodiscard]] Result<std::vector<Face>> ValidateSource(const OfflineVoronoiSource &source, Budget &budget,
                                                           const CancellationToken &cancellation) {
        if (source.positions.size() < 4 || source.indices.size() < 12 || source.indices.size() % 3 != 0 ||
            source.materialSlots.size() != source.indices.size() / 3 || source.positions.size() > budget.vertexLimit ||
            source.indices.size() / 3 > budget.triangleLimit)
            return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        Point lower = Position(source.positions.front());
        Point upper = lower;
        for (const auto &position : source.positions) {
            const Point point = Position(position);
            if (!Finite(point))
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            for (std::size_t axis = 0; axis < 3; ++axis) {
                lower[axis] = std::min(lower[axis], point[axis]);
                upper[axis] = std::max(upper[axis], point[axis]);
            }
        }
        const double extent = Length(Sub(upper, lower));
        if (!(extent > 0.0) || !std::isfinite(extent) || extent > 1.0e7)
            return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        const double tolerance = std::max(1.0e-8, extent * 1.0e-8);
        auto faces = ValidateSourceTriangles(source, tolerance, budget, cancellation);
        if (faces.HasError())
            return faces;
        Assets::PreFracturedSourceNode node;
        node.positions = source.positions;
        node.triangleIndices = source.indices;
        std::uint64_t remainingWork = budget.workLimit - budget.work;
        switch (Detail::CheckSelfIntersection(node, remainingWork, cancellation)) {
            case Detail::IntersectionCheck::Intersecting:
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            case Detail::IntersectionCheck::TooMuchWork:
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            case Detail::IntersectionCheck::Cancelled:
                return Result<std::vector<Face>>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            case Detail::IntersectionCheck::Clear:
                break;
        }
        budget.work = budget.workLimit - remainingWork;
        return faces;
    }

    [[nodiscard]] bool ConvexSource(const OfflineVoronoiSource &source, const std::vector<Face> &faces, const double tolerance,
                                    Budget &budget) {
        for (const auto &face : faces) {
            const Point normal = Cross(Sub(face.vertices[1], face.vertices[0]), Sub(face.vertices[2], face.vertices[0]));
            for (const auto &value : source.positions) {
                if (!budget.ChargeWork())
                    return false;
                if (Dot(normal, Sub(Position(value), face.vertices[0])) > Length(normal) * tolerance)
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool PointOnTriangle(const Point &point, const Face &face, const double tolerance) {
        const Point a = face.vertices[0];
        const Point b = face.vertices[1];
        const Point c = face.vertices[2];
        const Point normal = Cross(Sub(b, a), Sub(c, a));
        if (std::abs(Dot(normal, Sub(point, a))) > Length(normal) * tolerance)
            return false;
        const double squared = Dot(normal, normal);
        const double u = Dot(Cross(Sub(b, point), Sub(c, point)), normal) / squared;
        const double v = Dot(Cross(Sub(c, point), Sub(a, point)), normal) / squared;
        const double w = 1.0 - u - v;
        return u >= -tolerance && v >= -tolerance && w >= -tolerance;
    }

    [[nodiscard]] bool InsideSource(const Point &point, const std::vector<Face> &faces, const double tolerance) {
        double solidAngle{};
        for (const auto &face : faces) {
            if (PointOnTriangle(point, face, tolerance))
                return false;
            const Point a = Sub(face.vertices[0], point);
            const Point b = Sub(face.vertices[1], point);
            const Point c = Sub(face.vertices[2], point);
            const double la = Length(a);
            const double lb = Length(b);
            const double lc = Length(c);
            if (std::min({la, lb, lc}) <= tolerance)
                return false;
            const double numerator = Dot(a, Cross(b, c));
            const double denominator = la * lb * lc + Dot(a, b) * lc + Dot(b, c) * la + Dot(c, a) * lb;
            solidAngle += 2.0 * std::atan2(numerator, denominator);
        }
        return std::abs(solidAngle) > 2.0 * std::numbers::pi;
    }

    [[nodiscard]] std::uint64_t NextRandom(std::uint64_t &state) {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] double UnitRandom(std::uint64_t &state) {
        return static_cast<double>(NextRandom(state) >> 11U) * (1.0 / 9007199254740992.0);
    }

    [[nodiscard]] Result<std::vector<Point>> Sites(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                   const std::vector<Face> &faces, Budget &budget, const CancellationToken &cancellation) {
        Point lower = Position(source.positions.front());
        Point upper = lower;
        for (const auto &value : source.positions) {
            const Point point = Position(value);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                lower[axis] = std::min(lower[axis], point[axis]);
                upper[axis] = std::max(upper[axis], point[axis]);
            }
        }
        const double tolerance = std::max(1.0e-8, Length(Sub(upper, lower)) * 1.0e-8);
        std::vector<Point> sites;
        sites.reserve(recipe.siteCount);
        std::uint64_t state = recipe.seed;
        const std::uint64_t attemptLimit = static_cast<std::uint64_t>(recipe.siteCount) * 64U;
        for (std::uint64_t attempt = 0; sites.size() < recipe.siteCount && attempt < attemptLimit; ++attempt) {
            if (cancellation.IsCancellationRequested())
                return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            if (!budget.ChargeWork(1 + faces.size()))
                return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            Point candidate{};
            if (recipe.sites.empty()) {
                for (std::size_t axis = 0; axis < 3; ++axis)
                    candidate[axis] = lower[axis] + (upper[axis] - lower[axis]) * UnitRandom(state);
            } else {
                candidate = recipe.sites[sites.size()];
            }
            const bool unique = std::none_of(sites.begin(), sites.end(), [&](const Point &site) {
                return Near(site, candidate, tolerance);
            });
            if (Finite(candidate) && unique && InsideSource(candidate, faces, tolerance)) {
                sites.push_back(candidate);
            } else if (!recipe.sites.empty()) {
                return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
            }
            if (budget.work == budget.workLimit && sites.size() < recipe.siteCount)
                return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        }
        if (sites.size() != recipe.siteCount)
            return Result<std::vector<Point>>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
        return Result<std::vector<Point>>::Success(std::move(sites));
    }

}  // namespace Horo::Destruction::VoronoiDetail
