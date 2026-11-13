#include "AnimationBlendTreeGeometry.h"
#include "AnimationGraphCompilerInternal.h"

#include <cmath>
#include <set>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        using namespace BlendDetail;

        /** @brief Validates one member before any geometry arithmetic. */
        bool ValidSample(const GraphBlendSample &sample, const GraphBlendTreeNode &tree, const AnimationGraphData &data) noexcept {
            if (!sample.clip.IsValid() || sample.clip.Asset() == data.id.Asset() || sample.clip.Asset() == data.skeleton.Asset())
                return false;
            if (!std::isfinite(sample.x) || !std::isfinite(sample.y))
                return false;
            return tree.dimension != GraphBlendDimension::One || sample.y == 0.0F;
        }

        /** @brief Coordinates and identities are finite before any pair/triangle walk. */
        bool ValidSamples(const GraphBlendTreeNode &tree, const AnimationGraphData &data) {
            if (tree.samples.empty() || tree.samples.size() > AnimationGraphHardLimits::BlendSamplesPerTree || !UniqueIds(tree.samples))
                return false;
            for (const auto &sample : tree.samples)
                if (!ValidSample(sample, tree, data))
                    return false;
            return true;
        }

        /** @brief A coordinate is an exact Float declaration, not an implicitly converted integer/trigger. */
        bool FloatParameter(const AnimationGraphData &data, const GraphParameterId id) {
            const auto *parameter = Find(data.parameters, id);
            return parameter && parameter->type == GraphValueType::Float;
        }

        /** @brief Distinct positions prevent ambiguous equal-coordinate membership and empty domain edges. */
        bool UniquePositions(const GraphBlendTreeNode &tree) {
            for (std::size_t i = 0; i < tree.samples.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (Position(tree.samples[i]).x == Position(tree.samples[j]).x &&
                        Position(tree.samples[i]).y == Position(tree.samples[j]).y)
                        return false;
            return true;
        }

        /** @brief Resolves a previously identity-validated triangle. */
        std::array<Point, 3> Triangle(const GraphBlendTreeNode &tree, const GraphBlendTriangle &triangle) {
            return {Position(tree.samples[Index(tree, triangle.samples[0])]), Position(tree.samples[Index(tree, triangle.samples[1])]),
                    Position(tree.samples[Index(tree, triangle.samples[2])])};
        }

        /** @brief Rejects missing/repeated vertices, duplicate faces and degenerate triangles before geometry tests. */
        bool ValidTriangles(const GraphBlendTreeNode &tree) {
            if (tree.triangles.empty() || tree.triangles.size() > AnimationGraphHardLimits::BlendTrianglesPerTree)
                return false;
            std::set<std::array<GraphBlendSampleId, 3>> faces;
            for (const auto &triangle : tree.triangles) {
                auto ids = triangle.samples;
                std::ranges::sort(ids);
                if (ids[0] == ids[1] || ids[1] == ids[2] || !faces.insert(ids).second)
                    return false;
                for (const auto id : ids)
                    if (Index(tree, id) == tree.samples.size())
                        return false;
                const auto points = Triangle(tree, triangle);
                if (Cross(points[0], points[1], points[2]) == 0.0)
                    return false;
            }
            return true;
        }

        /** @brief Distinguishes lawful common faces/edges from crossings or overlapping triangle interiors. */
        bool Overlap(const std::array<Point, 3> &a, const std::array<Point, 3> &b) {
            for (std::size_t i = 0; i < 3; ++i) {
                if (StrictInside(a[i], b) || StrictInside(b[i], a))
                    return true;
                for (std::size_t j = 0; j < 3; ++j)
                    if (EdgesCross(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3]))
                        return true;
            }
            return false;
        }

        /** @brief A nonincident vertex strictly within an unsplit edge is a malformed T junction. */
        bool OnEdge(const GraphBlendSample &sample, const GraphBlendTriangle &triangle, const std::array<Point, 3> &points,
                    const std::size_t edge) noexcept {
            if (triangle.samples[edge] == sample.id || triangle.samples[(edge + 1) % 3] == sample.id)
                return false;
            const auto t = EdgeWeight(Position(sample), points[edge], points[(edge + 1) % 3]);
            return Cross(points[edge], points[(edge + 1) % 3], Position(sample)) == 0.0 && t > 0.0 && t < 1.0;
        }

        /** @brief Every sample is a mesh vertex and no unreferenced sample lies on an unsplit edge (T junction). */
        bool VertexTopology(const GraphBlendTreeNode &tree) {
            for (const auto &sample : tree.samples) {
                bool used = false;
                for (const auto &triangle : tree.triangles) {
                    const auto points = Triangle(tree, triangle);
                    for (std::size_t edge = 0; edge < 3; ++edge) {
                        if (triangle.samples[edge] == sample.id)
                            used = true;
                        if (OnEdge(sample, triangle, points, edge))
                            return false;
                    }
                }
                if (!used)
                    return false;
            }
            return true;
        }

        /** @brief Admits a bounded simplicial complex; disconnected domains remain explicit nearest-edge domains. */
        bool ValidMesh(const GraphBlendTreeNode &tree) {
            if (!ValidTriangles(tree))
                return false;
            for (std::size_t i = 0; i < tree.triangles.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (Overlap(Triangle(tree, tree.triangles[i]), Triangle(tree, tree.triangles[j])))
                        return false;
            return VertexTopology(tree);
        }

        /** @brief Explicit groups have one policy across all authored definitions, including inactive members. */
        bool ConsistentGroup(const GraphBlendTreeNode &tree, const AnimationGraphData &data) {
            if (!tree.group.IsValid())
                return true;
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (const auto *other = std::get_if<GraphBlendTreeNode>(&node.payload);
                        other && other->group == tree.group && other->sync != tree.sync)
                        return false;
            return true;
        }

        /** @brief Rejects unknown policies independently from coordinate/geometry admission. */
        bool ValidPolicy(const GraphBlendTreeNode &tree) noexcept {
            return tree.dimension < GraphBlendDimension::Count && tree.outside < GraphBlendOutside::Count &&
                   tree.sync.mode < GraphSyncMode::Count && tree.sync.events < GraphSyncEventPolicy::Count &&
                   tree.sync.leader < GraphSyncLeaderPolicy::Count;
        }

        /** @brief Matches dimensionality to its exact parameter and mesh requirements. */
        bool ValidDomain(const GraphBlendTreeNode &tree, const AnimationGraphData &data) {
            if (!FloatParameter(data, tree.x))
                return false;
            if (tree.dimension == GraphBlendDimension::One)
                return !tree.y.IsValid() && tree.triangles.empty();
            return FloatParameter(data, tree.y) && ValidMesh(tree);
        }
    }  // namespace

    /** @copydoc ValidateBlendTree */
    Result<void> ValidateBlendTree(const GraphBlendTreeNode &tree, const AnimationGraphData &data, const GraphSourceLocation location) {
        using namespace AnimationErrors;
        if (!ValidPolicy(tree))
            return Result<void>::Failure(Failure(GraphTypeMismatch, location));
        if (!ValidSamples(tree, data) || !UniquePositions(tree) || !ConsistentGroup(tree, data))
            return Result<void>::Failure(Failure(GraphMalformed, location));
        if (!ValidDomain(tree, data))
            return Result<void>::Failure(Failure(GraphBindingMismatch, location));
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::GraphCompileDetail
