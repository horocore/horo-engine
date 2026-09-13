#include "Horo/Physics/PhysicsTriangleMeshCook.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsTriangleMeshCookInternal.h"

#include <algorithm>
#include <array>
#include <format>
#include <new>
#include <tuple>
#include <utility>

namespace Horo::Physics {
    namespace {
        constexpr std::array<std::uint8_t, 4> CookKeyMagic{'P', 'H', 'T', 'K'};
        constexpr std::size_t MaximumSourceContextBytes = 256;
        using Writer = Detail::TriangleMeshPayloadWriter;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] std::string SourceMessage(const PhysicsTriangleMeshCookRequest &request, const std::string_view reason) {
            const auto context = request.sourceContext.empty() ? std::string{"<unnamed>"}
                                                               : std::string{request.sourceContext.substr(0, MaximumSourceContextBytes)};
            return std::format("Physics triangle source '{}' (asset {}, subresource {}): {}", context, request.asset.ToString(),
                               request.subresource.Value(), reason);
        }

        [[nodiscard]] bool KnownSettings(const PhysicsTriangleMeshCookSettings &settings) noexcept {
            return settings.schemaVersion == PhysicsTriangleMeshCookSettings::CurrentSchemaVersion &&
                   settings.algorithmVersion == PhysicsTriangleMeshCookSettings::CurrentAlgorithmVersion &&
                   settings.limitPolicy == PhysicsTriangleMeshLimitPolicy::Fail && Detail::TriangleMeshLimitsAreBounded(settings.limits);
        }

        [[nodiscard]] Sha256Digest DigestWriter(const Writer &writer) noexcept {
            return ComputeSha256(std::as_bytes(std::span{writer.View()}));
        }

