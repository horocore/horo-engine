#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"
#include "PhysicsTriangleMeshCookInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <new>
#include <tuple>
#include <utility>

namespace Horo::Physics {
    namespace {
        struct ArtifactHeader final {
            std::array<std::uint8_t, 4> magic{};
            std::uint32_t payloadVersion{};
            std::uint32_t schemaVersion{};
            std::uint32_t algorithmVersion{};
            std::array<std::uint8_t, 16> assetBytes{};
            std::uint64_t subresource{};
            Sha256Digest sourceDigest;
            Sha256Digest cacheKey;
            PhysicsShapeCookTargetDigest target;
            Math::Aabb bounds;
            std::uint32_t vertexCount{};
            std::uint32_t triangleCount{};
            std::uint32_t materialSlotCount{};
            std::uint64_t payloadBytes{};
        };

        [[nodiscard]] Result<LoadedPhysicsTriangleMesh> ArtifactFailure(std::string message) {
            return Detail::TriangleMeshFailure<LoadedPhysicsTriangleMesh>(PhysicsErrors::ShapeArtifactInvalid, std::move(message));
        }

        class Reader final {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool U8(std::uint8_t &value) noexcept {
                if (offset_ >= bytes_.size())
                    return false;
                value = bytes_[offset_++];
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &value) noexcept {
                value = 0;
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    std::uint8_t byte{};
                    if (!U8(byte))
                        return false;
                    value |= static_cast<std::uint32_t>(byte) << shift;
                }
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t &value) noexcept {
                value = 0;
                for (unsigned shift = 0; shift < 64; shift += 8) {
                    std::uint8_t byte{};
                    if (!U8(byte))
                        return false;
                    value |= static_cast<std::uint64_t>(byte) << shift;
                }
                return true;
            }

            [[nodiscard]] bool Float(float &value) noexcept {
                std::uint32_t bits{};
                if (!U32(bits))
                    return false;
                value = std::bit_cast<float>(bits);
                return true;
            }

            [[nodiscard]] bool Bytes(const std::span<std::uint8_t> destination) noexcept {
                if (destination.size() > bytes_.size() - offset_)
                    return false;
                std::ranges::copy(bytes_.subspan(offset_, destination.size()), destination.begin());
                offset_ += destination.size();
                return true;
            }

            [[nodiscard]] bool Finished() const noexcept {
                return offset_ == bytes_.size();
            }

        private:
            std::span<const std::uint8_t> bytes_;
            std::size_t offset_{};
        };

        [[nodiscard]] bool DigestEqual(const Sha256Digest &left, const Sha256Digest &right) noexcept {
            const auto leftBytes = std::as_bytes(std::span{left.bytes});
            const auto rightBytes = std::as_bytes(std::span{right.bytes});
            std::byte difference{};
            for (std::size_t index = 0; index < leftBytes.size(); ++index)
                difference |= leftBytes[index] ^ rightBytes[index];
            return difference == std::byte{};
        }

        [[nodiscard]] bool ReadHeaderPreamble(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.Bytes(header.magic) && reader.U32(header.payloadVersion) && reader.U32(header.schemaVersion) &&
                   reader.U32(header.algorithmVersion);
        }

        [[nodiscard]] bool ReadHeaderIdentity(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.Bytes(header.assetBytes) && reader.U64(header.subresource) && reader.Bytes(header.sourceDigest.bytes) &&
                   reader.Bytes(header.cacheKey.bytes) && reader.Bytes(header.target.digest.bytes);
        }

        [[nodiscard]] bool ReadHeaderBounds(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.Float(header.bounds.minimum.x) && reader.Float(header.bounds.minimum.y) &&
                   reader.Float(header.bounds.minimum.z) && reader.Float(header.bounds.maximum.x) &&
                   reader.Float(header.bounds.maximum.y) && reader.Float(header.bounds.maximum.z);
        }

        [[nodiscard]] bool ReadHeaderCounts(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.U32(header.vertexCount) && reader.U32(header.triangleCount) && reader.U32(header.materialSlotCount) &&
                   reader.U64(header.payloadBytes);
        }

        [[nodiscard]] bool ReadHeader(Reader &reader, ArtifactHeader &header) noexcept {
            return ReadHeaderPreamble(reader, header) && ReadHeaderIdentity(reader, header) && ReadHeaderBounds(reader, header) &&
                   ReadHeaderCounts(reader, header);
        }

