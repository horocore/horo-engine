#include "Horo/Destruction/OfflineVoronoi.h"

#include "OfflineVoronoiProvenance.h"
#include "PreFracturedIntersection.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numbers>
#include <utility>

namespace Horo::Destruction::OfflineVoronoiErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.destruction"};
        constexpr auto kSeverity = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor InvalidInput{kDomain, ErrorCode{"destruction.voronoi.invalid_input"}, kSeverity,
                                           "Offline Voronoi input or provenance is invalid.",
                                           "Capture a valid immutable source and recipe."};
    const ErrorCodeDescriptor InvalidMesh{kDomain, ErrorCode{"destruction.voronoi.invalid_mesh"}, kSeverity,
                                          "Offline Voronoi source must be a closed, consistently wound mesh.",
                                          "Repair the source topology before generation."};
    const ErrorCodeDescriptor InvalidSites{kDomain, ErrorCode{"destruction.voronoi.invalid_sites"}, kSeverity,
                                           "Voronoi sites are duplicate, outside the source, or cannot be placed.",
                                           "Choose finite distinct sites inside the source or change the seed."};
    const ErrorCodeDescriptor LimitExceeded{kDomain, ErrorCode{"destruction.voronoi.limit_exceeded"}, kSeverity,
                                            "Offline Voronoi work or output exceeds explicit limits.",
                                            "Reduce source complexity or site count, or select a supported larger profile."};
    const ErrorCodeDescriptor Cancelled{kDomain, ErrorCode{"destruction.voronoi.cancelled"}, kSeverity,
                                        "Offline Voronoi generation was cancelled.", "Retry from the current source and recipe."};
    const ErrorCodeDescriptor Stale{kDomain, ErrorCode{"destruction.voronoi.stale"}, kSeverity,
                                    "Voronoi candidate refers to replaced source or recipe content.",
                                    "Regenerate from the current revision."};
    const ErrorCodeDescriptor Shutdown{kDomain, ErrorCode{"destruction.voronoi.shutdown"}, kSeverity,
                                       "Voronoi owner has closed candidate acceptance.", "Do not publish after shutdown."};
}  // namespace Horo::Destruction::OfflineVoronoiErrors

namespace Horo::Destruction {
    namespace {
        using Point = std::array<double, 3>;

        struct Face final {
            std::vector<Point> vertices;
            std::uint32_t material{};
            std::uint32_t neighbor{};  // One-based site index; zero denotes source exterior.
            bool visible{true};        // Convex decomposition seams are collision-only.
        };

        [[nodiscard]] Point Add(const Point &a, const Point &b) {
            return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
        }

