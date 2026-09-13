#pragma once

/**
 * @file RenderFrontend.h
 * @brief Host-facing owner of one selected and initialized renderer backend.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        class RenderResourceRegistry;
        class RenderResourceUploadQueue;
        class RenderFrontendResourceAccess;
    }  // namespace Detail

    /** @brief Finite frontend admission and per-drain limits for initial resource uploads. */
    struct RenderResourceUploadLimits {
        std::size_t maximumPendingBytes{64U * 1024U * 1024U};  /**< Arena capacity including requested alignment padding. */
        std::size_t maximumBytesPerDrain{16U * 1024U * 1024U}; /**< Maximum payload processed after the progress-guaranteed first item. */
        std::uint32_t maximumRequestsPerDrain{64};             /**< Maximum items in one frame-boundary batch. */
        std::uint32_t maximumPendingRequests{1024};            /**< Maximum metadata records retained by the arena. */
        std::size_t stagingOffsetAlignment{1};                 /**< Power-of-two alignment applied to every staged payload offset. */

        /** @brief Reports whether every upload bound is finite and non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumPendingBytes > 0 && maximumBytesPerDrain > 0 && maximumBytesPerDrain <= maximumPendingBytes &&
                   maximumPendingRequests > 0 && maximumRequestsPerDrain > 0 && maximumRequestsPerDrain <= maximumPendingRequests &&
                   stagingOffsetAlignment > 0 && (stagingOffsetAlignment & (stagingOffsetAlignment - 1U)) == 0 &&
                   stagingOffsetAlignment <= maximumPendingBytes;
        }
    };

    /** @brief Bounded owner-thread upload-arena state captured without exposing backend-native storage. */
    struct RenderResourceUploadSnapshot {
        std::size_t pendingPayloadBytes{0};     /**< Source bytes still waiting for realization. */
        std::size_t occupiedStagingBytes{0};    /**< Arena bytes occupied including alignment padding. */
        std::uint32_t pendingRequests{0};       /**< Metadata records awaiting an owner-thread boundary. */
        std::uint64_t completedBatchCount{0};   /**< Non-empty bounded batches processed by this frontend. */
        std::uint64_t cancelledRequestCount{0}; /**< Pending requests cancelled before native realization. */
        std::size_t lastBatchPayloadBytes{0};   /**< Source payload consumed by the most recent non-empty batch. */
        std::uint32_t lastBatchRequestCount{0}; /**< Requests completed by the most recent non-empty batch. */
        bool acceptingRequests{false};          /**< False after teardown closes producer admission. */
    };

    /** @brief Host-composed renderer memory envelope, default scope, and bounded reclaim policy. */
    struct RenderFrontendMemoryConfig {
        RenderMemoryBudgetConfig budget;
        RenderMemoryScopeId defaultResourceScope{1, 1};
        std::uint32_t maximumEmptyBlocksReclaimedPerDrain{16};

        /** @brief Reports whether the envelope, scope, and reclaim bound are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return budget.IsValid() && defaultResourceScope.IsValid() && maximumEmptyBlocksReclaimedPerDrain > 0;
        }
    };

    /** @brief Host-composed bounds for GPU-completion pins and owner-thread native destruction. */
    struct RenderResourceRetirementLimits {
        std::uint32_t maximumSubmissionPins{4'096};    /**< Maximum accepted resource uses awaiting GPU completion. */
        std::uint32_t maximumTrackedQueues{8};         /**< Maximum logical queue timelines tracked by one frontend. */
        std::uint32_t maximumCompletionsPerDrain{128}; /**< Maximum submission pins inspected at one safe point. */
        std::uint32_t maximumRetirementsPerDrain{64};  /**< Maximum native instances destroyed at one safe point. */

        /** @brief Reports whether every completion and destruction bound is finite and non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumSubmissionPins > 0 && maximumTrackedQueues > 0 && maximumCompletionsPerDrain > 0 &&
                   maximumRetirementsPerDrain > 0;
        }
    };

    /**
     * @brief Move-only owner of one begun backend frame until presentation or abort.
     *
     * Destruction aborts an unpresented frame with its matching token. Destroying
     * the creating frontend first aborts and makes the outstanding scope inert.
     * Execute may succeed exactly once;
     * Present may succeed only after Execute. Invalid stage calls return typed errors
     * without consuming the scope so the caller may recover. Backend failures and
     * exceptions abort the scope before returning.
     *
     * All methods, moves, and destruction must run serially on the same host-declared
     * render-capable thread as the creating frontend. The scope is not thread-safe and
     * must not be transferred across threads or accessed concurrently.
     */
    class RenderFrameScope final {
    public:
        /** @brief Aborts the matching frame when this scope still owns one. */
        ~RenderFrameScope();

        RenderFrameScope(const RenderFrameScope &) = delete;
        RenderFrameScope &operator=(const RenderFrameScope &) = delete;

        /** @brief Transfers matching-frame ownership and leaves source inert. */
        RenderFrameScope(RenderFrameScope &&other) noexcept;

        /** @brief Aborts any currently owned frame, then transfers ownership. */
        RenderFrameScope &operator=(RenderFrameScope &&other) noexcept;

        /**
         * @brief Executes the ordered pass sequence for this frame exactly once.
         * @param orderedPasses Non-owning pass sequence valid for this call.
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Execute(std::span<const RenderPassDescriptor> orderedPasses);

        /**
         * @brief Presents and consumes a successfully executed frame.
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Present();

        /** @brief Explicitly aborts the owned frame; safe to call repeatedly. */
        void Cancel() noexcept;

    private:
        friend class RenderFrontend;

        RenderFrameScope(RenderFrontend &owner, IRenderBackend &backend, FrameToken frame) noexcept;
        void Abort() noexcept;
        void Release() noexcept;

        RenderFrontend *owner_{nullptr};
        IRenderBackend *backend_{nullptr};
        FrameToken frame_{};
        bool executed_{false};
    };

    /**
     * @brief Owns the initialized renderer backend for one host lifetime.
     *
     * Construction performs explicit registry selection and backend initialization.
     * Destruction deterministically shuts the backend down before releasing it.
     * All methods and destruction must run serially on one host-declared render-capable
     * thread. The frontend and its frame scope are not thread-safe.
     */
    class RenderFrontend final {
    public:
        /**
         * @brief Creates and initializes the selected backend from a sealed registry.
         * @param registry Host-owned sealed backend registry.
         * @param backendId Canonical backend identity selected by host policy.
         * @param config Backend-neutral initialization policy.
         * @param uploadLimits Finite initial-upload queue and per-safe-point work limits.
         * @param memoryConfig Finite host envelope, default resource scope, and reclaim bound.
         * @param retirementLimits Finite completion-pin, queue, and owner-thread destruction limits.
         * @return Owned frontend, or the backend creation/initialization failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderFrontend>> Create(const RenderBackendRegistry &registry,
                                                                            const RenderBackendId &backendId,
                                                                            const RenderBackendConfig &config,
                                                                            RenderResourceUploadLimits uploadLimits = {},
                                                                            const RenderFrontendMemoryConfig &memoryConfig = {},
                                                                            const RenderResourceRetirementLimits &retirementLimits = {});

        /** @brief Shuts down and releases the owned backend. */
        ~RenderFrontend();

        RenderFrontend(const RenderFrontend &) = delete;
        RenderFrontend &operator=(const RenderFrontend &) = delete;
        RenderFrontend(RenderFrontend &&) = delete;
        RenderFrontend &operator=(RenderFrontend &&) = delete;

        /**
         * @brief Returns the immutable capability snapshot of the initialized backend.
         * @return Reference valid until frontend destruction; safe to query during an active frame.
         */
        [[nodiscard]] const RenderBackendCapabilities &Capabilities() const noexcept;

        /**
         * @brief Begins one staged frame owned by a move-only recovery scope.
         * @param descriptor Host frame identity and output extent.
         * @return Frame scope, the original typed backend failure, or a translated
         * backend exception. Destroying the frontend first safely invalidates the scope.
         */
        [[nodiscard]] Result<RenderFrameScope> BeginFrame(const FrameDescriptor &descriptor);

        /**
         * @brief Executes and presents one ordered frame, aborting backend frame state on failure.
         * @param descriptor Host frame identity and output extent.
         * @param orderedPasses Non-owning pass sequence valid for this call.
         * @return Success, the original typed backend failure, or a translated backend exception.
         */
        [[nodiscard]] Result<void> SubmitFrame(const FrameDescriptor &descriptor, std::span<const RenderPassDescriptor> orderedPasses);

        /**
         * @brief Commits a framebuffer resize through the owned backend.
         * @param extent Non-zero framebuffer extent committed by the host.
         * @return Backend result, a typed active-frame rejection, or a translated backend exception.
         */
        [[nodiscard]] Result<void> Resize(FramebufferExtent extent);

        /**
         * @brief Attaches the synchronously borrowed static-mesh executor used by frame execution.
         * @param executor Executor that must outlive its attachment or be detached before destruction.
         * @return Success or a typed rejection when an executor is already attached or a frame is active.
         */
        [[nodiscard]] Result<void> AttachStaticMeshPassExecutor(IStaticMeshPassExecutor &executor);

        /** @brief Detaches the matching executor; safe to call repeatedly outside an active frame. */
        void DetachStaticMeshPassExecutor(const IStaticMeshPassExecutor &executor) noexcept;

        /** @brief Creates one logical offscreen target identity with an initial non-zero extent. */
        [[nodiscard]] Result<RenderTargetHandle> CreateOffscreenTarget(FramebufferExtent extent);

        /** @brief Updates the extent associated with a live generation-safe target handle. */
        [[nodiscard]] Result<void> ResizeOffscreenTarget(RenderTargetHandle target, FramebufferExtent extent);

        /** @brief Releases a live target and invalidates its generation; repeated stale release is rejected. */
        [[nodiscard]] Result<void> ReleaseOffscreenTarget(RenderTargetHandle target);

        /**
         * @brief Queues an owned initial upload for one immutable buffer generation.
         * @param descriptor Valid backend-neutral buffer descriptor.
         * @param initialData Bytes copied into the bounded frontend queue before return.
         * @return Pending typed handle and completion operation, or an admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderBufferHandle>> CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                                                std::span<const std::byte> initialData);

        /** @brief Queues one buffer against an explicit admitted owner-scope incarnation. */
        [[nodiscard]] Result<ResourceCreation<RenderBufferHandle>> CreateBuffer(RenderMemoryScopeId scope,
                                                                                const RenderBufferDescriptor &descriptor,
                                                                                std::span<const std::byte> initialData);

        /**
         * @brief Queues one immutable mesh over exact ready vertex and index buffers.
         * @param descriptor Valid mesh descriptor whose dependencies belong to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderMeshHandle>> CreateMesh(const RenderMeshDescriptor &descriptor);

        /**
         * @brief Queues one immutable texture allocation without initial pixel data.
         * @param descriptor Valid backend-neutral texture descriptor.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTextureHandle>> CreateTexture(const RenderTextureDescriptor &descriptor,
                                                                                  std::span<const std::byte> initialData = {});

        /** @brief Queues one texture and optional base-level upload against an explicit owner scope. */
        [[nodiscard]] Result<ResourceCreation<RenderTextureHandle>> CreateTexture(RenderMemoryScopeId scope,
                                                                                  const RenderTextureDescriptor &descriptor,
                                                                                  std::span<const std::byte> initialData = {});

        /**
         * @brief Queues one immutable view over an exact ready texture generation.
         * @param descriptor Valid view descriptor whose texture belongs to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTextureViewHandle>> CreateTextureView(const RenderTextureViewDescriptor &descriptor);

        /**
         * @brief Queues one immutable render target over exact ready attachment views.
         * @param descriptor Valid target descriptor whose views belong to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTargetHandle>> CreateRenderTarget(const RenderTargetDescriptor &descriptor);

        /**
         * @brief Queues a new mesh generation and retires the old generation only after publication.
         * @param current Ready mesh generation to replace without retargeting its dependents.
         * @param descriptor Descriptor for the independent replacement generation.
         * @return Pending replacement and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderMeshHandle>> ReplaceMesh(RenderMeshHandle current,
                                                                             const RenderMeshDescriptor &descriptor);

        /**
         * @brief Processes one bounded upload batch on the render-capable owner thread.
         * @return Number of completed requests, including typed backend failures.
         */
        [[nodiscard]] Result<std::size_t> ProcessResourceRequests();

        /** @brief Returns bounded staging occupancy and completed-batch counters for this frontend. */
        [[nodiscard]] RenderResourceUploadSnapshot UploadSnapshot() const noexcept;

        /** @brief Returns the current state of one buffer generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderBufferHandle buffer) const;

        /** @brief Returns the current state of one mesh generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderMeshHandle mesh) const;

        /** @brief Returns the current state of one texture generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTextureHandle texture) const;

        /** @brief Returns the current state of one texture-view generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTextureViewHandle view) const;

        /** @brief Returns the current state of one generic render-target generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTargetHandle target) const;

        /** @brief Returns success, pending, or the stored typed result for one resource operation. */
        [[nodiscard]] Result<void> ResourceOperationResult(ResourceOperationId operation) const;

        /** @brief Logically releases one buffer generation; dependent meshes retain its native realization. */
        [[nodiscard]] Result<void> ReleaseBuffer(RenderBufferHandle buffer);

        /** @brief Logically releases one mesh generation and drains newly eligible dependencies. */
        [[nodiscard]] Result<void> ReleaseMesh(RenderMeshHandle mesh);

        /** @brief Logically releases one texture generation after its dependent views retire. */
        [[nodiscard]] Result<void> ReleaseTexture(RenderTextureHandle texture);

        /** @brief Logically releases one texture-view generation after dependent targets retire. */
        [[nodiscard]] Result<void> ReleaseTextureView(RenderTextureViewHandle view);

        /** @brief Logically releases one generic render-target generation. */
        [[nodiscard]] Result<void> ReleaseRenderTarget(RenderTargetHandle target);

        /** @brief Returns non-additive renderer memory accounting for the current frontend envelope. */
        [[nodiscard]] RenderMemoryBudgetSnapshot MemorySnapshot() const noexcept;

    private:
        friend class RenderFrameScope;
        friend class Detail::RenderFrontendResourceAccess;

        class ConstructionKey {
            ConstructionKey() = default;
            friend class RenderFrontend;
        };

    public:
        RenderFrontend(std::unique_ptr<IRenderBackend> backend, RenderResourceOwnerId resourceOwner,
                       RenderResourceUploadLimits uploadLimits, std::unique_ptr<RenderMemoryBudget> memoryBudget,
                       const RenderFrontendMemoryConfig &memoryConfig, const RenderResourceRetirementLimits &retirementLimits,
                       ConstructionKey);

    private:
        [[nodiscard]] bool IsLiveTarget(RenderTargetHandle target, FramebufferExtent extent) const noexcept;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderBufferHandle buffer) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderMeshHandle mesh) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTextureViewHandle view) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTargetHandle target) const;
        [[nodiscard]] Result<void> ValidateMeshDependencies(const RenderMeshDescriptor &descriptor) const;
        [[nodiscard]] bool IsMeshBufferLayoutCompatible(const RenderMeshDescriptor &descriptor) const noexcept;
        [[nodiscard]] Result<void> ValidateTextureViewDependency(const RenderTextureViewDescriptor &descriptor) const;
        [[nodiscard]] Result<void> ValidateRenderTargetDependencies(const RenderTargetDescriptor &descriptor) const;
        [[nodiscard]] Result<void> ValidateRenderTargetAttachment(RenderTextureViewHandle handle, RenderTextureAspect requiredAspect,
                                                                  FramebufferExtent extent, std::uint32_t sampleCount) const;

        struct TargetRecord {
            FramebufferExtent extent{};
        };

        struct BufferRecord {
            std::uint32_t generation{0};
            RenderBufferDescriptor descriptor;
        };

        struct TextureRecord {
            std::uint32_t generation{0};
            RenderTextureDescriptor descriptor;
        };

        struct TextureViewRecord {
            std::uint32_t generation{0};
            RenderTextureViewDescriptor descriptor;
        };

        std::unique_ptr<IRenderBackend> backend_;
        std::unique_ptr<RenderMemoryBudget> memoryBudget_;
        RenderFrontendMemoryConfig memoryConfig_;
        std::unique_ptr<Detail::RenderResourceRegistry> resourceRegistry_;
        std::unique_ptr<Detail::RenderResourceUploadQueue> resourceUploadQueue_;
        RenderFrameScope *activeFrameScope_{nullptr};
        IStaticMeshPassExecutor *staticMeshPassExecutor_{nullptr};
        std::vector<TargetRecord> targets_{{}};
        std::vector<BufferRecord> buffers_{{}};
        std::vector<TextureRecord> textures_{{}};
        std::vector<TextureViewRecord> textureViews_{{}};
    };

}  // namespace Horo::Render
