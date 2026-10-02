#include "Horo/Destruction/ChunkCollisionCook.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <tuple>

namespace Horo::Destruction {
    namespace {
        using Output = Result<std::shared_ptr<const ChunkCollisionArtifactSet>>;

        /** @brief Canonical field hashing avoids native layout and includes all captured admission settings. */
        void U64(Sha256Builder &hash, std::uint64_t value) {
            std::array<std::byte, 8> bytes{};
            for (unsigned i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::byte>(value >> (i * 8U));
            (void)hash.Update(bytes);
        }

        /** @brief Computes exact generation/configuration evidence used by publication and Physics dependencies. */
        [[nodiscard]] Sha256Digest RequestDigest(const ChunkCollisionCookRequest &request) {
            Sha256Builder hash;
            const auto content = SerializeFractureArtifactContentIdentity(request.content);
            (void)hash.Update(std::as_bytes(std::span{content}));
            (void)hash.Update(std::as_bytes(std::span{request.meshIntegrityDigest.bytes}));
            (void)hash.Update(std::as_bytes(std::span{request.target.digest.bytes}));
            U64(hash, request.convex.schemaVersion);
            U64(hash, request.convex.algorithmVersion);
            U64(hash, static_cast<std::uint8_t>(request.convex.limitPolicy));
            for (const auto *limits : {&request.convex.limits, &request.compound.convex}) {
                U64(hash, limits->maxSourceVertices);
                U64(hash, limits->maxHullVertices);
                U64(hash, limits->maxPayloadBytes);
            }
            U64(hash, request.compound.maximumChildren);
            U64(hash, request.compound.maximumPayloadBytes);
            U64(hash, request.limits.maximumChunksPerDestructible);
            U64(hash, request.limits.maximumArtifactBytes);
            U64(hash, request.limits.maximumTransitionBytes);
            U64(hash, request.limits.maximumResidentBytes);
            U64(hash, request.limits.maximumWorkItemsPerTransition);
            auto materials = std::vector<ChunkCollisionMaterial>{request.materials.begin(), request.materials.end()};
            std::ranges::sort(materials, {}, &ChunkCollisionMaterial::chunk);
            U64(hash, materials.size());
            for (const auto &material : materials) {
                U64(hash, material.chunk.Value());
                U64(hash, material.slot.Value());
            }
            return hash.Finalize();
        }

        /** @brief Persistent Physics leaf identity binds stable chunk/piece membership, independent of target and array index. */
        [[nodiscard]] Physics::PhysicsShapeSubresourceId LeafId(DestructionChunkId chunk, CollisionPieceId piece) {
            Sha256Builder hash;
            U64(hash, 1);
            U64(hash, chunk.Value());
            U64(hash, piece.Value());
            const auto digest = hash.Finalize();
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < sizeof(value); ++i)
                value = (value << 8U) | digest.bytes[i];
            return Physics::PhysicsShapeSubresourceId::FromValue(value == 0 ? 1 : value);
        }

        /** @brief Aggregate budgets admit geometric checks and detached Physics output before allocation. */
        struct Budget final {
            const DestructionLimits &limits;
            std::uint64_t bytes{};
            std::uint64_t work{};

            [[nodiscard]] bool Charge(std::uint64_t addedBytes, std::uint64_t addedWork) {
                if (addedBytes > limits.maximumArtifactBytes - bytes || addedWork > limits.maximumWorkItemsPerTransition - work)
                    return false;
                bytes += addedBytes;
                work += addedWork;
                return true;
            }
        };

        /** @brief Tests one point against a normalized outward triangle plane in canonical meters. */
        [[nodiscard]] bool Outside(const std::array<float, 3> &position, const std::array<float, 3> &origin,
                                   const std::array<double, 3> &normal, double length) {
            double distance = 0;
            for (std::size_t axis = 0; axis < 3; ++axis)
                distance += normal[axis] * (static_cast<double>(position[axis]) - origin[axis]);
            return distance > 1.0e-6 * length;
        }

