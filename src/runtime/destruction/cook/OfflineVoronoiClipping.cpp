#include "OfflineVoronoiInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Destruction::VoronoiDetail {
    void RemoveAdjacentDuplicates(std::vector<Point> &vertices, const double tolerance) {
        std::vector<Point> unique;
        unique.reserve(vertices.size());
        for (const auto &point : vertices) {
            if (unique.empty() || !Near(unique.back(), point, tolerance))
                unique.push_back(point);
        }
        if (unique.size() > 1 && Near(unique.front(), unique.back(), tolerance))
            unique.pop_back();
        vertices = std::move(unique);
    }

    [[nodiscard]] Result<Face> ClipFace(const Face &face, const Point &normal, const double planeOffset, const double tolerance,
                                        std::vector<Point> &cap, Budget &budget) {
        Face next{{}, face.material, face.neighbor, face.visible};
        next.vertices.reserve(face.vertices.size() + 1);
        for (std::size_t index = 0; index < face.vertices.size(); ++index) {
            if (!budget.ChargeWork())
                return Result<Face>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            const Point &a = face.vertices[index];
            const Point &b = face.vertices[(index + 1) % face.vertices.size()];
            const double da = Dot(normal, a) - planeOffset;
            const double db = Dot(normal, b) - planeOffset;
            const bool insideA = da <= tolerance;
            const bool insideB = db <= tolerance;
            if (insideA)
                next.vertices.push_back(a);
            if (insideA != insideB) {
                const double fraction = std::clamp(da / (da - db), 0.0, 1.0);
                const Point crossing = Add(a, Scale(Sub(b, a), fraction));
                next.vertices.push_back(crossing);
                cap.push_back(crossing);
            }
        }
        RemoveAdjacentDuplicates(next.vertices, tolerance);
        return Result<Face>::Success(std::move(next));
    }

    void AppendClipCap(std::vector<Face> &clipped, std::vector<Point> cap, const Point &normal, const double normalLength,
                       const double tolerance, const std::uint32_t interiorMaterial, const std::uint32_t otherIndex) {
        std::sort(cap.begin(), cap.end());
        cap.erase(std::unique(cap.begin(), cap.end(),
                              [&](const Point &a, const Point &b) {
            return Near(a, b, tolerance);
        }),
                  cap.end());
        if (cap.size() < 3)
            return;
        Point center{};
        for (const Point &point : cap)
            center = Add(center, point);
        center = Scale(center, 1.0 / static_cast<double>(cap.size()));
        const Point axis = std::abs(normal[0]) < std::abs(normal[1]) ? Point{1, 0, 0} : Point{0, 1, 0};
        const Point u = Scale(Cross(axis, normal), 1.0 / Length(Cross(axis, normal)));
        const Point v = Scale(Cross(normal, u), 1.0 / normalLength);
        std::sort(cap.begin(), cap.end(), [&](const Point &a, const Point &b) {
            const Point aa = Sub(a, center);
            const Point bb = Sub(b, center);
            const double angleA = std::atan2(Dot(aa, v), Dot(aa, u));
            const double angleB = std::atan2(Dot(bb, v), Dot(bb, u));
            return angleA == angleB ? a < b : angleA < angleB;
        });
        clipped.push_back(Face{std::move(cap), interiorMaterial, otherIndex, otherIndex != 0});
    }

    [[nodiscard]] Result<void> ValidateClippedVolume(std::vector<Face> &clipped, Budget &budget) {
        if (clipped.size() > budget.triangleLimit)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        std::uint64_t scratchVertices{};
        for (const auto &face : clipped) {
            scratchVertices += face.vertices.size();
            if (scratchVertices > budget.vertexLimit)
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        }
        if (clipped.empty())
            return Result<void>::Success();
        const Point reference = clipped.front().vertices.front();
        double volume6{};
        for (const auto &face : clipped) {
            for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index) {
                if (!budget.ChargeWork())
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                volume6 += Dot(Sub(face.vertices[0], reference),
                               Cross(Sub(face.vertices[index], reference), Sub(face.vertices[index + 1U], reference)));
            }
        }
        if (std::abs(volume6) <= 1.0e-12)
            clipped.clear();
        return Result<void>::Success();
    }

    [[nodiscard]] Result<void> Clip(std::vector<Face> &faces, const Point &site, const Point &other, const std::uint32_t otherIndex,
                                    const std::uint32_t interiorMaterial, Budget &budget, const CancellationToken &cancellation) {
        const Point normal = Sub(other, site);
        const Point midpoint = Scale(Add(site, other), 0.5);
        const double normalLength = Length(normal);
        const double tolerance = std::max(1.0e-10, normalLength * 1.0e-9);
        const double planeOffset = Dot(normal, midpoint);
        std::vector<Face> clipped;
        clipped.reserve(faces.size() + 1);
        std::vector<Point> cap;
        for (const auto &face : faces) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            auto next = ClipFace(face, normal, planeOffset, tolerance, cap, budget);
            if (next.HasError())
                return Result<void>::Failure(next.ErrorValue());
            Face clippedFace = std::move(next.Value());
            if (clippedFace.vertices.size() < 3)
                continue;
            const bool bisectorFace =
                otherIndex != 0 && std::all_of(clippedFace.vertices.begin(), clippedFace.vertices.end(), [&](const Point &point) {
                return std::abs(Dot(normal, point) - planeOffset) <= tolerance;
            });
            if (bisectorFace) {
                clippedFace.material = interiorMaterial;
                clippedFace.neighbor = otherIndex;
                clippedFace.visible = true;
            }
            clipped.push_back(std::move(clippedFace));
        }
        AppendClipCap(clipped, std::move(cap), normal, normalLength, tolerance, interiorMaterial, otherIndex);
        if (auto checked = ValidateClippedVolume(clipped, budget); checked.HasError())
            return checked;
        faces = std::move(clipped);
        return Result<void>::Success();
    }

    struct Plane final {
        Point normal{};
        double offset{};
    };

    [[nodiscard]] std::vector<Face> BoundingBox(const OfflineVoronoiSource &source) {
        Point lower = Position(source.positions.front());
        Point upper = lower;
        for (const auto &value : source.positions) {
            const Point point = Position(value);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                lower[axis] = std::min(lower[axis], point[axis]);
                upper[axis] = std::max(upper[axis], point[axis]);
            }
        }
        const Point p000{lower[0], lower[1], lower[2]};
        const Point p100{upper[0], lower[1], lower[2]};
        const Point p110{upper[0], upper[1], lower[2]};
        const Point p010{lower[0], upper[1], lower[2]};
        const Point p001{lower[0], lower[1], upper[2]};
        const Point p101{upper[0], lower[1], upper[2]};
        const Point p111{upper[0], upper[1], upper[2]};
        const Point p011{lower[0], upper[1], upper[2]};
        return {{{p000, p010, p110, p100}, 0, 0, false}, {{p001, p101, p111, p011}, 0, 0, false}, {{p000, p100, p101, p001}, 0, 0, false},
                {{p010, p011, p111, p110}, 0, 0, false}, {{p000, p001, p011, p010}, 0, 0, false}, {{p100, p110, p111, p101}, 0, 0, false}};
    }

    [[nodiscard]] std::vector<Plane> SourcePlanes(const std::vector<Face> &faces) {
        std::vector<Plane> planes;
        planes.reserve(faces.size());
        for (const auto &face : faces) {
            Point normal = Cross(Sub(face.vertices[1], face.vertices[0]), Sub(face.vertices[2], face.vertices[0]));
            normal = Scale(normal, 1.0 / Length(normal));
            const std::size_t primary = std::abs(normal[0]) > 1.0e-12 ? 0 : (std::abs(normal[1]) > 1.0e-12 ? 1 : 2);
            if (normal[primary] < 0.0)
                normal = Scale(normal, -1.0);
            planes.push_back({normal, Dot(normal, face.vertices[0])});
        }
        std::sort(planes.begin(), planes.end(), [](const Plane &a, const Plane &b) {
            return a.normal == b.normal ? a.offset < b.offset : a.normal < b.normal;
        });
        planes.erase(std::unique(planes.begin(), planes.end(),
                                 [](const Plane &a, const Plane &b) {
            return Near(a.normal, b.normal, 1.0e-9) && std::abs(a.offset - b.offset) <= 1.0e-8;
        }),
                     planes.end());
        return planes;
    }

    [[nodiscard]] Result<void> ClipPlane(std::vector<Face> &faces, const Plane &plane, const bool positive, Budget &budget,
                                         const CancellationToken &cancellation) {
        const Point normal = positive ? Scale(plane.normal, -1.0) : plane.normal;
        const Point center = Scale(plane.normal, plane.offset);
        return Clip(faces, Sub(center, Scale(normal, 0.5)), Add(center, Scale(normal, 0.5)), 0, 0, budget, cancellation);
    }

    [[nodiscard]] Result<void> SplitRegions(std::vector<std::vector<Face>> &regions, const std::vector<Plane> &planes,
                                            const std::uint32_t maximumRegions, Budget &budget, const CancellationToken &cancellation) {
        for (const auto &plane : planes) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            std::vector<std::vector<Face>> next;
            for (const auto &region : regions) {
                bool negative = false;
                bool positive = false;
                for (const auto &face : region) {
                    for (const auto &point : face.vertices) {
                        if (!budget.ChargeWork())
                            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                        const double side = Dot(plane.normal, point) - plane.offset;
                        negative |= side < -1.0e-8;
                        positive |= side > 1.0e-8;
                    }
                }
                if (!negative || !positive) {
                    next.push_back(region);
                } else {
                    auto left = region;
                    auto right = region;
                    if (auto clipped = ClipPlane(left, plane, false, budget, cancellation); clipped.HasError())
                        return clipped;
                    if (auto clipped = ClipPlane(right, plane, true, budget, cancellation); clipped.HasError())
                        return clipped;
                    next.push_back(std::move(left));
                    next.push_back(std::move(right));
                }
                if (next.size() > maximumRegions)
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            }
            regions = std::move(next);
        }
        return Result<void>::Success();
    }

    [[nodiscard]] Result<std::vector<std::vector<Face>>> ConvexRegions(const OfflineVoronoiSource &source, const std::vector<Face> &surface,
                                                                       const OfflineVoronoiRecipe &recipe, Budget &budget,
                                                                       const CancellationToken &cancellation) {
        std::vector<std::vector<Face>> regions{BoundingBox(source)};
        if (auto split = SplitRegions(regions, SourcePlanes(surface), recipe.maximumConvexRegions, budget, cancellation); split.HasError())
            return Result<std::vector<std::vector<Face>>>::Failure(split.ErrorValue());
        std::vector<std::vector<Face>> inside;
        for (auto &region : regions) {
            Point center{};
            std::size_t count{};
            for (const auto &face : region) {
                for (const auto &point : face.vertices) {
                    center = Add(center, point);
                    ++count;
                }
            }
            if (count == 0)
                continue;
            center = Scale(center, 1.0 / static_cast<double>(count));
            if (!budget.ChargeWork(surface.size()))
                return Result<std::vector<std::vector<Face>>>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (!InsideSource(center, surface, 1.0e-8))
                continue;
            inside.push_back(std::move(region));
        }
        if (inside.empty())
            return Result<std::vector<std::vector<Face>>>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
        return Result<std::vector<std::vector<Face>>>::Success(std::move(inside));
    }

}  // namespace Horo::Destruction::VoronoiDetail
