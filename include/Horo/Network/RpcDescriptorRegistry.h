#pragma once

/**
 * @file RpcDescriptorRegistry.h
 * @brief Transactional immutable RPC declaration snapshots.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/ReplicationSerializer.h"
#include "Horo/Network/RpcDescriptor.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Network {
    /** @brief Immutable identity-sorted RPC declaration generation. */
    class RpcDescriptorSnapshot final {
    private:
        struct ConstructionKey final {};

    public:
        /** @brief Returns registry-owned declarations in ascending stable identity order. */
        [[nodiscard]] std::span<const RpcDescriptor> Descriptors() const noexcept;
        /** @brief Finds one exact accepted declaration without fallback. */
        [[nodiscard]] Result<const RpcDescriptor *> Find(RpcId id) const;
        /** @brief Returns the canonical semantic declaration-set digest. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;

        /** @brief Constructs canonical storage when presented with the builder-only key. */
        RpcDescriptorSnapshot(ConstructionKey, std::vector<RpcDescriptor> descriptors, const Sha256Digest &fingerprint);

    private:
        friend Result<std::shared_ptr<const RpcDescriptorSnapshot>> BuildRpcDescriptorSnapshot(
            std::span<const RpcDescriptor>, std::span<const ReplicationSerializerDescriptor>, const RpcDescriptorLimits &);
        friend Result<std::shared_ptr<const RpcDescriptorSnapshot>> BuildRpcDescriptorReplacement(
            const std::shared_ptr<const RpcDescriptorSnapshot> &, std::span<const RpcDescriptor>,
            std::span<const ReplicationSerializerDescriptor>, const RpcDescriptorLimits &);

        /** @brief Validates and allocates one initial or replacement snapshot transactionally. */
        [[nodiscard]] static Result<std::shared_ptr<const RpcDescriptorSnapshot>> Build(
            std::span<const RpcDescriptor> descriptors, std::span<const ReplicationSerializerDescriptor> serializers,
            const RpcDescriptorLimits &limits, const std::shared_ptr<const RpcDescriptorSnapshot> *previous);

        std::vector<RpcDescriptor> descriptors_;
        Sha256Digest fingerprint_;
    };

    /** @brief Shared immutable pin used by sessions and later dispatch registries. */
    using RpcDescriptorSnapshotPtr = std::shared_ptr<const RpcDescriptorSnapshot>;

    /**
     * @brief Builds an initial accepted RPC generation without registering executable methods.
     * @param descriptors Complete inert declaration set.
     * @param serializers Accepted typed codec metadata available to parameter schemas.
     * @param limits Finite construction limits.
     * @return Immutable snapshot or a typed validation error; failure publishes nothing.
     */
    [[nodiscard]] Result<RpcDescriptorSnapshotPtr> BuildRpcDescriptorSnapshot(std::span<const RpcDescriptor> descriptors,
                                                                              std::span<const ReplicationSerializerDescriptor> serializers,
                                                                              const RpcDescriptorLimits &limits = {});

    /**
     * @brief Validates a complete compatible replacement while retaining the prior generation.
     * @param previous Prior immutable generation.
     * @param descriptors Complete replacement declaration set.
     * @param serializers Accepted typed codec metadata available to parameter schemas.
     * @param limits Finite construction limits.
     * @return Replacement snapshot or an error with the prior generation unchanged.
     */
    [[nodiscard]] Result<RpcDescriptorSnapshotPtr> BuildRpcDescriptorReplacement(
        const RpcDescriptorSnapshotPtr &previous, std::span<const RpcDescriptor> descriptors,
        std::span<const ReplicationSerializerDescriptor> serializers, const RpcDescriptorLimits &limits = {});
}  // namespace Horo::Network