        /** @brief Rejects invalid or inward/concave planes without silently changing the source topology. */
        [[nodiscard]] bool ConvexPlane(const ChunkCollisionPiece &piece, const std::array<std::uint32_t, 3> &triangle) {
            if (std::ranges::any_of(triangle, [&](auto index) {
                return index >= piece.positions.size();
            }))
                return false;
            const auto &origin = piece.positions[triangle[0]];
            std::array<double, 3> a{};
            std::array<double, 3> b{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                a[axis] = static_cast<double>(piece.positions[triangle[1]][axis]) - origin[axis];
                b[axis] = static_cast<double>(piece.positions[triangle[2]][axis]) - origin[axis];
            }
            const std::array normal{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            return length > 0 && std::isfinite(length) && std::ranges::none_of(piece.positions, [&](const auto &position) {
                return Outside(position, origin, normal, length);
            });
        }

        /** @brief Rejects concave source regions rather than changing collision topology with an enclosing hull. */
        [[nodiscard]] Result<void> ValidateRegion(const ChunkCollisionPiece &piece, Budget &budget, const CancellationToken &cancellation) {
            if (!piece.id.IsValid() || piece.positions.size() < 4 || piece.triangles.size() < 4 || !std::isfinite(piece.volume) ||
                piece.volume <= 0)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (piece.positions.size() > Physics::PhysicsConvexHullCookLimits::MaximumSourceVertices ||
                piece.triangles.size() > budget.limits.maximumWorkItemsPerTransition / piece.positions.size() ||
                !budget.Charge(0, piece.triangles.size() * piece.positions.size()))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            if (const bool finite = std::ranges::all_of(piece.positions,
                                                        [](const auto &position) {
                return std::ranges::all_of(position, [](float value) {
                    return std::isfinite(value);
                });
            });
                !finite)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            for (const auto &triangle : piece.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
                if (!ConvexPlane(piece, triangle))
                    return Result<void>::Failure(
                        MakeError(ChunkMeshCookErrors::InvalidInput, "Collision region is not convex; normalize explicit pieces offline."));
            }
            return Result<void>::Success();
        }

        /** @brief Validates exact upstream provenance and complete explicit per-chunk collision material authority. */
        [[nodiscard]] Result<void> ValidateRequest(const ChunkMeshArtifact &mesh, const ChunkCollisionCookRequest &request) {
            const auto profile = GetDestructionTierProfile(mesh.tier);
            if (profile.HasError())
                return Result<void>::Failure(profile.ErrorValue());
            const auto &limits = request.limits;
            if (const auto &maximum = profile.Value().limits;
                limits.maximumChunksPerDestructible == 0 || limits.maximumChunksPerDestructible > maximum.maximumChunksPerDestructible ||
                limits.maximumArtifactBytes < sizeof(ChunkCollisionArtifactSet) ||
                limits.maximumArtifactBytes > maximum.maximumArtifactBytes || limits.maximumTransitionBytes < limits.maximumArtifactBytes ||
                limits.maximumTransitionBytes > maximum.maximumTransitionBytes ||
                limits.maximumResidentBytes < limits.maximumTransitionBytes || limits.maximumResidentBytes > maximum.maximumResidentBytes ||
                limits.maximumWorkItemsPerTransition == 0 || limits.maximumWorkItemsPerTransition > maximum.maximumWorkItemsPerTransition ||
                mesh.chunks.size() > limits.maximumChunksPerDestructible)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            if (!request.content.IsValid() || request.content != mesh.content || request.meshIntegrityDigest != mesh.integrityDigest)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
            if (auto intact = ValidateChunkMeshArtifact(mesh); intact.HasError())
                return intact;
            if (request.materials.size() != mesh.chunks.size())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
            std::set<DestructionChunkId> seen;
            for (const auto &material : request.materials)
                if (!material.chunk.IsValid() || !material.slot.IsValid() || !seen.insert(material.chunk).second)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
            for (const auto &chunk : mesh.chunks)
                if (!seen.contains(chunk.id) || chunk.collisionPieces.empty())
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            return Result<void>::Success();
        }

        /** @brief Validates one neutral region and cooks a bounded canonical convex leaf through Physics. */
        [[nodiscard]] Result<Physics::PhysicsConvexHullCookResult> CookLeaf(const ChunkMesh &chunk, const ChunkCollisionPiece &piece,
                                                                            const ChunkCollisionCookRequest &request, Budget &budget,
                                                                            const CancellationToken &cancellation) {
            using namespace Physics;
            if (auto valid = ValidateRegion(piece, budget, cancellation); valid.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(valid.ErrorValue());
            if (piece.positions.size() > request.convex.limits.maxSourceVertices)
                return Result<PhysicsConvexHullCookResult>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
            std::vector<Math::Vec3> vertices;
            vertices.reserve(piece.positions.size());
            for (const auto &position : piece.positions)
                vertices.push_back({position[0], position[1], position[2]});
            std::ranges::sort(vertices, {}, [](Math::Vec3 point) {
                return std::tuple{point.x, point.y, point.z};
            });
            vertices.erase(std::ranges::unique(vertices).begin(), vertices.end());
            const auto &settings = request.convex;
            // A conservative bounded face/vertex admission precedes all hull work.
            if (!budget.Charge(0, vertices.size() * settings.limits.maxHullVertices * 8ULL))
                return Result<PhysicsConvexHullCookResult>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            auto hull = CookPhysicsConvexHull({request.content.Asset().Asset(), LeafId(chunk.id, piece.id), vertices, settings,
                                               request.target, "DFR chunk collision"},
                                              cancellation);
            if (hull.HasError())
                return Result<PhysicsConvexHullCookResult>::Failure(hull.ErrorValue());
            return hull;
        }

        /** @brief Produces one complete compound, preserving original Physics validation/capability errors. */
        [[nodiscard]] Result<Physics::PhysicsCompoundCookResult> CookChunk(const ChunkMesh &chunk, const ChunkCollisionCookRequest &request,
                                                                           const Sha256Digest &dependency, Budget &budget,
                                                                           const CancellationToken &cancellation) {
            using namespace Physics;
            if (chunk.collisionPieces.size() > request.compound.maximumChildren ||
                chunk.collisionPieces.size() > PhysicsCompoundCookLimits::MaximumChildren)
                return Result<PhysicsCompoundCookResult>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            std::vector<PhysicsConvexHullCookResult> hulls;
            hulls.reserve(chunk.collisionPieces.size());
            std::uint64_t leafBytes = 0;
            for (const auto &piece : chunk.collisionPieces) {
                auto hull = CookLeaf(chunk, piece, request, budget, cancellation);
                if (hull.HasError())
                    return Result<PhysicsCompoundCookResult>::Failure(hull.ErrorValue());
                if (!budget.Charge(hull.Value().payload.size(), 0))
                    return Result<PhysicsCompoundCookResult>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
                leafBytes += hull.Value().payload.size();
                hulls.push_back(std::move(hull).Value());
            }
            const auto material = std::ranges::find(request.materials, chunk.id, &ChunkCollisionMaterial::chunk);
            std::vector<PhysicsCompoundCookChild> children;
            children.reserve(hulls.size());
            for (std::size_t i = 0; i < hulls.size(); ++i)
                children.emplace_back(PhysicsShapeSubresourceId::FromValue(chunk.collisionPieces[i].id.Value()), material->slot,
                                      hulls[i].descriptor, std::span<const std::uint8_t>{hulls[i].payload});
            auto compound = request.compound;
            compound.maximumPayloadBytes =
                std::min(compound.maximumPayloadBytes, request.limits.maximumArtifactBytes - budget.bytes + leafBytes);
            auto result = CookPhysicsCompound({request.content.Asset().Asset(), PhysicsShapeSubresourceId::FromValue(chunk.id.Value()),
                                               request.target, dependency, children, compound},
                                              cancellation);
            if (result.HasError())
                return result;
            // Physics owns its encoding; account only the measured additional envelope bytes.
            if (result.Value().payload.size() < leafBytes || !budget.Charge(result.Value().payload.size() - leafBytes, children.size()))
                return Result<PhysicsCompoundCookResult>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            return result;
        }
    }  // namespace

    /** @copydoc CookChunkCollision */
    Result<std::shared_ptr<const ChunkCollisionArtifactSet>> CookChunkCollision(const ChunkMeshArtifact &mesh,
                                                                                const ChunkCollisionCookRequest &request,
                                                                                const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
        try {
            if (auto valid = ValidateRequest(mesh, request); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto candidate = std::make_shared<ChunkCollisionArtifactSet>(ChunkCollisionArtifactSet::ConstructionKey());
            candidate->content_ = mesh.content;
            candidate->meshDigest_ = mesh.integrityDigest;
            candidate->target_ = request.target;
            candidate->tier_ = mesh.tier;
            candidate->requestDigest_ = RequestDigest(request);
            Budget budget{request.limits};
            if (!budget.Charge(sizeof(ChunkCollisionArtifactSet) + mesh.chunks.size() * sizeof(ChunkCollisionArtifact), mesh.chunks.size()))
                return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            candidate->shapes_.reserve(mesh.chunks.size());
            for (const auto &chunk : mesh.chunks) {
                auto shape = CookChunk(chunk, request, candidate->requestDigest_, budget, cancellation);
                if (shape.HasError())
                    return Output::Failure(shape.ErrorValue());
                candidate->shapes_.emplace_back(chunk.id, std::move(shape).Value());
            }
            std::ranges::sort(candidate->shapes_, {}, &ChunkCollisionArtifact::chunk);
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            candidate->bytes_ = budget.bytes;
            return Output::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        }
    }

    /** @copydoc ChunkCollisionCookOwner::~ChunkCollisionCookOwner */
    ChunkCollisionCookOwner::~ChunkCollisionCookOwner() {
        Shutdown();
    }

    /** @copydoc ChunkCollisionCookOwner::Revision */
    std::uint64_t ChunkCollisionCookOwner::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc ChunkCollisionCookOwner::Token */
    CancellationToken ChunkCollisionCookOwner::Token() const noexcept {
        return cancellation_.Token();
    }

    /** @copydoc ChunkCollisionCookOwner::Snapshot */
    std::shared_ptr<const ChunkCollisionArtifactSet> ChunkCollisionCookOwner::Snapshot() const noexcept {
        return current_;
    }

    /** @copydoc ChunkCollisionCookOwner::Accept */
    Result<void> ChunkCollisionCookOwner::Accept(std::shared_ptr<const ChunkCollisionArtifactSet> candidate, std::uint64_t expectedRevision,
                                                 const ChunkCollisionCookRequest &current) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Shutdown));
        if (!candidate || expectedRevision != revision_ || candidate->Content() != current.content ||
            candidate->MeshDigest() != current.meshIntegrityDigest || candidate->Target() != current.target ||
            current.materials.size() != candidate->Shapes().size())
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
        try {
            if (candidate->requestDigest_ != RequestDigest(current))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        }
        current_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc ChunkCollisionCookOwner::Invalidate */
    Result<void> ChunkCollisionCookOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Shutdown));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        try {
            CancellationSource replacement;
            cancellation_.RequestCancellation();
            cancellation_ = std::move(replacement);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        }
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc ChunkCollisionCookOwner::Shutdown */
    void ChunkCollisionCookOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
