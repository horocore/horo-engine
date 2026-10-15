#pragma once

/** @file SceneCellPayloadCodec.h
 * @brief Scene-owned versioned CoreEcs provider payload encoding and semantic admission.
 */
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

namespace Horo::Runtime {
    /** @brief Independent wire and allocation ceilings alongside the existing semantic payload bounds. */
    struct SceneCellPayloadCodecLimits final {
        std::size_t maximumEncodedBytes{};          /**< Complete provider bytes, including its versioned header. */
        std::size_t maximumDecodeAllocationBytes{}; /**< Aggregate owned vector/string allocation admitted before construction. */
        SceneCellPayloadLimits semantic;            /**< Existing owner-enforced semantic storage, entity and dependency bounds. */
    };

    /**
     * @brief Encodes a validated immutable Scene baseline without serializing native object layout.
     * @param payload Existing owner-admitted complete baseline.
     * @param limits Explicit wire/allocation/semantic bounds.
     * @param cancellation Borrowed observer checked between bounded encoding units.
     * @return Complete version-one CoreEcs bytes or typed payload failure, without publication or live Scene access.
     * @details Uses the existing canonical Scene field mapping for all core, authored Physics, AI, navigation,
     * behavior and opaque gameplay components. Capability/schema approval is never serialized as authority.
     * @throws std::bad_alloc on bounded output allocation.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeSceneCellPayload(const RuntimeSceneCellPayload &payload,
                                                                        SceneCellPayloadCodecLimits limits,
                                                                        const CancellationToken &cancellation = {});
    /**
     * @brief Decodes bounded logical fields and reuses the existing Scene cook admission before exposing a baseline.
     * @param partition Sole immutable topology authority.
     * @param bytes Complete versioned CoreEcs provider bytes.
     * @param expected Exact host-captured durable cell publication; wire data cannot replace this fence.
     * @param componentSchemas Host-captured approved opaque component schemas, never trusted from wire declarations.
     * @param behaviorSchemas Host-captured approved behavior schemas.
     * @param limits Mandatory wire, aggregate allocation and semantic ceilings.
     * @param cancellation Borrowed observer checked throughout preparation and final owner admission.
     * @return Complete owner-validated payload, or typed malformed/unsupported/stale/capacity/cancelled failure with no publication.
     * @details Rejects unknown versions, malformed counts/tags, truncated/trailing bytes and invalid typed identities.
     * All decoded allocations are charged before construction; no native pointers, live handles or executable callbacks exist in the
     * format.
     * @throws std::bad_alloc on bounded ownership allocation; no external state is changed.
     */
    [[nodiscard]] Result<RuntimeSceneCellPayload> DecodeSceneCellPayload(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                                         std::span<const std::byte> bytes,
                                                                         const SceneCellPayloadIdentity &expected,
                                                                         std::span<const SceneCellComponentSchema> componentSchemas,
                                                                         std::span<const SceneCellBehaviorSchema> behaviorSchemas,
                                                                         SceneCellPayloadCodecLimits limits,
                                                                         const CancellationToken &cancellation = {});
}  // namespace Horo::Runtime
