#pragma once

/** @file RenderGraphWorkload.h
 * @brief Typed workloads bound to one immutable compiled render graph.
 */
#include "Horo/Runtime/Render/RenderGraphExecution.h"

#include <cstdint>
#include <variant>

namespace Horo::Render {
    /** @brief Clears or preserves one whole single-mip, single-layer, single-sample graph color texture. */
    struct RenderGraphColorAttachment {
        RenderGraphResourceId texture;
        PrimaryOutputAttachment operations; /**< Clear/Load and Store are required to preserve declared graph contents. */
    };

    /** @brief Copies a finite byte range between two distinct graph buffers. */
    struct RenderGraphBufferCopy {
        RenderGraphResourceId source;
        RenderGraphResourceId destination;
        std::size_t sourceOffset{0};
        std::size_t destinationOffset{0};
        std::size_t byteCount{0};
    };

    /** @brief Exact typed operation; an empty workload represents an ordering-only pass. */
    using RenderGraphWorkload = std::variant<std::monostate, PrimaryOutputAttachment, RenderGraphColorAttachment, RenderGraphBufferCopy>;

    /** @brief Explicit operation for one retained pass in compiled order. */
    struct RenderGraphPassWorkload {
        RenderGraphPassRef pass;
        RenderGraphWorkload workload;
    };

    /**
     * @brief Backend instance resolved by the owning frontend for one exact graph identity.
     *
     * The instance is an opaque token in the selected backend's resource namespace, not a
     * native API handle. Callers cannot fabricate tokens or mix frontend/backend owners.
     * Native validation and retained command references remain the backend's responsibility.
     */
    struct RenderGraphResourceInstance {
        RenderGraphResourceId resource;
        std::uint64_t instance{0};
    };

    /**
     * @brief Frontend-owned submission pins released only by the consuming backend on its owner thread.
     *
     * A successful graph admission borrows this stable lease until the exact native command
     * completes, fails terminally, or is abandoned before submission. Backend shutdown releases
     * every outstanding lease before frontend registry destruction. Release is idempotent.
     */
    class IRenderGraphResourceLease {
    public:
        virtual ~IRenderGraphResourceLease() = default;
        /** @brief Releases registry pins after proven native completion or unsent-frame abandonment. */
        virtual void Release() noexcept = 0;
    };

    /** @brief Explicit completion-lease authority after a backend starts immediate native encoding. */
    enum class RenderGraphLeaseAuthority : std::uint8_t {
        Frontend,
        BackendCompletion,
    };

    /** @brief Synchronous transfer receipt; a backend never retains a pointer to this value. */
    struct RenderGraphLeaseTransfer {
        RenderGraphLeaseAuthority authority{RenderGraphLeaseAuthority::Frontend};
    };

    /**
     * @brief Synchronously borrowed compiled graph execution request for an active frame.
     *
     * Every retained pass has exactly one workload in compiled order. Every graph resource
     * has one frontend-resolved instance in graph resource order. All validation precedes
     * encoding; failure publishes no partial frame. No CPU wait or implicit queue remapping
     * is permitted. Multi-queue transfers require separately compiled submission timelines.
     */
    struct RenderGraphExecutionRequest {
        FrameToken frame;
        const CompiledRenderGraphExecution &graph;
        std::span<const RenderGraphPassWorkload> workloads;
        std::span<const RenderGraphResourceInstance> resources;
        IRenderGraphResourceLease *lease{nullptr}; /**< Stable frontend-owned lease required for resident graph work. */
        bool transientResourcesAdmitted{
            false}; /**< Frontend validated the exact realized lifetime proof and acquired its completion lease. */
        RenderGraphLeaseTransfer *transfer{
            nullptr}; /**< Immediate backends record retained ownership before any queued command; never borrowed after ExecuteGraph. */
    };

    /**
     * @brief Validates a finite single-queue graph request before selected-backend native validation.
     * @param request Borrowed graph, exact operation/resource views and completion lease.
     * @return Success or typed malformed, unsupported-workload, queue, or transient-admission failure.
     * @details Checks exact workload/use agreement, bounds, resolved identity coverage and
     * non-overlapping intervals for actual repeated transient instances. Imported storage cannot
     * serve as a transient alias. Native namespaces are compared by resource kind. It
     * creates no native resources and proves no backend instance validity or native synchronization.
     * Unused transient declarations may have no instance. Used transient declarations require
     * the frontend's explicit realized-set admission; every native backend must still validate its own objects.
     */
    [[nodiscard]] Result<void> ValidateRenderGraphExecutionRequest(const RenderGraphExecutionRequest &request);
}  // namespace Horo::Render
