#pragma once

/**
 * @file RenderBackend.h
 * @brief Backend-neutral renderer lifecycle, frame, capability, and execution-plan contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/LightFrameUpload.h"
#include "Horo/Runtime/Render/MaterialBindingBackend.h"
#include "Horo/Runtime/Render/PresentMode.h"
#include "Horo/Runtime/Render/RenderAdapter.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "Horo/Runtime/Render/RenderMemoryTypes.h"
#include "Horo/Runtime/Render/RenderParallelWork.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/Texture.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Horo::Render {
    struct RenderGraphExecutionRequest;

    /** @brief Stable renderer backend identity used by configuration and registries. */
    class RenderBackendId {
    public:
        RenderBackendId() = default;

        /** @brief Creates an identity from its stable machine-readable value. */
        explicit RenderBackendId(std::string value) : value_(std::move(value)) {}

        /** @brief Returns the stable machine-readable identity. */
        [[nodiscard]] const std::string &Value() const noexcept {
            return value_;
        }

        /** @brief Reports whether the identity uses canonical lowercase ASCII slug form. */
        [[nodiscard]] bool IsValid() const noexcept {
            constexpr std::size_t maxLength = 64;
            const auto isLowerAlpha = [](const char value) {
                return value >= 'a' && value <= 'z';
            };
            const auto isDigit = [](const char value) {
                return value >= '0' && value <= '9';
            };

            if (value_.empty() || value_.size() > maxLength || !isLowerAlpha(value_.front())) {
                return false;
            }
            for (const char value : value_) {
                if (!isLowerAlpha(value) && !isDigit(value) && value != '-') {
                    return false;
                }
            }
            return value_.back() != '-';
        }

        [[nodiscard]] auto operator<=>(const RenderBackendId &) const noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Backend-neutral host presentation family required before window creation. */
    enum class RenderPresentationKind : std::uint8_t {
        None,
        OpenGL,
        Metal,
        Vulkan,
    };

    /** @brief Immutable host-window policy published before native backend loading. */
    struct RenderHostWindowRequirements {
        RenderPresentationKind presentation{RenderPresentationKind::None};
        bool resizable{true};
        bool highPixelDensity{true};
    };

    /** @brief Native-free renderer module metadata available before window and device creation. */
    struct RenderBackendModuleInfo {
        RenderBackendId id;
        std::string displayName;
        RenderHostWindowRequirements windowRequirements;
        bool supportsInteractivePresentation{false};
    };

    /** @brief Host-owned renderer initialization policy with no native API values. */
    struct RenderBackendConfig {
        std::optional<RenderAdapterId> adapter;    /**< Exact host-selected adapter, or backend-defined deterministic default. */
        std::uint64_t adapterDiscoveryRevision{0}; /**< Discovery revision paired with an explicit adapter. */
        bool requirePresentation{false};
        bool enableValidation{false};
        std::uint32_t maxFramesInFlight{2};
        PresentMode presentMode{PresentMode::Fifo};

        /** @brief Reports whether ranges and presentation policy are valid. */
        [[nodiscard]] bool IsValid() const noexcept {
            if (adapter.has_value() != (adapterDiscoveryRevision != 0) || (adapter && !adapter->IsValid())) {
                return false;
            }
            if (maxFramesInFlight == 0 || maxFramesInFlight > 8) {
                return false;
            }
            switch (presentMode) {
                case PresentMode::Fifo:
                case PresentMode::Immediate:
                    return true;
                default:
                    return false;
            }
        }
    };

    /** @brief Immutable capabilities reported by one initialized backend instance. */
    struct RenderBackendCapabilities {
        RenderBackendId backend;
        bool presentsToWindow{false};
        bool supportsOffscreenTargets{false};
        bool supportsTimestampQueries{false};
        bool supportsCompute{false};
        bool supportsBindlessResources{false};
        bool supportsRayTracing{false};
        bool supportsBufferResources{false};
        bool supportsMeshResources{false};
        bool supportsTextureResources{false};
        bool supportsRenderTargetResources{false};
        /**
         * @brief Admits exact-descriptor transient object reuse on one effective queue.
         *
         * The selected backend must validate resolved instances before encoding and provide
         * equivalent visibility between consecutive non-overlapping logical occupants.
         * This grants no permission for overlapping resources or distinct placed-resource aliasing.
         */
        bool supportsExactTransientResourceReuse{false};
        /** @brief Modern immutable feature, queue, limit, and format support snapshot. */
        RenderCapabilitySnapshot support;
    };

    /** @brief Describes one host frame before backend work begins. */
    struct FrameDescriptor {
        std::uint64_t frameNumber{0};
        FramebufferExtent outputExtent;
    };

    /** @brief Opaque frame identity issued by the active backend. */
    struct FrameToken {
        std::uint64_t value{0};

        /** @brief Reports whether the token was issued by a backend. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const FrameToken &) const noexcept = default;
    };

    /** @brief Stable identity of one pass in a compiled execution plan. */
    struct RenderPassId {
        std::uint32_t value{0};

        /** @brief Reports whether the pass identity is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderPassId &) const noexcept = default;
    };

    /** @brief Backend-neutral category of work performed by one render pass. */
    enum class RenderPassKind : std::uint8_t {
        Graphics,
        Compute,
        Copy,
    };

    /** @brief Backend-neutral load behavior for one pass attachment. */
    enum class AttachmentLoadOperation : std::uint8_t {
        Load,
        Clear,
        DontCare,
    };

    /** @brief Backend-neutral store behavior for one pass attachment. */
    enum class AttachmentStoreOperation : std::uint8_t {
        Store,
        DontCare,
    };

    /** @brief Linear RGBA clear value; finite HDR values are permitted. */
    struct ClearColor {
        float red{0.0F};
        float green{0.0F};
        float blue{0.0F};
        float alpha{1.0F};

        /** @brief Reports whether every component is finite. */
        [[nodiscard]] bool IsFinite() const noexcept {
            return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha);
        }
    };

    /**
     * @brief Color attachment operations targeting the host's current primary output.
     *
     * This initial output contract has no native surface identity. Typed multi-output
     * target handles will supersede the implicit current primary output before
     * multi-window presentation is introduced.
     */
    struct PrimaryOutputAttachment {
        AttachmentLoadOperation loadOperation{AttachmentLoadOperation::Clear};
        AttachmentStoreOperation storeOperation{AttachmentStoreOperation::Store};
        ClearColor clearColor{};

        /** @brief Reports whether load/store operations and the used clear value are valid. */
        [[nodiscard]] bool IsValid() const noexcept {
            switch (storeOperation) {
                case AttachmentStoreOperation::Store:
                case AttachmentStoreOperation::DontCare:
                    break;
                default:
                    return false;
            }
            switch (loadOperation) {
                using enum Horo::Render::AttachmentLoadOperation;
                case AttachmentLoadOperation::Load:
                case AttachmentLoadOperation::DontCare:
                    return true;
                case AttachmentLoadOperation::Clear:
                    return clearColor.IsFinite();
                default:
                    return false;
            }
        }
    };

    /** @brief Generic static-mesh workload rendered into one generation-safe offscreen target. */
    struct StaticMeshPassDescriptor {
        RenderTargetHandle target;
        FramebufferExtent extent;
        RenderSceneView scene;
        ClearColor clearColor{0.035F, 0.045F, 0.070F, 1.0F};

        /** @brief Reports whether the target, extent, scene, and clear value are valid. */
        [[nodiscard]] bool IsValid() const noexcept {
            return target.IsValid() && extent.IsValid() && scene.IsValid() && clearColor.IsFinite();
        }
    };

    /** @brief Native-free executor attached to the frontend for static-mesh pass encoding. */
    class IStaticMeshPassExecutor {
    public:
        virtual ~IStaticMeshPassExecutor() = default;

        /**
         * @brief Encodes one validated static-mesh pass into the active backend frame.
         * @param descriptor Synchronously borrowed generic scene workload.
         * @return Success or a typed resource/encoding failure.
         */
        [[nodiscard]] virtual Result<void> ExecuteStaticMeshPass(const StaticMeshPassDescriptor &descriptor) = 0;
    };

    /** @brief Minimal compiled pass metadata consumed in deterministic order. */
    struct RenderPassDescriptor {
        RenderPassId id;
        RenderPassKind kind{RenderPassKind::Graphics};
        std::optional<PrimaryOutputAttachment> primaryOutput;
        std::optional<StaticMeshPassDescriptor> staticMesh;
    };

    /** @brief Frame-bound ordered pass view submitted to the selected backend. */
    struct RenderExecutionPlan {
        FrameToken frame;
        std::span<const RenderPassDescriptor> orderedPasses;
    };

    /** @brief Backend-neutral resource allocation and destruction contract. */
    class IRenderResourceBackend {
    public:
        virtual ~IRenderResourceBackend() = default;

        /**
         * @brief Queries native-free backing requirements before a buffer allocation is admitted.
         * @param descriptor Valid backend-neutral buffer descriptor.
         * @return Exact or conservative requirements, or a typed unsupported/failure result.
         * @throws std::bad_alloc Owned requirement or error metadata allocation failed.
         * @throws std::length_error Owned metadata exceeds its representable capacity.
         * @details Expected descriptor, capability and native failures must use Result and retain
         * their original error identity. Implementations may throw only the two documented
         * metadata exceptions; other exceptions violate this callback contract. Queries must
         * release temporary native probes before returning or unwinding. This is not noexcept
         * because the returned Error owns its metadata.
         */
        [[nodiscard]] virtual Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const = 0;

        /**
         * @brief Queries native-free backing requirements before a texture allocation is admitted.
         * @param descriptor Valid backend-neutral texture descriptor.
         * @return Exact or conservative requirements, or a typed unsupported/failure result.
         * @throws std::bad_alloc Owned requirement or error metadata allocation failed.
         * @throws std::length_error Owned metadata exceeds its representable capacity.
         * @details Expected descriptor, capability and native failures must use Result and retain
         * their original error identity. Implementations may throw only the two documented
         * metadata exceptions; other exceptions violate this callback contract. Queries must
         * release temporary native probes before returning or unwinding. This is not noexcept
         * because the returned Error owns its metadata.
         */
        [[nodiscard]] virtual Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const = 0;

        /**
         * @brief Realizes one validated immutable buffer and its initial upload.
         * @param descriptor Backend-neutral buffer policy validated by the frontend.
         * @param initialData Owned request bytes borrowed synchronously for this call.
         * @param placement Previously admitted native-free placement for this exact request.
         * @return Non-zero backend-private instance identity, or a typed failure.
         */
        [[nodiscard]] virtual Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                                 std::span<const std::byte> initialData,
                                                                 const RenderMemoryPlacement &placement) = 0;

        /**
         * @brief Realizes one validated mesh over exact ready buffer instances.
         * @param descriptor Backend-neutral immutable mesh descriptor.
         * @param vertexBuffer Backend-private instance for descriptor.vertexBuffer.
         * @param indexBuffer Backend-private instance for descriptor.indexBuffer.
         * @return Non-zero backend-private mesh identity, or a typed failure.
         */
        [[nodiscard]] virtual Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                               std::uint64_t indexBuffer) = 0;

        /**
         * @brief Realizes one validated immutable texture allocation and optional initial upload.
         * @param descriptor Backend-neutral allocation policy validated by the frontend.
         * @param initialData Empty for undefined initial contents, or one tightly packed complete base level.
         * @param placement Previously admitted native-free placement for this exact request.
         * @return Non-zero backend-private texture identity, or a typed failure.
         */
        [[nodiscard]] virtual Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor,
                                                                  std::span<const std::byte> initialData,
                                                                  const RenderMemoryPlacement &placement) = 0;

        /**
         * @brief Realizes one validated view over an exact ready texture instance.
         * @param descriptor Backend-neutral view policy validated by the frontend.
         * @param texture Backend-private instance for descriptor.texture.
         * @return Non-zero backend-private view identity, or a typed failure.
         */
        [[nodiscard]] virtual Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor,
                                                                      std::uint64_t texture) = 0;

        /**
         * @brief Realizes one validated render target over exact ready attachment views.
         * @param descriptor Backend-neutral attachment policy validated by the frontend.
         * @param colorAttachment Backend-private color view, or zero when absent.
         * @param depthAttachment Backend-private depth view, or zero when absent.
         * @return Non-zero backend-private render-target identity, or a typed failure.
         */
        [[nodiscard]] virtual Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor,
                                                                       std::uint64_t colorAttachment, std::uint64_t depthAttachment) = 0;

        /** @brief Releases one buffer instance after frontend dependency and submission pins drain. */
        virtual void DestroyBuffer(std::uint64_t backendInstance) noexcept = 0;

        /** @brief Releases one mesh instance after frontend dependency and submission pins drain. */
        virtual void DestroyMesh(std::uint64_t backendInstance) noexcept = 0;

        /**
         * @brief Releases one texture instance after dependency and submission pins drain.
         * @param backendInstance Backend-private identity previously returned by CreateTexture.
         */
        virtual void DestroyTexture(std::uint64_t backendInstance) noexcept = 0;

        /**
         * @brief Releases one texture-view instance after dependency and submission pins drain.
         * @param backendInstance Backend-private identity previously returned by CreateTextureView.
         */
        virtual void DestroyTextureView(std::uint64_t backendInstance) noexcept = 0;

        /**
         * @brief Releases one render-target instance after dependency and submission pins drain.
         * @param backendInstance Backend-private identity previously returned by CreateRenderTarget.
         */
        virtual void DestroyRenderTarget(std::uint64_t backendInstance) noexcept = 0;
    };

    /**
     * @brief Coarse renderer backend interface implemented by engine-internal backend modules.
     *
     * Implementations own native device/context state. Calls are restricted to the
     * host-declared render-capable thread. Implementations must release remaining
     * resources safely from their destructor; explicit Shutdown remains the
     * deterministic lifecycle path and must be idempotent.
     */
    class IRenderBackend : public IRenderResourceBackend, public IMaterialBindingBackend {
    public:
        ~IRenderBackend() override = default;

        /**
         * @brief Realizes an exact cooked light-culling kernel at an owner-thread preparation boundary.
         * @param kernel Admitted artifact, stage, entry point and final-target binding evidence.
         * @return Native preparation lease or typed unsupported/original admission failure.
         * @details No source compilation, discovery or policy fallback. The host drains leases before teardown.
         */
        [[nodiscard]] virtual Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
            const CookedLightCullingKernel &kernel);

        /**
         * @brief Updates a reusable frame slot only after all prior native buffer use has completed.
         * @param update Exact resolved buffers and bounded synchronously borrowed packed frame.
         * @return Success, typed pending without mutation, unsupported, or original validation failure.
         * @details Owner-thread safe point only; no CPU wait, source compilation, allocation or implicit fallback.
         */
        [[nodiscard]] virtual Result<void> UpdateLightFrame(const NativeLightFrameUpdate &update);

        /** @brief Initializes the inert backend instance and acquires its runtime resources. */
        [[nodiscard]] virtual Result<void> Initialize(const RenderBackendConfig &config) = 0;

        /** @brief Returns the immutable capability snapshot for this backend instance. */
        [[nodiscard]] virtual const RenderBackendCapabilities &Capabilities() const noexcept = 0;

        /** @brief Starts one frame and returns the token required by later frame operations. */
        [[nodiscard]] virtual Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) = 0;

        /** @brief Validates and executes the ordered plan associated with the active frame. */
        [[nodiscard]] virtual Result<void> Execute(const RenderExecutionPlan &plan) = 0;

        /**
         * @brief Executes typed workloads through the intact compiled graph for the active frame.
         * @param request Borrowed graph, exact workloads and frontend-resolved resource instances.
         * @return Success or a typed unsupported/admission/encoding failure, without fallback.
         */
        [[nodiscard]] virtual Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request);

        /** @brief Returns explicit worker-native support; unsupported backends return zero bounds. */
        [[nodiscard]] virtual RenderParallelRecordingCapabilities ParallelRecordingCapabilities() const noexcept;
        /**
         * @brief Freezes and validates native graph payload on the render owner before scheduling.
         * @param request Synchronously borrowed exact graph, workloads, resolved generations and owner lease.
         * @return Owning worker-safe recording, or typed unsupported/admission failure without fallback.
         * @details Success transfers resource-lease release responsibility to this backend. Failure
         * leaves it with the caller. Cancelled live callbacks count against the finite capability bound.
         */
        [[nodiscard]] virtual Result<std::shared_ptr<IRenderParallelGraphRecording>> PrepareParallelGraph(
            const RenderGraphExecutionRequest &request);
        /**
         * @brief Accepts fully recorded commands on the render owner in canonical graph order.
         * @param recording Exact session returned for the still-active frame by this backend.
         * @return Success or typed stale, cancelled, incomplete or native failure; never waits.
         * @details Success prepares ordered submission at Present; CPU readiness is not GPU completion.
         */
        [[nodiscard]] virtual Result<void> AcceptParallelGraph(const std::shared_ptr<IRenderParallelGraphRecording> &recording);

        /** @brief Completes the active frame and presents when the backend supports presentation. */
        [[nodiscard]] virtual Result<void> Present(FrameToken frame) = 0;

        /** @brief Discards matching active-frame work after a failed execution or presentation step. */
        virtual void AbortFrame(FrameToken frame) noexcept = 0;

        /** @brief Discards partial active-frame state when frame creation did not return a token. */
        virtual void AbortActiveFrame() noexcept = 0;

        /** @brief Commits a non-zero output extent at a host-controlled frame boundary. */
        [[nodiscard]] virtual Result<void> Resize(FramebufferExtent extent) = 0;

        /** @brief Releases all runtime resources; repeated calls are safe. */
        virtual void Shutdown() noexcept = 0;
    };
}  // namespace Horo::Render