        [[nodiscard]] Result<void> ValidateHeaderEnvelope(const ArtifactHeader &header) {
            if (header.magic != Detail::TriangleMeshPayloadMagic || header.payloadVersion != Detail::TriangleMeshPayloadVersion ||
                header.schemaVersion != PhysicsTriangleMeshCookSettings::CurrentSchemaVersion ||
                header.algorithmVersion != PhysicsTriangleMeshCookSettings::CurrentAlgorithmVersion)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Unsupported triangle-mesh artifact envelope or cooker version."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeaderIdentity(const ArtifactHeader &header, const PhysicsCookedShapeDescriptor &descriptor,
                                                          const PhysicsShapeCookTargetDigest &expectedTarget) {
            if (Assets::AssetId::FromBytes(header.assetBytes) != descriptor.asset ||
                PhysicsShapeSubresourceId::FromValue(header.subresource) != descriptor.subresource ||
                !DigestEqual(header.cacheKey, *descriptor.cacheKeyDigest) || header.target != expectedTarget)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh artifact identity does not match."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeaderCounts(const ArtifactHeader &header, const PhysicsTriangleMeshCookLimits &limits) {
            if (header.vertexCount == 0 || header.vertexCount > limits.maxSourceVertices || header.triangleCount == 0 ||
                header.triangleCount > limits.maxTriangles || header.materialSlotCount == 0 ||
                header.materialSlotCount > limits.maxMaterialSlots || !header.bounds.IsValid())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh artifact counts or bounds are invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeaderExtent(const ArtifactHeader &header, const PhysicsTriangleMeshCookLimits &limits,
                                                        const std::size_t suppliedPayloadBytes) {
            if (header.payloadBytes != suppliedPayloadBytes || header.payloadBytes > limits.maxPayloadBytes)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh artifact payload size is invalid."));
            const std::uint64_t expectedBytes = Detail::TriangleMeshPayloadHeaderBytes +
                                                static_cast<std::uint64_t>(header.vertexCount) * 3U * sizeof(float) +
                                                static_cast<std::uint64_t>(header.materialSlotCount) * sizeof(std::uint64_t) +
                                                static_cast<std::uint64_t>(header.triangleCount) * 28U;
            if (expectedBytes != header.payloadBytes)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh artifact table extent is inconsistent."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeader(const ArtifactHeader &header, const PhysicsCookedShapeDescriptor &descriptor,
                                                  const PhysicsShapeCookTargetDigest &expectedTarget,
                                                  const PhysicsTriangleMeshCookLimits &limits, const std::size_t suppliedPayloadBytes) {
            if (const auto envelope = ValidateHeaderEnvelope(header); envelope.HasError())
                return envelope;
            if (const auto identity = ValidateHeaderIdentity(header, descriptor, expectedTarget); identity.HasError())
                return identity;
            if (const auto counts = ValidateHeaderCounts(header, limits); counts.HasError())
                return counts;
            return ValidateHeaderExtent(header, limits, suppliedPayloadBytes);
        }

        [[nodiscard]] bool IsCanonicalVertex(const Math::Vec3 vertex) noexcept {
            if (!Math::IsFinite(vertex))
                return false;
            return !(vertex.x == 0.0F && std::signbit(vertex.x)) && !(vertex.y == 0.0F && std::signbit(vertex.y)) &&
                   !(vertex.z == 0.0F && std::signbit(vertex.z));
        }

        [[nodiscard]] bool ReadVertex(Reader &reader, Math::Vec3 &vertex) noexcept {
            return reader.Float(vertex.x) && reader.Float(vertex.y) && reader.Float(vertex.z) && IsCanonicalVertex(vertex);
        }

        [[nodiscard]] bool ReadTriangle(Reader &reader, LoadedPhysicsTriangle &triangle, const std::uint32_t vertexCount) noexcept {
            std::uint64_t subshape{};
            std::uint64_t materialSlot{};
            if (!reader.U64(subshape) || !reader.U32(triangle.vertexIndices[0]) || !reader.U32(triangle.vertexIndices[1]) ||
                !reader.U32(triangle.vertexIndices[2]) || !reader.U64(materialSlot))
                return false;
            triangle.subshape = PhysicsShapeSubresourceId::FromValue(subshape);
            triangle.materialSlot = PhysicsMaterialSlotId::FromValue(materialSlot);
            return triangle.subshape.IsValid() && triangle.materialSlot.IsValid() &&
                   std::ranges::all_of(triangle.vertexIndices, [vertexCount](const std::uint32_t index) {
                return index < vertexCount;
            });
        }

        [[nodiscard]] Result<void> ReadTables(Reader &reader, LoadedPhysicsTriangleMesh &loaded, const ArtifactHeader &header) {
            loaded.vertices.resize(header.vertexCount);
            loaded.materialSlots.resize(header.materialSlotCount);
            loaded.triangles.resize(header.triangleCount);
            for (auto &vertex : loaded.vertices) {
                if (!ReadVertex(reader, vertex))
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid triangle-mesh vertex table."));
            }
            for (auto &slot : loaded.materialSlots) {
                std::uint64_t value{};
                if (!reader.U64(value) || value == 0)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid triangle-mesh material-slot table."));
                slot = PhysicsMaterialSlotId::FromValue(value);
            }
            for (auto &triangle : loaded.triangles) {
                if (!ReadTriangle(reader, triangle, header.vertexCount))
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid triangle-mesh triangle table."));
            }
            if (!reader.Finished())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Trailing triangle-mesh artifact bytes."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateVertices(const LoadedPhysicsTriangleMesh &loaded) {
            if (!std::ranges::is_sorted(loaded.vertices,
                                        [](const Math::Vec3 left, const Math::Vec3 right) {
                return std::tuple{left.x, left.y, left.z} < std::tuple{right.x, right.y, right.z};
            }) ||
                std::ranges::adjacent_find(loaded.vertices) != loaded.vertices.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Non-canonical triangle-mesh vertex table."));
            Math::Aabb bounds{.minimum = loaded.vertices.front(), .maximum = loaded.vertices.front()};
            for (const auto vertex : loaded.vertices) {
                bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
                bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
                bounds.minimum.z = std::min(bounds.minimum.z, vertex.z);
                bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
                bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
                bounds.maximum.z = std::max(bounds.maximum.z, vertex.z);
            }
            if (bounds.minimum != loaded.bounds.minimum || bounds.maximum != loaded.bounds.maximum)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh bounds do not match its vertices."));
            return Result<void>::Success();
        }

        struct TopologyScratch final {
            std::vector<bool> used;
            std::vector<std::array<std::uint32_t, 3>> faces;
            std::vector<Detail::TriangleMeshEdgeUse> edges;
        };

        [[nodiscard]] Result<void> ValidateMaterialSlots(const LoadedPhysicsTriangleMesh &loaded) {
            if (!std::ranges::is_sorted(loaded.materialSlots) ||
                std::ranges::adjacent_find(loaded.materialSlots) != loaded.materialSlots.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Non-canonical triangle-mesh material-slot table."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTriangle(const LoadedPhysicsTriangleMesh &loaded, const LoadedPhysicsTriangle &triangle,
                                                    const std::uint64_t previousSubshape) {
            if (triangle.subshape.Value() <= previousSubshape ||
                std::ranges::find(loaded.materialSlots, triangle.materialSlot) == loaded.materialSlots.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid triangle-mesh identity or material mapping."));
            auto canonicalRotation = triangle.vertexIndices;
            std::ranges::rotate(canonicalRotation, std::ranges::min_element(canonicalRotation));
            if (canonicalRotation != triangle.vertexIndices || triangle.vertexIndices[0] == triangle.vertexIndices[1] ||
                triangle.vertexIndices[1] == triangle.vertexIndices[2] || triangle.vertexIndices[2] == triangle.vertexIndices[0] ||
                Detail::TriangleAreaSquared(loaded.vertices[triangle.vertexIndices[0]], loaded.vertices[triangle.vertexIndices[1]],
                                            loaded.vertices[triangle.vertexIndices[2]]) <= Detail::TriangleMeshMinimumAreaSquared)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid canonical triangle-mesh face."));
            return Result<void>::Success();
        }

        void AppendTopology(TopologyScratch &scratch, const LoadedPhysicsTriangle &triangle) {
            auto face = triangle.vertexIndices;
            std::ranges::sort(face);
            scratch.faces.push_back(face);
            scratch.used[face[0]] = true;
            scratch.used[face[1]] = true;
            scratch.used[face[2]] = true;
            Detail::AppendTriangleMeshEdges(scratch.edges, triangle.vertexIndices);
        }

        [[nodiscard]] Result<void> ValidateCoverage(TopologyScratch &scratch) {
            std::ranges::sort(scratch.faces);
            if (std::ranges::adjacent_find(scratch.faces) != scratch.faces.end() ||
                std::ranges::find(scratch.used, false) != scratch.used.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Duplicate faces or unreferenced vertices in triangle-mesh artifact."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTopology(const LoadedPhysicsTriangleMesh &loaded) {
            if (const auto slots = ValidateMaterialSlots(loaded); slots.HasError())
                return slots;
            TopologyScratch scratch{.used = std::vector<bool>(loaded.vertices.size())};
            scratch.faces.reserve(loaded.triangles.size());
            scratch.edges.reserve(loaded.triangles.size() * 3U);
            std::uint64_t previousSubshape{};
            for (const auto &triangle : loaded.triangles) {
                if (const auto valid = ValidateTriangle(loaded, triangle, previousSubshape); valid.HasError())
                    return valid;
                previousSubshape = triangle.subshape.Value();
                AppendTopology(scratch, triangle);
            }
            if (const auto coverage = ValidateCoverage(scratch); coverage.HasError())
                return coverage;
            return Detail::ValidateTriangleMeshEdges(scratch.edges, [] {
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid,
                                                       "Triangle-mesh artifact is non-manifold or has inconsistent shared-edge winding."));
            });
        }

        [[nodiscard]] Result<void> ValidateLoadRequest(const PhysicsCookedShapeDescriptor &descriptor,
                                                       const PhysicsShapeCookTargetDigest &expectedTarget,
                                                       const std::span<const std::uint8_t> payload,
                                                       const PhysicsTriangleMeshCookLimits &limits) {
            if (!Detail::TriangleMeshLimitsAreBounded(limits))
                return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported, "Unsupported triangle-mesh artifact limits."));
            if (const auto reference = ValidatePhysicsCookedShapeDescriptor(descriptor, expectedTarget); reference.HasError())
                return reference;
            if (descriptor.kind != PhysicsCookedShapeKind::TriangleMesh)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Cooked reference does not name a triangle mesh."));
            if (payload.size() > limits.maxPayloadBytes)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookLimitExceeded, "Triangle-mesh artifact exceeds runtime limit."));
            if (!DigestEqual(ComputeSha256(std::as_bytes(payload)), *descriptor.payloadDigest))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Triangle-mesh artifact digest does not match its exact reference."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<LoadedPhysicsTriangleMesh> DecodeArtifact(const PhysicsCookedShapeDescriptor &descriptor,
                                                                       const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                       const std::span<const std::uint8_t> payload,
                                                                       const PhysicsTriangleMeshCookLimits &limits) {
            Reader reader{payload};
            ArtifactHeader header;
            if (!ReadHeader(reader, header))
                return ArtifactFailure("Triangle-mesh artifact header is truncated.");
            if (const auto headerResult = ValidateHeader(header, descriptor, expectedTarget, limits, payload.size());
                headerResult.HasError())
                return Result<LoadedPhysicsTriangleMesh>::Failure(headerResult.ErrorValue());
            LoadedPhysicsTriangleMesh loaded{.descriptor = descriptor,
                                             .sourceDigest = header.sourceDigest,
                                             .schemaVersion = header.schemaVersion,
                                             .algorithmVersion = header.algorithmVersion,
                                             .bounds = header.bounds};
            if (const auto tables = ReadTables(reader, loaded, header); tables.HasError())
                return Result<LoadedPhysicsTriangleMesh>::Failure(tables.ErrorValue());
            if (const auto vertices = ValidateVertices(loaded); vertices.HasError())
                return Result<LoadedPhysicsTriangleMesh>::Failure(vertices.ErrorValue());
            if (const auto topology = ValidateTopology(loaded); topology.HasError())
                return Result<LoadedPhysicsTriangleMesh>::Failure(topology.ErrorValue());
            return Result<LoadedPhysicsTriangleMesh>::Success(std::move(loaded));
        }
    }  // namespace

    /** @copydoc LoadCookedPhysicsTriangleMesh */
    Result<LoadedPhysicsTriangleMesh> LoadCookedPhysicsTriangleMesh(const PhysicsCookedShapeDescriptor &descriptor,
                                                                    const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                    const std::span<const std::uint8_t> payload,
                                                                    const PhysicsTriangleMeshCookLimits &limits) {
        if (const auto request = ValidateLoadRequest(descriptor, expectedTarget, payload, limits); request.HasError())
            return Result<LoadedPhysicsTriangleMesh>::Failure(request.ErrorValue());
        try {
            return DecodeArtifact(descriptor, expectedTarget, payload, limits);
        } catch (const std::bad_alloc &) {
            return Detail::TriangleMeshFailure<
                LoadedPhysicsTriangleMesh>(PhysicsErrors::ShapeCookLimitExceeded,
                                           "Triangle-mesh artifact storage could not be allocated within runtime limits.");
        }
    }
}  // namespace Horo::Physics
