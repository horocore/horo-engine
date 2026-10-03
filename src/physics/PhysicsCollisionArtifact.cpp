#include "Horo/Physics/PhysicsCollisionCooker.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsCollisionArtifactInternal.h"

#include <algorithm>

namespace Horo::Physics {
    namespace {
        /** @brief Verifies canonical tables using the same source-free loaders as runtime cache admission. */
        Result<void> VerifyCollisionPayload(const PhysicsCollisionArtifactView &view, const PhysicsShapeCookTargetDigest &target) {
            const auto &descriptor = view.descriptor;
            switch (descriptor.kind) {
                case PhysicsCookedShapeKind::ConvexHull: {
                    PhysicsConvexHullCookLimits limits{PhysicsConvexHullCookLimits::MaximumSourceVertices,
                                                       PhysicsConvexHullCookLimits::MaximumHullVertices,
                                                       PhysicsConvexHullCookLimits::MaximumPayloadBytes};
                    auto loaded = LoadCookedPhysicsConvexHull(descriptor, target, view.payload, limits);
                    return loaded.HasError() ? Result<void>::Failure(loaded.ErrorValue()) : Result<void>::Success();
                }
                case PhysicsCookedShapeKind::TriangleMesh: {
                    PhysicsTriangleMeshCookLimits limits{PhysicsTriangleMeshCookLimits::MaximumSourceVertices,
                                                         PhysicsTriangleMeshCookLimits::MaximumTriangles,
                                                         PhysicsTriangleMeshCookLimits::MaximumMaterialSlots,
                                                         PhysicsTriangleMeshCookLimits::MaximumPayloadBytes};
                    auto loaded = LoadCookedPhysicsTriangleMesh(descriptor, target, view.payload, limits);
                    return loaded.HasError() ? Result<void>::Failure(loaded.ErrorValue()) : Result<void>::Success();
                }
                case PhysicsCookedShapeKind::HeightField: {
                    PhysicsHeightFieldCookLimits limits{PhysicsHeightFieldCookLimits::MaximumDimension,
                                                        PhysicsHeightFieldCookLimits::MaximumSamples,
                                                        PhysicsHeightFieldCookLimits::MaximumMaterialSlots,
                                                        PhysicsHeightFieldCookLimits::MaximumPayloadBytes};
                    auto loaded = LoadCookedPhysicsHeightField(descriptor, target, view.payload, limits);
                    return loaded.HasError() ? Result<void>::Failure(loaded.ErrorValue()) : Result<void>::Success();
                }
                default:
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            }
        }
    }  // namespace

    /** @copydoc InspectPhysicsCollisionArtifact */
    Result<PhysicsCollisionArtifactView> InspectPhysicsCollisionArtifact(const Assets::AssetId &asset,
                                                                         const std::span<const std::uint8_t> bytes,
                                                                         const PhysicsShapeCookTargetDigest &target) {
        if (bytes.size() < Detail::CollisionArtifactHeaderBytes ||
            bytes.size() - Detail::CollisionArtifactHeaderBytes > PhysicsConvexHullCookLimits::MaximumPayloadBytes ||
            !std::ranges::equal(bytes.first(4), Detail::CollisionArtifactMagic))
            return Result<PhysicsCollisionArtifactView>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid));

        std::uint64_t subresource{};
        for (unsigned index = 0; index < 8; ++index)
            subresource |= static_cast<std::uint64_t>(bytes[5 + index]) << (8U * index);
        std::size_t offset = 13;
        const auto readDigest = [&bytes, &offset] {
            Sha256Digest digest;
            std::ranges::copy(bytes.subspan(offset, 32), digest.bytes.begin());
            offset += 32;
            return digest;
        };
        PhysicsCollisionArtifactView view;
        view.descriptor.asset = asset;
        view.descriptor.subresource = PhysicsShapeSubresourceId::FromValue(subresource);
        view.descriptor.kind = static_cast<PhysicsCookedShapeKind>(bytes[4]);
        view.descriptor.cacheKeyDigest = readDigest();
        view.descriptor.payloadDigest = readDigest();
        view.descriptor.target = PhysicsShapeCookTargetDigest{readDigest()};
        view.sourceDigest = readDigest();
        view.configurationDigest = readDigest();
        view.payload = bytes.subspan(offset);
        if (auto verified = VerifyCollisionPayload(view, target); verified.HasError())
            return Result<PhysicsCollisionArtifactView>::Failure(verified.ErrorValue());
        return Result<PhysicsCollisionArtifactView>::Success(std::move(view));
    }
}  // namespace Horo::Physics
