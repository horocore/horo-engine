#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsConvexHullCookInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
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
            std::uint32_t sourceVertexCount{};
            std::uint32_t hullVertexCount{};
            std::uint32_t triangleCount{};
            Math::Aabb bounds;
        };

        struct Triangle final {
            std::uint32_t a{};
            std::array<double, 3> normal{};
        };

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            auto error = MakeError(descriptor, std::move(message));
            return Result<T>::Failure(std::move(error));
        }

        [[nodiscard]] Result<LoadedPhysicsConvexHull> ArtifactFailure(std::string message) {
            return Failure<LoadedPhysicsConvexHull>(PhysicsErrors::ShapeArtifactInvalid, std::move(message));
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

        [[nodiscard]] bool KnownLimits(const PhysicsConvexHullCookLimits &limits) noexcept {
            return Detail::ConvexLimitsAreBounded(limits);
        }

        [[nodiscard]] bool ReadPreamble(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.Bytes(header.magic) && reader.U32(header.payloadVersion) && reader.U32(header.schemaVersion) &&
                   reader.U32(header.algorithmVersion);
        }

        [[nodiscard]] bool ReadIdentity(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.Bytes(header.assetBytes) && reader.U64(header.subresource) && reader.Bytes(header.sourceDigest.bytes) &&
                   reader.Bytes(header.cacheKey.bytes) && reader.Bytes(header.target.digest.bytes);
        }

        [[nodiscard]] bool ReadCountsAndBounds(Reader &reader, ArtifactHeader &header) noexcept {
            return reader.U32(header.sourceVertexCount) && reader.U32(header.hullVertexCount) && reader.U32(header.triangleCount) &&
                   reader.Float(header.bounds.minimum.x) && reader.Float(header.bounds.minimum.y) &&
                   reader.Float(header.bounds.minimum.z) && reader.Float(header.bounds.maximum.x) &&
                   reader.Float(header.bounds.maximum.y) && reader.Float(header.bounds.maximum.z);
        }

        [[nodiscard]] bool ReadHeader(Reader &reader, ArtifactHeader &header) noexcept {
            return ReadPreamble(reader, header) && ReadIdentity(reader, header) && ReadCountsAndBounds(reader, header);
        }

        [[nodiscard]] Result<void> ValidateEnvelope(const ArtifactHeader &header) {
            if (header.magic != Detail::ConvexPayloadMagic || header.payloadVersion != Detail::ConvexPayloadVersion)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Unsupported convex artifact envelope."));
            if (header.schemaVersion != PhysicsConvexHullCookSettings::CurrentSchemaVersion ||
                header.algorithmVersion != PhysicsConvexHullCookSettings::CurrentAlgorithmVersion)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Unsupported convex cooker version."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateIdentity(const ArtifactHeader &header, const PhysicsCookedShapeDescriptor &descriptor,
                                                    const PhysicsShapeCookTargetDigest &expectedTarget) {
            if (Assets::AssetId::FromBytes(header.assetBytes) != descriptor.asset ||
                PhysicsShapeSubresourceId::FromValue(header.subresource) != descriptor.subresource ||
                !DigestEqual(header.cacheKey, *descriptor.cacheKeyDigest) || header.target != expectedTarget)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex artifact identity does not match."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCounts(const ArtifactHeader &header, const PhysicsConvexHullCookLimits &limits) {
            if (header.sourceVertexCount == 0 || header.sourceVertexCount > limits.maxSourceVertices || header.hullVertexCount < 4 ||
                header.hullVertexCount > header.sourceVertexCount || header.hullVertexCount > limits.maxHullVertices)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex artifact vertex counts are invalid."));
            if (header.triangleCount < 4 || header.triangleCount > 2U * header.hullVertexCount - 4U)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex artifact triangle count is invalid."));
            if (!header.bounds.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex artifact bounds are invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeader(const ArtifactHeader &header, const PhysicsCookedShapeDescriptor &descriptor,
                                                  const PhysicsShapeCookTargetDigest &expectedTarget,
                                                  const PhysicsConvexHullCookLimits &limits) {
            if (const auto envelope = ValidateEnvelope(header); envelope.HasError())
                return envelope;
            if (const auto identity = ValidateIdentity(header, descriptor, expectedTarget); identity.HasError())
                return identity;
            return ValidateCounts(header, limits);
        }

        [[nodiscard]] bool ReadVertex(Reader &reader, Math::Vec3 &vertex) noexcept {
            return reader.Float(vertex.x) && reader.Float(vertex.y) && reader.Float(vertex.z);
        }

        [[nodiscard]] bool IsCanonicalVertex(const Math::Vec3 vertex) noexcept {
            if (!Math::IsFinite(vertex))
                return false;
            return !(vertex.x == 0.0F && std::signbit(vertex.x)) && !(vertex.y == 0.0F && std::signbit(vertex.y)) &&
                   !(vertex.z == 0.0F && std::signbit(vertex.z));
        }

        [[nodiscard]] Result<void> ReadTables(Reader &reader, LoadedPhysicsConvexHull &loaded, const ArtifactHeader &header) {
            loaded.vertices.resize(header.hullVertexCount);
            loaded.triangleIndices.resize(static_cast<std::size_t>(header.triangleCount) * 3U);
            for (auto &vertex : loaded.vertices) {
                if (!ReadVertex(reader, vertex) || !IsCanonicalVertex(vertex))
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid convex vertex table."));
            }
            for (auto &index : loaded.triangleIndices) {
                if (!reader.U32(index) || index >= header.hullVertexCount)
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Invalid convex index table."));
            }
            if (!reader.Finished())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Trailing convex artifact bytes."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateVertices(const LoadedPhysicsConvexHull &loaded) {
            auto canonical = loaded.vertices;
            std::ranges::sort(canonical, [](const Math::Vec3 left, const Math::Vec3 right) {
                return std::tuple{left.x, left.y, left.z} < std::tuple{right.x, right.y, right.z};
            });
            if (canonical != loaded.vertices || std::ranges::adjacent_find(canonical) != canonical.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Non-canonical convex vertex table."));
            Math::Aabb computed{.minimum = canonical.front(), .maximum = canonical.front()};
            for (const auto vertex : canonical) {
                computed.minimum.x = std::min(computed.minimum.x, vertex.x);
                computed.minimum.y = std::min(computed.minimum.y, vertex.y);
                computed.minimum.z = std::min(computed.minimum.z, vertex.z);
                computed.maximum.x = std::max(computed.maximum.x, vertex.x);
                computed.maximum.y = std::max(computed.maximum.y, vertex.y);
                computed.maximum.z = std::max(computed.maximum.z, vertex.z);
            }
            if (computed.minimum != loaded.bounds.minimum || computed.maximum != loaded.bounds.maximum)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex bounds do not match vertices."));
            return Result<void>::Success();
        }

        [[nodiscard]] double SignedDistanceNumerator(const Triangle &triangle, const Math::Vec3 origin, const Math::Vec3 point) noexcept {
            return triangle.normal[0] * (static_cast<double>(point.x) - origin.x) +
                   triangle.normal[1] * (static_cast<double>(point.y) - origin.y) +
                   triangle.normal[2] * (static_cast<double>(point.z) - origin.z);
        }

        [[nodiscard]] bool IsOutside(const Triangle &triangle, const Math::Vec3 point,
                                     const std::span<const Math::Vec3> vertices) noexcept {
            const double length = std::sqrt(triangle.normal[0] * triangle.normal[0] + triangle.normal[1] * triangle.normal[1] +
                                            triangle.normal[2] * triangle.normal[2]);
            return SignedDistanceNumerator(triangle, vertices[triangle.a], point) > Detail::ConvexHullPlaneToleranceMeters * length;
        }

        [[nodiscard]] std::array<std::uint32_t, 3> CanonicalTriangle(std::array<std::uint32_t, 3> triangle) noexcept {
            std::ranges::rotate(triangle, std::ranges::min_element(triangle));
            return triangle;
        }

        [[nodiscard]] Result<Triangle> ValidateTriangle(const std::array<std::uint32_t, 3> indices, const Math::Vec3 interior,
                                                        const std::span<const Math::Vec3> vertices) {
            if (indices[0] == indices[1] || indices[1] == indices[2] || indices[0] == indices[2])
                return Failure<Triangle>(PhysicsErrors::ShapeArtifactInvalid, "Degenerate convex triangle.");
            if (CanonicalTriangle(indices) != indices)
                return Failure<Triangle>(PhysicsErrors::ShapeArtifactInvalid, "Non-canonical convex triangle.");
            Triangle triangle{indices[0], Detail::ConvexCross(vertices[indices[0]], vertices[indices[1]], vertices[indices[2]])};
            if (const double areaSquared = triangle.normal[0] * triangle.normal[0] + triangle.normal[1] * triangle.normal[1] +
                                           triangle.normal[2] * triangle.normal[2];
                areaSquared == 0.0)
                return Failure<Triangle>(PhysicsErrors::ShapeArtifactInvalid, "Zero-area convex triangle.");
            if (SignedDistanceNumerator(triangle, vertices[triangle.a], interior) >= 0.0)
                return Failure<Triangle>(PhysicsErrors::ShapeArtifactInvalid, "Inward convex triangle winding.");
            if (std::ranges::any_of(vertices, [&triangle, vertices](const Math::Vec3 vertex) {
                return IsOutside(triangle, vertex, vertices);
            }))
                return Failure<Triangle>(PhysicsErrors::ShapeArtifactInvalid, "Triangle table is not convex.");
            return Result<Triangle>::Success(triangle);
        }

        using Edge = std::pair<std::uint32_t, std::uint32_t>;

        struct EdgeUse final {
            Edge edge;
            std::int32_t orientation{};
        };

        void AddEdge(std::vector<EdgeUse> &edgeUse, const std::uint32_t from, const std::uint32_t to) {
            edgeUse.push_back({.edge = {std::min(from, to), std::max(from, to)}, .orientation = from < to ? 1 : -1});
        }

        [[nodiscard]] bool HasCanonicalEdgeUse(std::vector<EdgeUse> &edgeUse) {
            std::ranges::sort(edgeUse, {}, &EdgeUse::edge);
            for (std::size_t first = 0; first < edgeUse.size();) {
                auto last = first + 1;
                std::int32_t orientation = edgeUse[first].orientation;
                while (last < edgeUse.size() && edgeUse[last].edge == edgeUse[first].edge)
                    orientation += edgeUse[last++].orientation;
                if (last - first != 2 || orientation != 0)
                    return false;
                first = last;
            }
            return true;
        }

        [[nodiscard]] Math::Vec3 ComputeInterior(const std::span<const Math::Vec3> vertices) noexcept {
            std::array<double, 3> sum{};
            for (const auto vertex : vertices) {
                sum[0] += vertex.x;
                sum[1] += vertex.y;
                sum[2] += vertex.z;
            }
            const auto divisor = static_cast<double>(vertices.size());
            return {static_cast<float>(sum[0] / divisor), static_cast<float>(sum[1] / divisor), static_cast<float>(sum[2] / divisor)};
        }

        [[nodiscard]] Result<void> ValidateTriangles(const LoadedPhysicsConvexHull &loaded) {
            const auto interior = ComputeInterior(loaded.vertices);
            std::vector<EdgeUse> edgeUse;
            edgeUse.reserve(loaded.triangleIndices.size());
            std::bitset<PhysicsConvexHullCookLimits::MaximumHullVertices> referenced;
            std::array<std::uint32_t, 3> previous{};
            bool hasPrevious = false;
            for (std::size_t offset = 0; offset < loaded.triangleIndices.size(); offset += 3) {
                const std::array indices{loaded.triangleIndices[offset], loaded.triangleIndices[offset + 1],
                                         loaded.triangleIndices[offset + 2]};
                if (hasPrevious && !(previous < indices))
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Unordered convex triangles."));
                if (const auto triangle = ValidateTriangle(indices, interior, loaded.vertices); triangle.HasError())
                    return Result<void>::Failure(triangle.ErrorValue());
                previous = indices;
                hasPrevious = true;
                referenced.set(indices[0]);
                referenced.set(indices[1]);
                referenced.set(indices[2]);
                AddEdge(edgeUse, indices[0], indices[1]);
                AddEdge(edgeUse, indices[1], indices[2]);
                AddEdge(edgeUse, indices[2], indices[0]);
            }
            if (referenced.count() != loaded.vertices.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Unreferenced convex vertex."));
            if (!HasCanonicalEdgeUse(edgeUse))
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, "Open or non-manifold convex table."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoadRequest(const PhysicsCookedShapeDescriptor &descriptor,
                                                       const PhysicsShapeCookTargetDigest &expectedTarget,
                                                       const std::span<const std::uint8_t> payload,
                                                       const PhysicsConvexHullCookLimits &limits) {
            if (!KnownLimits(limits))
                return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported, "Unsupported convex artifact limits."));
            if (const auto reference = ValidatePhysicsCookedShapeDescriptor(descriptor, expectedTarget); reference.HasError())
                return reference;
            if (descriptor.kind != PhysicsCookedShapeKind::ConvexHull)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Cooked reference does not name a convex hull."));
            if (payload.size() > limits.maxPayloadBytes)
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded, "Convex artifact exceeds runtime limit."));
            if (!DigestEqual(ComputeSha256(std::as_bytes(payload)), *descriptor.payloadDigest))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Convex artifact digest does not match its exact reference."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<LoadedPhysicsConvexHull> DecodeArtifact(const PhysicsCookedShapeDescriptor &descriptor,
                                                                     const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                     const std::span<const std::uint8_t> payload,
                                                                     const PhysicsConvexHullCookLimits &limits) {
            Reader reader{payload};
            ArtifactHeader header;
            if (!ReadHeader(reader, header))
                return ArtifactFailure("Convex artifact header is truncated.");
            if (const auto validation = ValidateHeader(header, descriptor, expectedTarget, limits); validation.HasError())
                return Result<LoadedPhysicsConvexHull>::Failure(validation.ErrorValue());
            LoadedPhysicsConvexHull loaded{.descriptor = descriptor,
                                           .sourceDigest = header.sourceDigest,
                                           .schemaVersion = header.schemaVersion,
                                           .algorithmVersion = header.algorithmVersion,
                                           .bounds = header.bounds};
            if (const auto tables = ReadTables(reader, loaded, header); tables.HasError())
                return Result<LoadedPhysicsConvexHull>::Failure(tables.ErrorValue());
            if (const auto vertices = ValidateVertices(loaded); vertices.HasError())
                return Result<LoadedPhysicsConvexHull>::Failure(vertices.ErrorValue());
            if (const auto triangles = ValidateTriangles(loaded); triangles.HasError())
                return Result<LoadedPhysicsConvexHull>::Failure(triangles.ErrorValue());
            return Result<LoadedPhysicsConvexHull>::Success(std::move(loaded));
        }
    }  // namespace

    /** @copydoc LoadCookedPhysicsConvexHull */
    Result<LoadedPhysicsConvexHull> LoadCookedPhysicsConvexHull(const PhysicsCookedShapeDescriptor &descriptor,
                                                                const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                const std::span<const std::uint8_t> payload,
                                                                const PhysicsConvexHullCookLimits &limits) {
        if (const auto request = ValidateLoadRequest(descriptor, expectedTarget, payload, limits); request.HasError())
            return Result<LoadedPhysicsConvexHull>::Failure(request.ErrorValue());
        try {
            return DecodeArtifact(descriptor, expectedTarget, payload, limits);
        } catch (const std::bad_alloc &) {
            return Failure<LoadedPhysicsConvexHull>(PhysicsErrors::ShapeCookLimitExceeded,
                                                    "Convex artifact storage could not be allocated within runtime limits.");
        }
    }
}  // namespace Horo::Physics
