#include "Horo/Physics/PhysicsCompoundCook.h"

#include "PhysicsCompoundCookInternal.h"

#include <new>

namespace Horo::Physics {
    namespace {
        using namespace CompoundDetail;

        /** @brief Verifies each detached child before its bytes can enter a compound artifact. */
        [[nodiscard]] Result<std::uint64_t> ValidateChildren(const PhysicsCompoundCookRequest &request,
                                                             const CancellationToken &cancellation) {
            std::uint64_t bytes = HeaderBytes;
            std::vector<PhysicsShapeSubresourceId> ids;
            ids.reserve(request.children.size());
            for (const auto &child : request.children) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::uint64_t>::Failure(MakeError(PhysicsErrors::ShapeCookCancelled));
                if (!child.id.IsValid() || !child.material.IsValid())
                    return Invalid<std::uint64_t>("Compound child identity or material slot is invalid.");
                ids.push_back(child.id);
                if (ChildHeaderBytes > request.limits.maximumPayloadBytes - bytes ||
                    child.payload.size() > request.limits.maximumPayloadBytes - bytes - ChildHeaderBytes)
                    return Result<std::uint64_t>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
                bytes += ChildHeaderBytes + child.payload.size();
                const auto hull = LoadCookedPhysicsConvexHull(child.descriptor, request.target, child.payload, request.limits.convex);
                if (hull.HasError())
                    return Result<std::uint64_t>::Failure(hull.ErrorValue());
            }
            std::ranges::sort(ids);
            if (std::ranges::adjacent_find(ids) != ids.end())
                return Invalid<std::uint64_t>("Compound child identities must be unique.");
            return Result<std::uint64_t>::Success(bytes);
        }

        /** @brief Encodes exact verified child references and bytes in canonical stable-ID order. */
        [[nodiscard]] PhysicsCompoundCookResult Encode(const PhysicsCompoundCookRequest &request, std::uint64_t bytes) {
            Writer writer;
            writer.Reserve(static_cast<std::size_t>(bytes));
            writer.Bytes(Magic);
            writer.U32(Schema);
            writer.Bytes(request.asset.Bytes());
            writer.U64(request.subresource.Value());
            writer.Bytes(request.target.digest.bytes);
            writer.Bytes(request.dependencyDigest.bytes);
            writer.U32(static_cast<std::uint32_t>(request.children.size()));
            std::vector<const PhysicsCompoundCookChild *> children;
            children.reserve(request.children.size());
            for (const auto &child : request.children)
                children.push_back(&child);
            std::ranges::sort(children, {}, [](const auto *child) {
                return child->id;
            });
            for (const auto *child : children) {
                writer.U64(child->id.Value());
                writer.U64(child->material.Value());
                writer.Bytes(child->descriptor.asset.Bytes());
                writer.U64(child->descriptor.subresource.Value());
                writer.Bytes(child->descriptor.cacheKeyDigest->bytes);
                writer.Bytes(child->descriptor.payloadDigest->bytes);
                writer.U64(child->payload.size());
                writer.Bytes(child->payload);
            }
            PhysicsCompoundCookResult result;
            result.payload = std::move(writer).Take();
            result.descriptor = {.asset = request.asset,
                                 .subresource = request.subresource,
                                 .kind = PhysicsCookedShapeKind::Compound,
                                 .cacheKeyDigest = CookKey(result.payload),
                                 .payloadDigest = ComputeSha256(std::as_bytes(std::span{result.payload})),
                                 .target = request.target};
            return result;
        }

    }  // namespace

    /** @copydoc CookPhysicsCompound */
    Result<PhysicsCompoundCookResult> CookPhysicsCompound(const PhysicsCompoundCookRequest &request,
                                                          const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<PhysicsCompoundCookResult>::Failure(MakeError(PhysicsErrors::ShapeCookCancelled));
        if (!Bounded(request.limits))
            return Result<PhysicsCompoundCookResult>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        if (!request.asset.IsValid() || !request.subresource.IsValid() || request.children.empty())
            return Invalid<PhysicsCompoundCookResult>("Compound identity or child set is missing.");
        if (request.children.size() > request.limits.maximumChildren)
            return Result<PhysicsCompoundCookResult>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
        try {
            const auto verified = ValidateChildren(request, cancellation);
            if (verified.HasError())
                return Result<PhysicsCompoundCookResult>::Failure(verified.ErrorValue());
            auto result = Encode(request, verified.Value());
            if (cancellation.IsCancellationRequested())
                return Result<PhysicsCompoundCookResult>::Failure(MakeError(PhysicsErrors::ShapeCookCancelled));
            return Result<PhysicsCompoundCookResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<PhysicsCompoundCookResult>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
        }
    }

}  // namespace Horo::Physics