        [[nodiscard]] Point Sub(const Point &a, const Point &b) {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        [[nodiscard]] Point Scale(const Point &a, const double s) {
            return {a[0] * s, a[1] * s, a[2] * s};
        }

        [[nodiscard]] double Dot(const Point &a, const Point &b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        [[nodiscard]] Point Cross(const Point &a, const Point &b) {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        [[nodiscard]] double Length(const Point &a) {
            return std::sqrt(Dot(a, a));
        }

        [[nodiscard]] Point Position(const std::array<float, 3> &value) {
            return {static_cast<double>(value[0]), static_cast<double>(value[1]), static_cast<double>(value[2])};
        }

        [[nodiscard]] bool NonzeroDigest(const Sha256Digest &digest) {
            return std::any_of(digest.bytes.begin(), digest.bytes.end(), [](const std::uint8_t value) {
                return value != 0;
            });
        }

        [[nodiscard]] bool Finite(const Point &point) {
            return std::all_of(point.begin(), point.end(), [](const double value) {
                return std::isfinite(value);
            });
        }

        [[nodiscard]] bool Near(const Point &a, const Point &b, const double tolerance) {
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

            [[nodiscard]] bool ChargeWork(const std::uint64_t amount = 1) {
                if (amount > workLimit - work)
                    return false;
                work += amount;
                return true;
            }

            [[nodiscard]] bool ChargeGeometry(const std::uint64_t newVertices, const std::uint64_t newTriangles) {
                const std::uint64_t cost = newVertices * sizeof(std::array<float, 3>) + newTriangles * sizeof(OfflineVoronoiTriangle);
                if (newVertices > vertexLimit - vertices || newTriangles > triangleLimit - triangles || cost > byteLimit - bytes)
                    return false;
                vertices += newVertices;
                triangles += newTriangles;
                bytes += cost;
                return true;
            }

            [[nodiscard]] bool ChargeBytes(const std::uint64_t amount) {
                if (amount > byteLimit - bytes)
                    return false;
                bytes += amount;
                return true;
            }
        };

        [[nodiscard]] bool ValidLimits(const OfflineVoronoiRecipe &recipe) {
            const auto tier = GetDestructionTierProfile(recipe.tier);
            if (tier.HasError())
                return false;
            const auto &limit = recipe.limits;
            const auto &ceiling = tier.Value().limits;
            return recipe.siteCount > 0 && recipe.siteCount <= limit.maximumChunksPerDestructible &&
                   limit.maximumChunksPerDestructible > 0 && limit.maximumChunksPerDestructible <= ceiling.maximumChunksPerDestructible &&
                   limit.maximumArtifactBytes > 0 && limit.maximumArtifactBytes <= ceiling.maximumArtifactBytes &&
                   limit.maximumTransitionBytes >= limit.maximumArtifactBytes &&
                   limit.maximumTransitionBytes <= ceiling.maximumTransitionBytes &&
                   limit.maximumResidentBytes >= limit.maximumTransitionBytes &&
                   limit.maximumResidentBytes <= ceiling.maximumResidentBytes && limit.maximumWorkItemsPerTransition > 0 &&
                   limit.maximumWorkItemsPerTransition <= ceiling.maximumWorkItemsPerTransition && recipe.maximumVertices > 0 &&
                   recipe.maximumTriangles > 0 && recipe.maximumWorkItems > 0 &&
                   recipe.maximumWorkItems <= limit.maximumWorkItemsPerTransition &&
                   recipe.maximumVertices <= limit.maximumArtifactBytes / sizeof(std::array<float, 3>) &&
                   recipe.maximumTriangles <= limit.maximumArtifactBytes / sizeof(OfflineVoronoiTriangle) &&
                   recipe.maximumConvexRegions > 0 && recipe.maximumConvexRegions <= DestructionHardLimits::ChunksPerDestructible;
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
                if (a >= source.positions.size() || b >= source.positions.size() || c >= source.positions.size() || a == b || b == c ||
                    c == a)
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
                                                       const std::vector<Face> &faces, Budget &budget,
                                                       const CancellationToken &cancellation) {
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
            return {{{p000, p010, p110, p100}, 0, 0, false}, {{p001, p101, p111, p011}, 0, 0, false},
                    {{p000, p100, p101, p001}, 0, 0, false}, {{p010, p011, p111, p110}, 0, 0, false},
                    {{p000, p001, p011, p010}, 0, 0, false}, {{p100, p110, p111, p101}, 0, 0, false}};
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

        [[nodiscard]] Result<std::vector<std::vector<Face>>> ConvexRegions(const OfflineVoronoiSource &source,
                                                                           const std::vector<Face> &surface,
                                                                           const OfflineVoronoiRecipe &recipe, Budget &budget,
                                                                           const CancellationToken &cancellation) {
            std::vector<std::vector<Face>> regions{BoundingBox(source)};
            if (auto split = SplitRegions(regions, SourcePlanes(surface), recipe.maximumConvexRegions, budget, cancellation);
                split.HasError())
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

        [[nodiscard]] Result<void> ValidateCollisionEdges(const OfflineVoronoiCollisionPiece &piece, Budget &budget) {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<unsigned, int>> edges;
            for (const auto &triangle : piece.triangles) {
                for (const auto [from, to] :
                     {std::pair{triangle[0], triangle[1]}, std::pair{triangle[1], triangle[2]}, std::pair{triangle[2], triangle[0]}}) {
                    if (!budget.ChargeWork())
                        return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                    auto &record = edges[std::minmax(from, to)];
                    ++record.first;
                    record.second += from < to ? 1 : -1;
                }
            }
            for (const auto &[edge, incidence] : edges) {
                (void)edge;
                if (incidence.first != 2 || incidence.second != 0)
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CloseCollisionPiece(OfflineVoronoiCollisionPiece &piece, Budget &budget) {
            std::size_t triangleIndex{};
            while (triangleIndex < piece.triangles.size()) {
                bool split{};
                for (std::size_t edge = 0; edge < 3 && !split; ++edge) {
                    const auto triangle = piece.triangles[triangleIndex];
                    const auto a = triangle[edge];
                    const auto b = triangle[(edge + 1U) % 3U];
                    const auto c = triangle[(edge + 2U) % 3U];
                    const Point start = Position(piece.positions[a]);
                    const Point end = Position(piece.positions[b]);
                    const Point direction = Sub(end, start);
                    const double squared = Dot(direction, direction);
                    if (!(squared > 0.0))
                        return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
                    std::uint32_t splitVertex = std::numeric_limits<std::uint32_t>::max();
                    double firstFraction = 1.0;
                    for (std::uint32_t candidate = 0; candidate < piece.positions.size(); ++candidate) {
                        if (!budget.ChargeWork())
                            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                        if (candidate == a || candidate == b)
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
                    if (splitVertex != std::numeric_limits<std::uint32_t>::max()) {
                        if (!budget.ChargeGeometry(0, 1))
                            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                        piece.triangles[triangleIndex] = {a, splitVertex, c};
                        piece.triangles.push_back({splitVertex, b, c});
                        split = true;
                    }
                }
                if (!split)
                    ++triangleIndex;
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

        [[nodiscard]] Result<void> AppendChunkTriangle(const std::array<Point, 3> &triangle, const Face &face, const Point &reference,
                                                       double &sixVolume, Point &weightedCenter, OfflineVoronoiChunk &chunk,
                                                       OfflineVoronoiCollisionPiece &piece,
                                                       std::map<std::array<float, 3>, std::uint32_t> &collisionVertices, Budget &budget) {
            const double volume6 = Dot(Sub(triangle[0], reference), Cross(Sub(triangle[1], reference), Sub(triangle[2], reference)));
            if (!std::isfinite(volume6) || volume6 < -1.0e-12)
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
            if (volume6 <= 1.0e-12)
                return Result<void>::Success();
            if (!budget.ChargeWork())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            sixVolume += volume6;
            const Point tetraCenter = Scale(Add(Add(Add(reference, triangle[0]), triangle[1]), triangle[2]), 0.25);
            weightedCenter = Add(weightedCenter, Scale(tetraCenter, volume6));
            const std::uint32_t renderFirst = static_cast<std::uint32_t>(chunk.positions.size());
            std::array<std::uint32_t, 3> collisionIndices{};
            std::size_t vertexIndex{};
            for (const Point &point : triangle) {
                std::array<float, 3> value{};
                for (std::size_t axis = 0; axis < 3; ++axis)
                    value[axis] = static_cast<float>(point[axis]);
                if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2]))
                    return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
                const auto existing = collisionVertices.find(value);
                if (existing == collisionVertices.end()) {
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
            const Point reference = FaceReference(faces, referenceCount);
            if (referenceCount == 0)
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
            double sixVolume{};
            Point weightedCenter{};
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
                    if (auto appended = AppendChunkTriangle(triangle, face, reference, sixVolume, weightedCenter, chunk, piece,
                                                            collisionVertices, budget);
                        appended.HasError())
                        return Result<OfflineVoronoiChunk>::Failure(appended.ErrorValue());
                }
            }
            if (!(sixVolume > 0.0) || !std::isfinite(sixVolume))
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
            chunk.volume = sixVolume / 6.0;
            chunk.centerOfMass = Scale(weightedCenter, 1.0 / sixVolume);
            piece.volume = chunk.volume;
            if (auto closed = CloseCollisionPiece(piece, budget); closed.HasError())
                return Result<OfflineVoronoiChunk>::Failure(closed.ErrorValue());
            chunk.collisionPieces.push_back(std::move(piece));
            std::sort(chunk.neighbors.begin(), chunk.neighbors.end());
            chunk.neighbors.erase(std::unique(chunk.neighbors.begin(), chunk.neighbors.end()), chunk.neighbors.end());
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
            for (std::uint32_t other = 0; other < sites.size() && polygon.size() >= 3; ++other) {
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
                    const Point &a = polygon[vertex];
                    const Point &b = polygon[(vertex + 1U) % polygon.size()];
                    const double da = Dot(normal, a) - planeOffset;
                    const double db = Dot(normal, b) - planeOffset;
                    const bool insideA = da <= tolerance;
                    const bool insideB = db <= tolerance;
                    if (insideA)
                        next.push_back(a);
                    if (insideA != insideB)
                        next.push_back(Add(a, Scale(Sub(b, a), std::clamp(da / (da - db), 0.0, 1.0))));
                }
                RemoveAdjacentDuplicates(next, tolerance);
                polygon = std::move(next);
            }
            return Result<std::vector<Point>>::Success(std::move(polygon));
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
                    const double area = Length(Cross(Sub(triangle[1], triangle[0]), Sub(triangle[2], triangle[0])));
                    if (area <= 1.0e-14)
                        continue;
                    if (!budget.ChargeGeometry(3, 1))
                        return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
                    const auto first = static_cast<std::uint32_t>(chunk.positions.size());
                    for (const auto &point : triangle) {
                        std::array<float, 3> value{};
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            value[axis] = static_cast<float>(point[axis]);
                        chunk.positions.push_back(value);
                    }
                    chunk.triangles.push_back({{first, first + 1U, first + 2U}, sourceFace.material, false});
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateGenerationInput(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                           const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            if (!source.asset.IsValid() || source.revision == 0 || recipe.id == 0 || recipe.revision == 0 || recipe.toolchainVersion == 0 ||
                !NonzeroDigest(recipe.toolchainDigest) || recipe.siteIds.size() != recipe.siteCount ||
                (!recipe.sites.empty() && recipe.sites.size() != recipe.siteCount))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            if (!ValidLimits(recipe))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            std::vector<DestructionChunkId> sortedIds = recipe.siteIds;
            std::sort(sortedIds.begin(), sortedIds.end());
            if (std::any_of(sortedIds.begin(), sortedIds.end(),
                            [](const DestructionChunkId id) {
                return !id.IsValid();
            }) ||
                std::adjacent_find(sortedIds.begin(), sortedIds.end()) != sortedIds.end())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            if (source.positions.size() > recipe.maximumVertices || source.indices.size() / 3 > recipe.maximumTriangles ||
                source.positions.size() > recipe.limits.maximumArtifactBytes / sizeof(std::array<float, 3>) ||
                source.indices.size() > recipe.limits.maximumArtifactBytes / sizeof(std::uint32_t) ||
                source.materialSlots.size() > recipe.limits.maximumArtifactBytes / sizeof(std::uint32_t))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (source.positions.size() > recipe.maximumWorkItems ||
                source.indices.size() > recipe.maximumWorkItems - source.positions.size() ||
                source.materialSlots.size() > recipe.maximumWorkItems - source.positions.size() - source.indices.size())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (source.digest != ComputeOfflineVoronoiSourceDigest(source))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<OfflineVoronoiChunk> GenerateSiteChunk(const std::uint32_t index, const std::vector<Point> &sites,
                                                                    const std::vector<std::vector<Face>> &regions,
                                                                    const std::vector<Face> &sourceFaces,
                                                                    const OfflineVoronoiRecipe &recipe, Budget &budget,
                                                                    const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            OfflineVoronoiChunk aggregate;
            aggregate.id = recipe.siteIds[index];
            aggregate.site = sites[index];
            for (const auto &region : regions) {
                auto faces = region;
                for (std::uint32_t other = 0; other < sites.size(); ++other) {
                    if (other == index)
                        continue;
                    auto clipped = Clip(faces, sites[index], sites[other], other + 1U, recipe.interiorMaterialSlot, budget, cancellation);
                    if (clipped.HasError())
                        return Result<OfflineVoronoiChunk>::Failure(clipped.ErrorValue());
                    if (faces.empty())
                        break;
                }
                if (faces.empty())
                    continue;
                auto chunk = MakeChunk(faces, sites[index], recipe.siteIds, index, budget);
                if (chunk.HasError())
                    return chunk;
                MergeChunk(aggregate, std::move(chunk.Value()));
            }
            if (!(aggregate.volume > 0.0))
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
            if (auto exterior = AppendExterior(aggregate, sourceFaces, sites, index, budget, cancellation); exterior.HasError())
                return Result<OfflineVoronoiChunk>::Failure(exterior.ErrorValue());
            std::sort(aggregate.neighbors.begin(), aggregate.neighbors.end());
            aggregate.neighbors.erase(std::unique(aggregate.neighbors.begin(), aggregate.neighbors.end()), aggregate.neighbors.end());
            return Result<OfflineVoronoiChunk>::Success(std::move(aggregate));
        }

        [[nodiscard]] Result<void> ValidateNeighbors(const std::vector<OfflineVoronoiChunk> &chunks) {
            for (const auto &chunk : chunks) {
                for (const auto neighbor : chunk.neighbors) {
                    const auto other = std::find_if(chunks.begin(), chunks.end(), [&](const auto &candidateChunk) {
                        return candidateChunk.id == neighbor;
                    });
                    if (other == chunks.end() || !std::binary_search(other->neighbors.begin(), other->neighbors.end(), chunk.id))
                        return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
                }
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc GenerateOfflineVoronoi */
    Result<OfflineVoronoiCandidate> GenerateOfflineVoronoi(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                           const CancellationToken &cancellation) {
        if (auto validated = ValidateGenerationInput(source, recipe, cancellation); validated.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(validated.ErrorValue());
        Budget budget{.workLimit = recipe.maximumWorkItems,
                      .byteLimit = recipe.limits.maximumArtifactBytes,
                      .vertexLimit = recipe.maximumVertices,
                      .triangleLimit = recipe.maximumTriangles};
        if (!budget.ChargeBytes(sizeof(OfflineVoronoiCandidate) +
                                static_cast<std::uint64_t>(recipe.siteCount) * sizeof(OfflineVoronoiChunk)))
            return Result<OfflineVoronoiCandidate>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        auto sourceFaces = ValidateSource(source, budget, cancellation);
        if (sourceFaces.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(sourceFaces.ErrorValue());
        auto sites = Sites(source, recipe, sourceFaces.Value(), budget, cancellation);
        if (sites.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(sites.ErrorValue());
        std::vector<std::vector<Face>> regions;
        if (ConvexSource(source, sourceFaces.Value(), 1.0e-8, budget)) {
            regions.push_back(sourceFaces.Value());
            for (auto &face : regions.front())
                face.visible = false;
        } else {
            auto decomposed = ConvexRegions(source, sourceFaces.Value(), recipe, budget, cancellation);
            if (decomposed.HasError())
                return Result<OfflineVoronoiCandidate>::Failure(decomposed.ErrorValue());
            regions = std::move(decomposed.Value());
        }
        OfflineVoronoiCandidate candidate;
        candidate.sourceAsset = source.asset;
        candidate.sourceRevision = source.revision;
        candidate.sourceDigest = source.digest;
        candidate.recipeId = recipe.id;
        candidate.recipeRevision = recipe.revision;
        candidate.semanticFingerprint = Detail::VoronoiFingerprint(source, recipe, sites.Value());
        candidate.toolchainDigest = recipe.toolchainDigest;
        candidate.chunks.reserve(sites.Value().size());
        for (std::uint32_t index = 0; index < sites.Value().size(); ++index) {
            auto chunk = GenerateSiteChunk(index, sites.Value(), regions, sourceFaces.Value(), recipe, budget, cancellation);
            if (chunk.HasError())
                return Result<OfflineVoronoiCandidate>::Failure(chunk.ErrorValue());
            candidate.chunks.push_back(std::move(chunk.Value()));
        }
        if (auto neighbors = ValidateNeighbors(candidate.chunks); neighbors.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(neighbors.ErrorValue());
        candidate.estimatedBytes = budget.bytes;
        candidate.workItems = budget.work;
        candidate.outputChecksum_ = Detail::VoronoiOutputChecksum(candidate);
        return Result<OfflineVoronoiCandidate>::Success(std::move(candidate));
    }

    /** @copydoc OfflineVoronoiOwner::Accept */
    Result<void> OfflineVoronoiOwner::Accept(OfflineVoronoiCandidate candidate, const std::uint64_t expectedOwnerRevision,
                                             const OfflineVoronoiSource &currentSource, const OfflineVoronoiRecipe &currentRecipe) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Shutdown));
        if (candidate.outputChecksum_ != Detail::VoronoiOutputChecksum(candidate))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
        if (!ValidLimits(currentRecipe) || currentRecipe.siteIds.size() != currentRecipe.siteCount ||
            (!currentRecipe.sites.empty() && currentRecipe.sites.size() != currentRecipe.siteCount))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        if (expectedOwnerRevision != revision_ || candidate.sourceAsset != currentSource.asset ||
            candidate.sourceRevision != currentSource.revision || candidate.sourceDigest != currentSource.digest ||
            candidate.recipeId != currentRecipe.id || candidate.recipeRevision != currentRecipe.revision ||
            candidate.toolchainDigest != currentRecipe.toolchainDigest || candidate.schemaVersion != OfflineVoronoiSchemaVersion ||
            candidate.sourceDigest != ComputeOfflineVoronoiSourceDigest(currentSource) ||
            candidate.chunks.size() != currentRecipe.siteCount)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        std::vector<Point> sites;
        sites.reserve(candidate.chunks.size());
        for (const auto &chunk : candidate.chunks)
            sites.push_back(chunk.site);
        if (candidate.semanticFingerprint != Detail::VoronoiFingerprint(currentSource, currentRecipe, sites) ||
            (!currentRecipe.sites.empty() && currentRecipe.sites != sites))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        auto next = std::make_shared<const OfflineVoronoiCandidate>(std::move(candidate));
        cancellation_.RequestCancellation();
        current_ = std::move(next);
        cancellation_ = std::move(nextCancellation);
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc OfflineVoronoiOwner::Invalidate */
    Result<void> OfflineVoronoiOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Shutdown));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        cancellation_.RequestCancellation();
        cancellation_ = std::move(nextCancellation);
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc OfflineVoronoiOwner::Shutdown */
    void OfflineVoronoiOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