        [[nodiscard]] Result<void> ValidateRequestIdentity(const PhysicsTriangleMeshCookRequest &request,
                                                           const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Failure<void>(PhysicsErrors::ShapeCookCancelled, SourceMessage(request, "cook was cancelled before validation."));
            if (request.sourceContext.size() > MaximumSourceContextBytes)
                return Failure<void>(PhysicsErrors::ShapeCookSourceInvalid,
                                     SourceMessage(request, "diagnostic source context exceeds 256 bytes."));
            if (!KnownSettings(request.settings))
                return Failure<void>(PhysicsErrors::ProfileUnsupported,
                                     SourceMessage(request, "cook settings or fail-only limit policy are unsupported."));
            if (!request.asset.IsValid() || !request.subresource.IsValid())
                return Failure<void>(PhysicsErrors::ShapeCookSourceInvalid,
                                     SourceMessage(request, "asset and subresource identities must be non-zero."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequestCounts(const PhysicsTriangleMeshCookRequest &request) {
            if (request.vertices.empty() || request.triangles.empty() || request.materialSlots.empty())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, "vertices, triangles and the declared material-slot table must all be non-empty.")));
            if (const auto &limits = request.settings.limits; request.vertices.size() > limits.maxSourceVertices ||
                                                              request.triangles.size() > limits.maxTriangles ||
                                                              request.materialSlots.size() > limits.maxMaterialSlots)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookLimitExceeded,
                              SourceMessage(request,
                                            std::format("source counts ({}, {}, {}) exceed configured limits ({}, {}, {}); fail policy "
                                                        "forbids truncation.",
                                                        request.vertices.size(), request.triangles.size(), request.materialSlots.size(),
                                                        limits.maxSourceVertices, limits.maxTriangles, limits.maxMaterialSlots))));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const PhysicsTriangleMeshCookRequest &request, const CancellationToken &cancellation) {
            if (const auto identity = ValidateRequestIdentity(request, cancellation); identity.HasError())
                return identity;
            return ValidateRequestCounts(request);
        }

        struct CapturedSource final {
            std::vector<Math::Vec3> vertices;
            Sha256Digest digest;
        };

        void WriteSourceTriangle(Writer &bytes, const PhysicsTriangleMeshSourceTriangle &triangle) {
            bytes.U64(triangle.subshape.Value());
            bytes.U32(triangle.vertexIndices[0]);
            bytes.U32(triangle.vertexIndices[1]);
            bytes.U32(triangle.vertexIndices[2]);
            bytes.U64(triangle.materialSlot.Value());
        }

        [[nodiscard]] Result<CapturedSource> CaptureSource(const PhysicsTriangleMeshCookRequest &request,
                                                           const CancellationToken &cancellation) {
            CapturedSource source;
            source.vertices.reserve(request.vertices.size());
            Writer bytes;
            bytes.Reserve(sizeof(std::uint32_t) * 3U + request.vertices.size() * 3U * sizeof(float) + request.triangles.size() * 28U +
                          request.materialSlots.size() * sizeof(std::uint64_t));
            bytes.U32(static_cast<std::uint32_t>(request.vertices.size()));
            for (std::size_t index = 0; index < request.vertices.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return Failure<CapturedSource>(PhysicsErrors::ShapeCookCancelled,
                                                   SourceMessage(request, "cook was cancelled during vertex validation."));
                auto vertex = request.vertices[index];
                if (!Math::IsFinite(vertex))
                    return Failure<CapturedSource>(PhysicsErrors::ShapeCookSourceInvalid,
                                                   SourceMessage(request,
                                                                 std::format("vertex {} contains a non-finite coordinate.", index)));
                if (vertex.x == 0.0F)
                    vertex.x = 0.0F;
                if (vertex.y == 0.0F)
                    vertex.y = 0.0F;
                if (vertex.z == 0.0F)
                    vertex.z = 0.0F;
                bytes.Float(vertex.x);
                bytes.Float(vertex.y);
                bytes.Float(vertex.z);
                source.vertices.push_back(vertex);
            }
            bytes.U32(static_cast<std::uint32_t>(request.triangles.size()));
            for (const auto &triangle : request.triangles)
                WriteSourceTriangle(bytes, triangle);
            bytes.U32(static_cast<std::uint32_t>(request.materialSlots.size()));
            for (const auto slot : request.materialSlots)
                bytes.U64(slot.Value());
            source.digest = DigestWriter(bytes);
            return Result<CapturedSource>::Success(std::move(source));
        }

        struct VertexOrder final {
            Math::Vec3 vertex;
            std::uint32_t sourceIndex{};
        };

        struct CanonicalMesh final {
            std::vector<Math::Vec3> vertices;
            std::vector<LoadedPhysicsTriangle> triangles;
            std::vector<PhysicsMaterialSlotId> materialSlots;
            Math::Aabb bounds;
        };

        [[nodiscard]] Result<std::vector<std::uint32_t>> CanonicalizeVertices(const PhysicsTriangleMeshCookRequest &request,
                                                                              CanonicalMesh &mesh,
                                                                              const std::span<const Math::Vec3> source) {
            std::vector<VertexOrder> order;
            order.reserve(source.size());
            for (std::uint32_t index = 0; index < source.size(); ++index)
                order.push_back({source[index], index});
            std::ranges::sort(order, [](const VertexOrder &left, const VertexOrder &right) {
                return std::tuple{left.vertex.x, left.vertex.y, left.vertex.z} < std::tuple{right.vertex.x, right.vertex.y, right.vertex.z};
            });
            if (std::ranges::adjacent_find(order, [](const VertexOrder &left, const VertexOrder &right) {
                return left.vertex == right.vertex;
            }) != order.end())
                return Failure<std::vector<std::uint32_t>>(PhysicsErrors::ShapeCookSourceInvalid,
                                                           SourceMessage(request,
                                                                         "duplicate normalized vertices require authoring repair."));
            std::vector<std::uint32_t> remap(source.size());
            mesh.vertices.reserve(source.size());
            for (std::uint32_t index = 0; index < order.size(); ++index) {
                remap[order[index].sourceIndex] = index;
                mesh.vertices.push_back(order[index].vertex);
            }
            return Result<std::vector<std::uint32_t>>::Success(std::move(remap));
        }

        [[nodiscard]] Result<void> CanonicalizeMaterialSlots(const PhysicsTriangleMeshCookRequest &request, CanonicalMesh &mesh) {
            mesh.materialSlots.assign(request.materialSlots.begin(), request.materialSlots.end());
            if (std::ranges::any_of(mesh.materialSlots, [](const PhysicsMaterialSlotId slot) {
                return !slot.IsValid();
            }))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid, SourceMessage(request, "material-slot identities must be non-zero.")));
            std::ranges::sort(mesh.materialSlots);
            if (std::ranges::adjacent_find(mesh.materialSlots) != mesh.materialSlots.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid, SourceMessage(request, "material-slot identities must be unique.")));
            return Result<void>::Success();
        }

        [[nodiscard]] std::array<std::uint32_t, 3> CanonicalRotation(std::array<std::uint32_t, 3> indices) noexcept {
            const auto minimum = std::ranges::min_element(indices);
            std::ranges::rotate(indices, minimum);
            return indices;
        }

        [[nodiscard]] Result<void> ValidateAndAppendTriangle(const PhysicsTriangleMeshCookRequest &request,
                                                             const PhysicsTriangleMeshSourceTriangle &sourceTriangle,
                                                             const std::span<const std::uint32_t> remap, CanonicalMesh &mesh) {
            if (!sourceTriangle.subshape.IsValid() || !sourceTriangle.materialSlot.IsValid())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, "triangle subshape and material-slot identities must be non-zero.")));
            if (std::ranges::find(mesh.materialSlots, sourceTriangle.materialSlot) == mesh.materialSlots.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, std::format("triangle {} references undeclared material slot {}.",
                                                                 sourceTriangle.subshape.Value(), sourceTriangle.materialSlot.Value()))));
            if (std::ranges::any_of(sourceTriangle.vertexIndices, [count = remap.size()](const std::uint32_t index) {
                return index >= count;
            }))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, std::format("triangle {} contains an out-of-range vertex index.",
                                                                 sourceTriangle.subshape.Value()))));
            auto indices = sourceTriangle.vertexIndices;
            if (indices[0] == indices[1] || indices[1] == indices[2] || indices[2] == indices[0])
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, std::format("triangle {} repeats a vertex index.", sourceTriangle.subshape.Value()))));
            indices = {remap[indices[0]], remap[indices[1]], remap[indices[2]]};
            if (Detail::TriangleAreaSquared(mesh.vertices[indices[0]], mesh.vertices[indices[1]], mesh.vertices[indices[2]]) <=
                Detail::TriangleMeshMinimumAreaSquared)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, std::format("triangle {} is degenerate at the cook tolerance.",
                                                                 sourceTriangle.subshape.Value()))));
            mesh.triangles.emplace_back(CanonicalRotation(indices), sourceTriangle.subshape, sourceTriangle.materialSlot);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTopology(const PhysicsTriangleMeshCookRequest &request, const CanonicalMesh &mesh) {
            std::vector<std::array<std::uint32_t, 3>> faces;
            std::vector<Detail::TriangleMeshEdgeUse> edges;
            std::vector<bool> used(mesh.vertices.size());
            faces.reserve(mesh.triangles.size());
            edges.reserve(mesh.triangles.size() * 3U);
            for (const auto &triangle : mesh.triangles) {
                auto face = triangle.vertexIndices;
                std::ranges::sort(face);
                faces.push_back(face);
                used[face[0]] = true;
                used[face[1]] = true;
                used[face[2]] = true;
                Detail::AppendTriangleMeshEdges(edges, triangle.vertexIndices);
            }
            std::ranges::sort(faces);
            if (std::ranges::adjacent_find(faces) != faces.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                                                       SourceMessage(request, "duplicate triangle topology is unsupported.")));
            if (std::ranges::find(used, false) != used.end())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid, SourceMessage(request, "source contains an unreferenced vertex.")));
            return Detail::ValidateTriangleMeshEdges(edges, [&request] {
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeCookSourceInvalid,
                              SourceMessage(request, "triangle topology is non-manifold or has inconsistent shared-edge winding.")));
            });
        }

        [[nodiscard]] Result<CanonicalMesh> MakeCanonicalMesh(const PhysicsTriangleMeshCookRequest &request,
                                                              const std::span<const Math::Vec3> source,
                                                              const CancellationToken &cancellation) {
            CanonicalMesh mesh;
            auto remapResult = CanonicalizeVertices(request, mesh, source);
            if (remapResult.HasError())
                return Result<CanonicalMesh>::Failure(remapResult.ErrorValue());
            auto remap = std::move(remapResult).Value();
            if (const auto slots = CanonicalizeMaterialSlots(request, mesh); slots.HasError())
                return Result<CanonicalMesh>::Failure(slots.ErrorValue());
            mesh.triangles.reserve(request.triangles.size());
            for (std::size_t index = 0; index < request.triangles.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return Failure<CanonicalMesh>(PhysicsErrors::ShapeCookCancelled,
                                                  SourceMessage(request, "cook was cancelled during topology validation."));
                if (const auto triangle = ValidateAndAppendTriangle(request, request.triangles[index], remap, mesh); triangle.HasError())
                    return Result<CanonicalMesh>::Failure(triangle.ErrorValue());
            }
            std::ranges::sort(mesh.triangles, [](const LoadedPhysicsTriangle &left, const LoadedPhysicsTriangle &right) {
                return left.subshape.Value() < right.subshape.Value();
            });
            if (std::ranges::adjacent_find(mesh.triangles, [](const LoadedPhysicsTriangle &left, const LoadedPhysicsTriangle &right) {
                return left.subshape == right.subshape;
            }) != mesh.triangles.end())
                return Failure<CanonicalMesh>(PhysicsErrors::ShapeCookSourceInvalid,
                                              SourceMessage(request, "triangle subshape identities must be unique."));
            if (const auto topology = ValidateTopology(request, mesh); topology.HasError())
                return Result<CanonicalMesh>::Failure(topology.ErrorValue());
            mesh.bounds = {.minimum = mesh.vertices.front(), .maximum = mesh.vertices.front()};
            for (const auto vertex : mesh.vertices) {
                mesh.bounds.minimum.x = std::min(mesh.bounds.minimum.x, vertex.x);
                mesh.bounds.minimum.y = std::min(mesh.bounds.minimum.y, vertex.y);
                mesh.bounds.minimum.z = std::min(mesh.bounds.minimum.z, vertex.z);
                mesh.bounds.maximum.x = std::max(mesh.bounds.maximum.x, vertex.x);
                mesh.bounds.maximum.y = std::max(mesh.bounds.maximum.y, vertex.y);
                mesh.bounds.maximum.z = std::max(mesh.bounds.maximum.z, vertex.z);
            }
            return Result<CanonicalMesh>::Success(std::move(mesh));
        }

        void WriteCookKeyIdentity(Writer &writer, const PhysicsTriangleMeshCookRequest &request, const Sha256Digest &sourceDigest) {
            writer.Bytes(CookKeyMagic);
            writer.U32(1);
            writer.Bytes(request.asset.Bytes());
            writer.U64(request.subresource.Value());
            writer.Bytes(sourceDigest.bytes);
        }

        void WriteCookKeySettings(Writer &writer, const PhysicsTriangleMeshCookSettings &settings) {
            writer.U32(settings.schemaVersion);
            writer.U32(settings.algorithmVersion);
            writer.U8(static_cast<std::uint8_t>(settings.limitPolicy));
            writer.U32(settings.limits.maxSourceVertices);
            writer.U32(settings.limits.maxTriangles);
            writer.U32(settings.limits.maxMaterialSlots);
            writer.U64(settings.limits.maxPayloadBytes);
        }

        [[nodiscard]] Sha256Digest ComputeCookKey(const PhysicsTriangleMeshCookRequest &request, const Sha256Digest &sourceDigest) {
            Writer writer;
            WriteCookKeyIdentity(writer, request, sourceDigest);
            WriteCookKeySettings(writer, request.settings);
            writer.Bytes(request.target.digest.bytes);
            return DigestWriter(writer);
        }

        [[nodiscard]] std::size_t PayloadSize(const CanonicalMesh &mesh) noexcept {
            return Detail::TriangleMeshPayloadHeaderBytes + mesh.vertices.size() * 3U * sizeof(float) +
                   mesh.materialSlots.size() * sizeof(std::uint64_t) + mesh.triangles.size() * 28U;
        }

        void WriteHeader(Writer &writer, const PhysicsTriangleMeshCookRequest &request, const Sha256Digest &sourceDigest,
                         const Sha256Digest &cookKey, const CanonicalMesh &mesh, const std::size_t payloadBytes) {
            writer.Bytes(Detail::TriangleMeshPayloadMagic);
            writer.U32(Detail::TriangleMeshPayloadVersion);
            writer.U32(request.settings.schemaVersion);
            writer.U32(request.settings.algorithmVersion);
            writer.Bytes(request.asset.Bytes());
            writer.U64(request.subresource.Value());
            writer.Bytes(sourceDigest.bytes);
            writer.Bytes(cookKey.bytes);
            writer.Bytes(request.target.digest.bytes);
            writer.Float(mesh.bounds.minimum.x);
            writer.Float(mesh.bounds.minimum.y);
            writer.Float(mesh.bounds.minimum.z);
            writer.Float(mesh.bounds.maximum.x);
            writer.Float(mesh.bounds.maximum.y);
            writer.Float(mesh.bounds.maximum.z);
            writer.U32(static_cast<std::uint32_t>(mesh.vertices.size()));
            writer.U32(static_cast<std::uint32_t>(mesh.triangles.size()));
            writer.U32(static_cast<std::uint32_t>(mesh.materialSlots.size()));
            writer.U64(payloadBytes);
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePayload(const PhysicsTriangleMeshCookRequest &request,
                                                                      const Sha256Digest &sourceDigest, const Sha256Digest &cookKey,
                                                                      const CanonicalMesh &mesh) {
            const auto payloadBytes = PayloadSize(mesh);
            if (payloadBytes > request.settings.limits.maxPayloadBytes)
                return Failure<std::vector<std::uint8_t>>(PhysicsErrors::ShapeCookLimitExceeded,
                                                          SourceMessage(request,
                                                                        std::format("{} artifact bytes exceed the configured limit of {}.",
                                                                                    payloadBytes,
                                                                                    request.settings.limits.maxPayloadBytes)));
            Writer writer;
            writer.Reserve(payloadBytes);
            WriteHeader(writer, request, sourceDigest, cookKey, mesh, payloadBytes);
            for (const auto vertex : mesh.vertices) {
                writer.Float(vertex.x);
                writer.Float(vertex.y);
                writer.Float(vertex.z);
            }
            for (const auto slot : mesh.materialSlots)
                writer.U64(slot.Value());
            for (const auto &triangle : mesh.triangles) {
                writer.U64(triangle.subshape.Value());
                writer.U32(triangle.vertexIndices[0]);
                writer.U32(triangle.vertexIndices[1]);
                writer.U32(triangle.vertexIndices[2]);
                writer.U64(triangle.materialSlot.Value());
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(writer).Take());
        }

        [[nodiscard]] PhysicsTriangleMeshCookResult MakeResult(const PhysicsTriangleMeshCookRequest &request,
                                                               const Sha256Digest &sourceDigest, const Sha256Digest &cookKey,
                                                               const CanonicalMesh &mesh, std::vector<std::uint8_t> payload) noexcept {
            const auto payloadDigest = ComputeSha256(std::as_bytes(std::span{payload}));
            return {.descriptor = {.asset = request.asset,
                                   .subresource = request.subresource,
                                   .kind = PhysicsCookedShapeKind::TriangleMesh,
                                   .cacheKeyDigest = cookKey,
                                   .payloadDigest = payloadDigest,
                                   .target = request.target},
                    .sourceDigest = sourceDigest,
                    .bounds = mesh.bounds,
                    .vertexCount = static_cast<std::uint32_t>(mesh.vertices.size()),
                    .triangleCount = static_cast<std::uint32_t>(mesh.triangles.size()),
                    .materialSlotCount = static_cast<std::uint32_t>(mesh.materialSlots.size()),
                    .payload = std::move(payload)};
        }
    }  // namespace

    /** @copydoc CookPhysicsTriangleMesh */
    Result<PhysicsTriangleMeshCookResult> CookPhysicsTriangleMesh(const PhysicsTriangleMeshCookRequest &request,
                                                                  const CancellationToken &cancellation) {
        if (const auto valid = ValidateRequest(request, cancellation); valid.HasError())
            return Result<PhysicsTriangleMeshCookResult>::Failure(valid.ErrorValue());
        try {
            auto sourceResult = CaptureSource(request, cancellation);
            if (sourceResult.HasError())
                return Result<PhysicsTriangleMeshCookResult>::Failure(sourceResult.ErrorValue());
            auto source = std::move(sourceResult).Value();
            auto meshResult = MakeCanonicalMesh(request, source.vertices, cancellation);
            if (meshResult.HasError())
                return Result<PhysicsTriangleMeshCookResult>::Failure(meshResult.ErrorValue());
            auto mesh = std::move(meshResult).Value();
            const auto cookKey = ComputeCookKey(request, source.digest);
            auto payloadResult = EncodePayload(request, source.digest, cookKey, mesh);
            if (payloadResult.HasError())
                return Result<PhysicsTriangleMeshCookResult>::Failure(payloadResult.ErrorValue());
            return Result<PhysicsTriangleMeshCookResult>::Success(
                MakeResult(request, source.digest, cookKey, mesh, std::move(payloadResult).Value()));
        } catch (const std::bad_alloc &) {
            return Failure<PhysicsTriangleMeshCookResult>(PhysicsErrors::ShapeCookLimitExceeded,
                                                          SourceMessage(request, "bounded cook storage could not be allocated."));
        }
    }

    /** @copydoc ValidatePhysicsTriangleMeshMotion */
    Result<void> ValidatePhysicsTriangleMeshMotion(const PhysicsMotionType motion) {
        using enum PhysicsMotionType;
        if (motion == Static)
            return Result<void>::Success();
        if (motion == Kinematic || motion == Dynamic)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::ShapeMotionUnsupported, "Triangle-mesh collision supports static bodies only in CanonicalV1."));
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown body motion mode."));
    }
}  // namespace Horo::Physics
