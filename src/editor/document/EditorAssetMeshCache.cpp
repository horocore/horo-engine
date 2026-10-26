#include "editor/document/EditorAssetMeshCache.h"

#include "Horo/Assets/MeshEditorPayload.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumEditorMeshPayloadBytes = 256U * 1024U * 1024U;
        constexpr std::uint32_t MaximumVertices = 4U * 1024U * 1024U;
        constexpr std::uint32_t MaximumIndices = 24U * 1024U * 1024U;

        [[nodiscard]] Error MeshError(std::string message) {
            return Error{.message = std::move(message)};
        }

        [[nodiscard]] bool ReadU32(const std::span<const std::uint8_t> bytes, const std::size_t offset, std::uint32_t &value) noexcept {
            if (offset > bytes.size() || bytes.size() - offset < 4)
                return false;
            value = static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1]) << 8U |
                    static_cast<std::uint32_t>(bytes[offset + 2]) << 16U | static_cast<std::uint32_t>(bytes[offset + 3]) << 24U;
            return true;
        }

        [[nodiscard]] bool ReadFloat(const std::span<const std::uint8_t> bytes, const std::size_t offset, float &value) noexcept {
            std::uint32_t raw{};
            if (!ReadU32(bytes, offset, raw))
                return false;
            value = std::bit_cast<float>(raw);
            return std::isfinite(value);
        }

        [[nodiscard]] Result<Render::MeshData> Decode(const std::span<const std::uint8_t> bytes) {
            constexpr std::size_t headerBytes = 48;
            std::uint32_t schema{};
            std::uint32_t vertexCount{};
            std::uint32_t faceCount{};
            std::uint32_t positionBytes{};
            std::uint32_t texcoordBytes{};
            std::uint32_t normalBytes{};
            if (bytes.size() < headerBytes || !ReadU32(bytes, 0, schema) || !ReadU32(bytes, 4, vertexCount) ||
                !ReadU32(bytes, 8, faceCount) || !ReadU32(bytes, 36, positionBytes) || !ReadU32(bytes, 40, texcoordBytes) ||
                !ReadU32(bytes, 44, normalBytes) || schema != Assets::MeshEditorPayloadSchemaVersion || vertexCount == 0 ||
                vertexCount > MaximumVertices || positionBytes != vertexCount * 3U * sizeof(float)) {
                return Result<Render::MeshData>::Failure(MeshError("Imported mesh payload header is invalid or unsupported."));
            }
            const std::uint64_t indexHeader = headerBytes + static_cast<std::uint64_t>(positionBytes) + texcoordBytes + normalBytes;
            std::uint32_t indexCount{};
            if (indexHeader > bytes.size() || !ReadU32(bytes, static_cast<std::size_t>(indexHeader), indexCount) || indexCount == 0 ||
                indexCount > MaximumIndices || indexCount % 3U != 0 || faceCount != indexCount / 3U ||
                indexHeader + 4U + static_cast<std::uint64_t>(indexCount) * 4U > bytes.size()) {
                return Result<Render::MeshData>::Failure(MeshError("Imported mesh triangle data is invalid."));
            }

            Render::MeshData mesh;
            mesh.vertices.resize(vertexCount);
            for (std::uint32_t index = 0; index < vertexCount; ++index) {
                Math::Vec3 position;
                if (const std::size_t offset = headerBytes + static_cast<std::size_t>(index) * 12U;
                    !ReadFloat(bytes, offset, position.x) || !ReadFloat(bytes, offset + 4U, position.y) ||
                    !ReadFloat(bytes, offset + 8U, position.z)) {
                    return Result<Render::MeshData>::Failure(MeshError("Imported mesh contains an invalid vertex."));
                }
                mesh.vertices[index] = {position, {}, {}};
            }

            mesh.indices.resize(indexCount);
            for (std::uint32_t index = 0; index < indexCount; ++index) {
                if (!ReadU32(bytes, static_cast<std::size_t>(indexHeader) + 4U + static_cast<std::size_t>(index) * 4U,
                             mesh.indices[index]) ||
                    mesh.indices[index] >= vertexCount) {
                    return Result<Render::MeshData>::Failure(MeshError("Imported mesh contains an invalid triangle index."));
                }
            }

            for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
                Render::MeshVertex &a = mesh.vertices[mesh.indices[index]];
                Render::MeshVertex &b = mesh.vertices[mesh.indices[index + 1]];
                Render::MeshVertex &c = mesh.vertices[mesh.indices[index + 2]];
                const Math::Vec3 face = Math::Cross(b.position - a.position, c.position - a.position);
                if (Math::Length(face) > Math::DefaultEpsilon) {
                    a.normal = a.normal + face;
                    b.normal = b.normal + face;
                    c.normal = c.normal + face;
                }
            }
            for (Render::MeshVertex &vertex : mesh.vertices) {
                const Result<Math::Vec3> normal = Math::TryNormalize(vertex.normal);
                vertex.normal = normal.HasValue() ? normal.Value() : Math::Vec3{0.0F, 1.0F, 0.0F};
            }

            mesh.localBounds = {mesh.vertices.front().position, mesh.vertices.front().position};
            for (const Render::MeshVertex &vertex : mesh.vertices) {
                mesh.localBounds.minimum.x = std::min(mesh.localBounds.minimum.x, vertex.position.x);
                mesh.localBounds.minimum.y = std::min(mesh.localBounds.minimum.y, vertex.position.y);
                mesh.localBounds.minimum.z = std::min(mesh.localBounds.minimum.z, vertex.position.z);
                mesh.localBounds.maximum.x = std::max(mesh.localBounds.maximum.x, vertex.position.x);
                mesh.localBounds.maximum.y = std::max(mesh.localBounds.maximum.y, vertex.position.y);
                mesh.localBounds.maximum.z = std::max(mesh.localBounds.maximum.z, vertex.position.z);
            }
            if (!mesh.IsValid())
                return Result<Render::MeshData>::Failure(MeshError("Imported mesh is empty, degenerate, or unsupported."));
            return Result<Render::MeshData>::Success(std::move(mesh));
        }

        [[nodiscard]] Render::RenderMeshSourceHandle HandleFor(const Assets::AssetId asset, const std::uint32_t generation) noexcept {
            std::uint64_t value = 1469598103934665603ULL;
            for (const std::uint8_t byte : asset.Bytes()) {
                value ^= byte;
                value *= 1099511628211ULL;
            }
            value |= 1ULL << 63U;
            return {{value}, generation};
        }
    }  // namespace

    Result<EditorAssetMeshView> EditorAssetMeshCache::Load(const Assets::AssetId asset, const std::filesystem::path &absolutePath) {
        if (generation_ == 0 || !asset.IsValid() || !absolutePath.is_absolute())
            return Result<EditorAssetMeshView>::Failure(MeshError("Imported mesh reference is invalid."));
        if (const auto found = meshes_.find(asset); found != meshes_.end())
            return Result<EditorAssetMeshView>::Success({HandleFor(asset, generation_), found->second.get()});

        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(absolutePath, error);
            error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
            return Result<EditorAssetMeshView>::Failure(MeshError("Imported mesh file is missing or unsafe to load."));
        const std::uintmax_t size = std::filesystem::file_size(absolutePath, error);

        if (error || size == 0 || size > MaximumEditorMeshPayloadBytes)
            return Result<EditorAssetMeshView>::Failure(MeshError("Imported mesh file exceeds the editor payload limit."));
        std::ifstream stream(absolutePath, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return Result<EditorAssetMeshView>::Failure(MeshError("Imported mesh file could not be read."));
        auto decoded = Decode(bytes);
        if (decoded.HasError())
            return Result<EditorAssetMeshView>::Failure(decoded.ErrorValue());
        auto [inserted, created] = meshes_.try_emplace(asset, std::make_shared<const Render::MeshData>(std::move(decoded).Value()));
        static_cast<void>(created);
        return Result<EditorAssetMeshView>::Success({HandleFor(asset, generation_), inserted->second.get()});
    }

    std::optional<EditorAssetMeshView> EditorAssetMeshCache::Find(const Assets::AssetId asset) const noexcept {
        const auto found = meshes_.find(asset);
        return generation_ == 0 || found == meshes_.end()
                   ? std::nullopt
                   : std::optional{EditorAssetMeshView{HandleFor(asset, generation_), found->second.get()}};
    }

    /** @copydoc EditorAssetMeshCache::Acquire */
    Result<EditorAssetMeshLease> EditorAssetMeshCache::Acquire(const Assets::AssetId asset) const {
        const auto found = meshes_.find(asset);
        if (generation_ == 0 || found == meshes_.end())
            return Result<EditorAssetMeshLease>::Failure(MeshError("Imported mesh source is missing or retired."));
        return Result<EditorAssetMeshLease>::Success({HandleFor(asset, generation_), found->second});
    }

    void EditorAssetMeshCache::Clear() noexcept {
        if (generation_ != 0)
            generation_ = generation_ == std::numeric_limits<std::uint32_t>::max() ? 0 : generation_ + 1;
        meshes_.clear();
    }
}  // namespace Horo::Editor
