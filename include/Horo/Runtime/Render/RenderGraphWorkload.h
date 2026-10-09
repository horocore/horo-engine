#pragma once

/** @file RenderGraphWorkload.h
 * @brief Typed workloads bound to one immutable compiled render graph.
 */
#include "Horo/Runtime/Render/RenderGraphExecution.h"

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
    };
}  // namespace Horo::Render
