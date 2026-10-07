#pragma once

/** @file PhysicsCellAttachments.h
 * @brief Exact cooked-shape admission joining Physics resources to streamed Scene publication.
 */
#include "Horo/Physics/PhysicsCookedShapeCache.h"
#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Runtime/Scene/SceneCellAttachments.h"

namespace Horo::Physics {
    /** @brief One exact immutable feature-owned cooked descriptor and streaming reference. */
    struct PhysicsCellAttachment final {
        WorldStreaming::CellAttachmentReference reference;
        PhysicsCookedShapeDescriptor shape;
    };

    /** @brief Creates an explicit Physics attachment factory using real verified shape leases and native Scene preparation.
     * @param identity Stable host-composed provider identity. @param revision Exact implementation/configuration revision.
     * @param version Exact supported attachment schema. @param cache Qualified Physics shape owner; outlives preparation.
     * @param participant Explicitly selected Physics/Character Scene owner; outlives preparation and returned candidates.
     * @param attachments Complete immutable bounded descriptor membership, copied before return.
     * @param maximumAttachments Positive hard membership ceiling.
     * @return Provider binding or typed invalid/unsupported/capacity failure; no resource acquired on failure.
     * @details Each reference must exactly bind its descriptor asset, subresource and payload digest. The factory acquires
     * the exact verified cooked resource before invoking real Physics Scene preparation. All resource/native errors survive.
     * Native publication stays at the common Scene barrier, and immutable shape leases survive cache eviction and shutdown.
     * No runtime recooking, provider discovery or backend selection occurs. Compose one aggregate Physics reference per cell
     * when the underlying participant owns one complete Physics world; do not publish several competing world candidates.
     */
    [[nodiscard]] Result<Runtime::SceneCellAttachmentProvider> MakePhysicsCellAttachmentProvider(
        WorldStreaming::StreamingRuntimeServiceId identity, WorldStreaming::StreamingRuntimeServiceRevision revision, std::uint32_t version,
        const PhysicsCookedShapeCache &cache, PhysicsSceneActivationParticipant &participant,
        std::span<const PhysicsCellAttachment> attachments, std::size_t maximumAttachments);
}  // namespace Horo::Physics
