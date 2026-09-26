#include "../../../AssetErrors.h"
#include "Horo/Assets/PreFracturedSource.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <ufbx.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Assets {
    namespace {
        constexpr std::size_t kMaximumSourceBytes = 512U * 1024U * 1024U;
        constexpr std::size_t kMaximumParserBytes = 256U * 1024U * 1024U;
        constexpr std::size_t kMaximumNodes = 1024;
        constexpr std::size_t kMaximumSceneNodes = 8192;
        constexpr std::size_t kMaximumVertices = 1'000'000;
        constexpr std::size_t kMaximumIndices = 6'000'000;
        constexpr std::size_t kMaximumFaces = 2'000'000;
        constexpr std::size_t kMaximumNormalizedBytes = 256U * 1024U * 1024U;

        struct SceneDeleter {
            void operator()(ufbx_scene *scene) const noexcept {
                ufbx_free_scene(scene);
            }
        };

        /** @brief Observes only the token borrowed for the synchronous ufbx load call. */
        ufbx_progress_result ParseProgress(void *user, const ufbx_progress *) noexcept {
            const auto *cancellation = static_cast<const CancellationToken *>(user);
            return cancellation->IsCancellationRequested() ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
        }

        [[nodiscard]] Error SourceError(const ErrorCodeDescriptor &code, std::string_view sourceName, std::string_view path,
                                        std::string message) {
            auto error = MakeError(code, message);
            error.diagnostics.push_back({DiagnosticCode{code.code.Value()}, DiagnosticSeverity::Error, std::move(message),
                                         SourceLocation{std::string{sourceName}}, std::string{path}});
            return error;
        }

        [[nodiscard]] std::string NodePath(const ufbx_node *node) {
            return "nodes/" + std::to_string(node->typed_id);
        }

        [[nodiscard]] bool FiniteMatrix(const ufbx_matrix &matrix) {
            for (const double value : matrix.v) {
                if (!std::isfinite(value))
                    return false;
            }
            const double determinant = matrix.m00 * (matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21) -
                                       matrix.m01 * (matrix.m10 * matrix.m22 - matrix.m12 * matrix.m20) +
                                       matrix.m02 * (matrix.m10 * matrix.m21 - matrix.m11 * matrix.m20);
            return std::isfinite(determinant) && std::abs(determinant) > 1.0e-18;
        }

        [[nodiscard]] Result<void> CopyPositions(const ufbx_node *node, PreFracturedSourceNode &output, const std::string_view sourceName,
                                                 const CancellationToken &cancellation) {
            const ufbx_mesh *mesh = node->mesh;
            output.positions.reserve(mesh->vertices.count);
            for (std::size_t index = 0; index < mesh->vertices.count; ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(SourceError(ImportErrors::ImportCancelled, sourceName, output.sourcePath,
                                                             "Pre-fractured source import was cancelled."));
                const ufbx_vec3 position = ufbx_transform_position(&node->geometry_to_world, mesh->vertices[index]);
                const std::array<float, 3> converted{static_cast<float>(position.x), static_cast<float>(position.y),
                                                     static_cast<float>(position.z)};
                if (!std::isfinite(converted[0]) || !std::isfinite(converted[1]) || !std::isfinite(converted[2]))
                    return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, output.sourcePath,
                                                             "Mesh node contains a non-finite vertex position."));
                output.positions.push_back(converted);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::string FaceMaterial(const ufbx_node *node, const std::size_t faceIndex) {
            const ufbx_mesh *mesh = node->mesh;
            if (faceIndex >= mesh->face_material.count)
                return {};
            const std::uint32_t index = mesh->face_material[faceIndex];
            if (index >= node->materials.count || node->materials[index] == nullptr)
                return {};
            const ufbx_string name = node->materials[index]->name;
            return name.data != nullptr ? std::string{name.data, name.length} : std::string{};
        }

        [[nodiscard]] Result<void> CopyTriangles(const ufbx_node *node, PreFracturedSourceNode &output, const std::string_view sourceName,
                                                 const CancellationToken &cancellation) {
            const ufbx_mesh *mesh = node->mesh;
            if (mesh->max_face_triangles > kMaximumIndices / 3U)
                return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath,
                                                         "Mesh polygon exceeds the triangle budget."));
            std::vector<std::uint32_t> corners(mesh->max_face_triangles * 3U);
            const auto &matrix = node->geometry_to_world;
            const double determinant = matrix.m00 * (matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21) -
                                       matrix.m01 * (matrix.m10 * matrix.m22 - matrix.m12 * matrix.m20) +
                                       matrix.m02 * (matrix.m10 * matrix.m21 - matrix.m11 * matrix.m20);
            const bool mirrored = determinant < 0.0;
            output.triangleIndices.reserve(mesh->num_triangles * 3U);
            output.triangleMaterials.reserve(mesh->num_triangles);
            for (std::size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(SourceError(ImportErrors::ImportCancelled, sourceName, output.sourcePath,
                                                             "Pre-fractured source import was cancelled."));
                const ufbx_face face = mesh->faces[faceIndex];
                if (face.num_indices < 3)
                    return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, output.sourcePath,
                                                             "Mesh node contains a degenerate polygon."));
                const std::uint32_t triangleCount = ufbx_triangulate_face(corners.data(), corners.size(), mesh, face);
                if (triangleCount == 0 || output.triangleIndices.size() > kMaximumIndices - triangleCount * 3U)
                    return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath,
                                                             "Mesh triangulation failed or exceeded the triangle budget."));
                const std::string material = FaceMaterial(node, faceIndex);
                for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        const std::uint32_t polygonCorner = corners[triangle * 3U + corner];
                        if (polygonCorner >= mesh->vertex_indices.count || mesh->vertex_indices[polygonCorner] >= mesh->vertices.count)
                            return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, output.sourcePath,
                                                                     "Mesh polygon references an invalid vertex."));
                        output.triangleIndices.push_back(mesh->vertex_indices[polygonCorner]);
                    }
                    if (mirrored)
                        std::swap(output.triangleIndices[output.triangleIndices.size() - 1U],
                                  output.triangleIndices[output.triangleIndices.size() - 2U]);
                    output.triangleMaterials.push_back(material);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyGeometry(const ufbx_node *node, PreFracturedSourceNode &output, const std::string_view sourceName,
                                                const CancellationToken &cancellation) {
            const ufbx_mesh *mesh = node->mesh;
            if (!FiniteMatrix(node->geometry_to_world))
                return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, output.sourcePath,
                                                         "Mesh node has a non-finite or singular geometry transform."));
            for (std::size_t index = 0; index < output.geometryToWorld.size(); ++index)
                output.geometryToWorld[index] = node->geometry_to_world.v[index];
            if (mesh->vertices.count > kMaximumVertices || mesh->num_triangles > kMaximumIndices / 3U || mesh->num_faces > kMaximumFaces)
                return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath,
                                                         "Mesh node exceeds the vertex or triangle budget."));
            if (auto positions = CopyPositions(node, output, sourceName, cancellation); positions.HasError())
                return positions;
            return CopyTriangles(node, output, sourceName, cancellation);
        }

        [[nodiscard]] Result<void> CopyNode(const ufbx_node *node, PreFracturedSourceNode &output, const std::string_view sourceName,
                                            const CancellationToken &cancellation, std::size_t &normalizedBytes) {
            if (node->name.data != nullptr)
                output.name.assign(node->name.data, node->name.length);
            output.sourcePath = NodePath(node);
            const ufbx_mesh *mesh = node->mesh;
            if (mesh->vertices.count > kMaximumVertices || mesh->num_triangles > kMaximumIndices / 3U || mesh->num_faces > kMaximumFaces)
                return Result<void>::Failure(
                    SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath, "Mesh exceeds the geometry budget."));
            const std::size_t minimumBytes = mesh->vertices.count * sizeof(std::array<float, 3>) +
                                             mesh->num_triangles * (3U * sizeof(std::uint32_t) + sizeof(std::string));
            if (minimumBytes > kMaximumNormalizedBytes - normalizedBytes)
                return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath,
                                                         "Mesh exceeds the normalized source budget."));
            if (auto copied = CopyGeometry(node, output, sourceName, cancellation); copied.HasError())
                return copied;
            std::size_t nodeBytes = sizeof(PreFracturedSourceNode) + output.positions.size() * sizeof(std::array<float, 3>) +
                                    output.triangleIndices.size() * sizeof(std::uint32_t) +
                                    output.triangleMaterials.size() * sizeof(std::string) + output.name.size() + output.sourcePath.size();
            for (const auto &material : output.triangleMaterials) {
                if (nodeBytes > kMaximumNormalizedBytes || material.size() > kMaximumNormalizedBytes - nodeBytes)
                    return Result<void>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath,
                                                             "Material names exceed the normalized source budget."));
                nodeBytes += material.size();
            }
            if (nodeBytes > kMaximumNormalizedBytes - normalizedBytes)
                return Result<void>::Failure(
                    SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, output.sourcePath, "Normalized FBX exceeds 256 MiB."));
            normalizedBytes += nodeBytes;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<PreFracturedSource> NormalizeScene(const ufbx_scene &scene, const std::string_view sourceName,
                                                                const CancellationToken &cancellation) {
            if (scene.nodes.count > kMaximumSceneNodes)
                return Result<PreFracturedSource>::Failure(
                    SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, {}, "FBX scene exceeds 8192 source nodes."));
            PreFracturedSource result{.sourceName = std::string{sourceName}};
            std::unordered_map<const ufbx_node *, std::uint32_t> meshNodes;
            std::size_t normalizedBytes{};
            for (const ufbx_node *node : scene.nodes) {
                if (node == nullptr || node->mesh == nullptr)
                    continue;
                if (result.nodes.size() == kMaximumNodes)
                    return Result<PreFracturedSource>::Failure(SourceError(ImportErrors::PreFracturedSourceLimit, sourceName,
                                                                           NodePath(node), "FBX has more than 1024 mesh nodes."));
                PreFracturedSourceNode output;
                if (auto copied = CopyNode(node, output, sourceName, cancellation, normalizedBytes); copied.HasError())
                    return Result<PreFracturedSource>::Failure(copied.ErrorValue());
                meshNodes.emplace(node, static_cast<std::uint32_t>(result.nodes.size()));
                result.nodes.push_back(std::move(output));
            }
            if (result.nodes.empty())
                return Result<PreFracturedSource>::Failure(
                    SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, {}, "FBX source contains no mesh nodes."));
            for (const auto &[node, index] : meshNodes) {
                for (const ufbx_node *ancestor = node->parent; ancestor != nullptr; ancestor = ancestor->parent) {
                    if (const auto found = meshNodes.find(ancestor); found != meshNodes.end()) {
                        result.nodes[index].parent = found->second;
                        break;
                    }
                }
            }
            return Result<PreFracturedSource>::Success(std::move(result));
        }

        [[nodiscard]] ufbx_load_opts LoadOptions(const CancellationToken &cancellation) {
            ufbx_load_opts options{};
            options.ignore_animation = true;
            options.ignore_embedded = true;
            options.skip_mesh_parts = true;
            options.skip_skin_vertices = true;
            options.node_depth_limit = 128;
            options.target_axes = ufbx_axes_right_handed_y_up;
            options.target_unit_meters = 1.0;
            options.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
            options.progress_cb.fn = ParseProgress;
            options.progress_cb.user = const_cast<CancellationToken *>(&cancellation);
            options.progress_interval_hint = 64U * 1024U;
            options.temp_allocator.memory_limit = kMaximumParserBytes;
            options.result_allocator.memory_limit = kMaximumParserBytes;
            return options;
        }
    }  // namespace

    /** @copydoc ParsePreFracturedFbx */
    Result<PreFracturedSource> ParsePreFracturedFbx(const std::span<const std::uint8_t> bytes, const std::string_view sourceName,
                                                    const CancellationToken &cancellation) {
        if (bytes.empty() || bytes.size() > kMaximumSourceBytes || sourceName.size() > 4096U)
            return Result<PreFracturedSource>::Failure(
                SourceError(ImportErrors::PreFracturedSourceLimit, sourceName, {}, "FBX source is empty or exceeds 512 MiB."));
        if (cancellation.IsCancellationRequested())
            return Result<PreFracturedSource>::Failure(
                SourceError(ImportErrors::ImportCancelled, sourceName, {}, "Pre-fractured source import was cancelled."));
        const ufbx_load_opts options = LoadOptions(cancellation);
        ufbx_error parserError{};
        std::unique_ptr<ufbx_scene, SceneDeleter> scene{ufbx_load_memory(bytes.data(), bytes.size(), &options, &parserError)};
        if (cancellation.IsCancellationRequested())
            return Result<PreFracturedSource>::Failure(
                SourceError(ImportErrors::ImportCancelled, sourceName, {}, "Pre-fractured source import was cancelled."));
        if (!scene) {
            const std::string detail{parserError.description.data != nullptr ? parserError.description.data : "",
                                     parserError.description.length};
            return Result<PreFracturedSource>::Failure(
                SourceError(ImportErrors::PreFracturedSourceInvalid, sourceName, {}, "FBX parse failed: " + detail));
        }
        return NormalizeScene(*scene, sourceName, cancellation);
    }
}  // namespace Horo::Assets
