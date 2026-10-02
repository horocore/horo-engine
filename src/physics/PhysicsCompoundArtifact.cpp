#include "Horo/Physics/PhysicsCompoundCook.h"
#include "PhysicsCompoundCookInternal.h"

#include <new>

namespace Horo::Physics {
    namespace {
        using namespace CompoundDetail;

        /** @brief Decodes a bounded child envelope and delegates all convex semantic verification to Physics. */
        [[nodiscard]] Result<LoadedPhysicsCompoundChild> ReadChild(Reader &reader, const PhysicsShapeCookTargetDigest &target,
                                                                   const PhysicsCompoundCookLimits &limits) {
            std::uint64_t id{};
            std::uint64_t material{};
            std::uint64_t subresource{};
            std::uint64_t size{};
            std::array<std::uint8_t, 16> asset{};
            Sha256Digest key;
            Sha256Digest digest;
            if (!reader.U64(id) || !reader.U64(material) || !reader.Bytes(asset) || !reader.U64(subresource) || !reader.Bytes(key.bytes) ||
                !reader.Bytes(digest.bytes) || !reader.U64(size))
                return Invalid<LoadedPhysicsCompoundChild>("Truncated compound child header.");
            std::span<const std::uint8_t> payload;
            if (id == 0 || material == 0 || !reader.Payload(size, payload))
                return Invalid<LoadedPhysicsCompoundChild>("Invalid compound child identity, slot or extent.");
            const PhysicsCookedShapeDescriptor descriptor{.asset = Assets::AssetId::FromBytes(asset),
                                                          .subresource = PhysicsShapeSubresourceId::FromValue(subresource),
                                                          .kind = PhysicsCookedShapeKind::ConvexHull,
                                                          .cacheKeyDigest = key,
                                                          .payloadDigest = digest,
                                                          .target = target};
            auto hull = LoadCookedPhysicsConvexHull(descriptor, target, payload, limits.convex);
            if (hull.HasError())
                return Result<LoadedPhysicsCompoundChild>::Failure(hull.ErrorValue());
            return Result<LoadedPhysicsCompoundChild>::Success(
                {PhysicsShapeSubresourceId::FromValue(id), PhysicsMaterialSlotId::FromValue(material), std::move(hull).Value()});
        }

        /** @brief Verifies the complete packaged identity before bounded child table allocation. */
        [[nodiscard]] Result<LoadedPhysicsCompound> Decode(const PhysicsCookedShapeDescriptor &descriptor,
                                                           const PhysicsShapeCookTargetDigest &target,
                                                           std::span<const std::uint8_t> payload, const PhysicsCompoundCookLimits &limits) {
            Reader reader{payload};
            std::array<std::uint8_t, 4> magic{};
            std::uint32_t schema{};
            std::uint32_t count{};
            std::array<std::uint8_t, 16> asset{};
            std::uint64_t subresource{};
            PhysicsShapeCookTargetDigest encodedTarget;
            LoadedPhysicsCompound loaded{.descriptor = descriptor};
            if (!reader.Bytes(magic) || !reader.U32(schema) || !reader.Bytes(asset) || !reader.U64(subresource) ||
                !reader.Bytes(encodedTarget.digest.bytes) || !reader.Bytes(loaded.dependencyDigest.bytes) || !reader.U32(count))
                return Invalid<LoadedPhysicsCompound>("Truncated compound header.");
            if (magic != Magic || schema != Schema || Assets::AssetId::FromBytes(asset) != descriptor.asset ||
                PhysicsShapeSubresourceId::FromValue(subresource) != descriptor.subresource || encodedTarget != target)
                return Invalid<LoadedPhysicsCompound>("Compound schema or exact identity does not match.");
            if (count == 0 || count > limits.maximumChildren)
                return Result<LoadedPhysicsCompound>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
            if (count > (payload.size() - HeaderBytes) / ChildHeaderBytes)
                return Invalid<LoadedPhysicsCompound>("Compound child count exceeds its encoded extent.");
            loaded.children.reserve(count);
            PhysicsShapeSubresourceId previous;
            for (std::uint32_t i = 0; i < count; ++i) {
                auto child = ReadChild(reader, target, limits);
                if (child.HasError())
                    return Result<LoadedPhysicsCompound>::Failure(child.ErrorValue());
                if (child.Value().id <= previous)
                    return Invalid<LoadedPhysicsCompound>("Compound children must be strictly ordered by persistent ID.");
                previous = child.Value().id;
                loaded.children.push_back(std::move(child).Value());
            }
            if (!reader.Finished())
                return Invalid<LoadedPhysicsCompound>("Trailing compound artifact bytes.");
            return Result<LoadedPhysicsCompound>::Success(std::move(loaded));
        }
    }  // namespace

    /** @copydoc LoadCookedPhysicsCompound */
    Result<LoadedPhysicsCompound> LoadCookedPhysicsCompound(const PhysicsCookedShapeDescriptor &descriptor,
                                                            const PhysicsShapeCookTargetDigest &expectedTarget,
                                                            std::span<const std::uint8_t> payload,
                                                            const PhysicsCompoundCookLimits &limits) {
        if (!Bounded(limits))
            return Result<LoadedPhysicsCompound>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        if (auto valid = ValidatePhysicsCookedShapeDescriptor(descriptor, expectedTarget); valid.HasError())
            return Result<LoadedPhysicsCompound>::Failure(valid.ErrorValue());
        if (descriptor.kind != PhysicsCookedShapeKind::Compound || payload.size() < HeaderBytes)
            return Invalid<LoadedPhysicsCompound>("Reference does not name a complete compound artifact.");
        if (payload.size() > limits.maximumPayloadBytes)
            return Result<LoadedPhysicsCompound>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
        if (ComputeSha256(std::as_bytes(payload)) != *descriptor.payloadDigest || CookKey(payload) != *descriptor.cacheKeyDigest)
            return Invalid<LoadedPhysicsCompound>("Compound payload or semantic cook key does not match.");
        try {
            return Decode(descriptor, expectedTarget, payload, limits);
        } catch (const std::bad_alloc &) {
            return Result<LoadedPhysicsCompound>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
        }
    }
}  // namespace Horo::Physics
