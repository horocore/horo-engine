#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "OpenGLBackendInternal.h"
#include "OpenGLExecutionAdapter.h"
#include "OpenGLGraphWorkloads.h"
#include "OpenGLPreparedMaterialState.h"
#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <glad/gl.h>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] std::optional<std::size_t> ConservativeRequirement(const std::size_t payload) noexcept {
            constexpr std::size_t alignment = 256;
            constexpr std::size_t mask = alignment - 1;
            if (payload > std::numeric_limits<std::size_t>::max() - mask)
                return std::nullopt;
            return (payload + mask) & ~mask;
        }

        [[nodiscard]] bool MatchesPlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes == 0;
        }

        [[nodiscard]] RenderMemoryCostPlan MemoryCostPlan(const std::uint64_t compatibility, const std::size_t payload,
                                                          const std::size_t required) noexcept {
            return {.memoryClass = RenderMemoryClass::PersistentDevice,
                    .allocationClass = RenderMemoryAllocationClass::Dedicated,
                    .provenance = RenderMemoryCostProvenance::Estimated,
                    .compatibility = RenderMemoryCompatibilityId{compatibility},
                    .payloadBytes = payload,
                    .requiredBytes = required,
                    .alignment = 256};
        }

        /** @brief Serializes ownership of the single context retained by one presentation port. */
        struct OpenGLContextLease {
            bool claimed{false};
        };

        struct OpenGLTextureFormat {
            std::int32_t internal;
            std::uint32_t external;
            std::uint32_t type;
        };

        /** @brief Backend-private metadata for one logical view over a native texture. */
        struct OpenGLTextureView {
            std::uint32_t texture{0};
            FramebufferExtent extent;
            RenderTextureFormat format{RenderTextureFormat::Rgba8Unorm};
            RenderTextureAspect aspect{RenderTextureAspect::Color};
            RenderTextureUsage usage{RenderTextureUsage::None};
            std::uint32_t sampleCount{1};
            std::uint32_t references{1};
        };

        [[nodiscard]] OpenGLTextureFormat TextureFormat(const RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            switch (format) {
                case Rgba8Unorm:
                    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
                case Depth24Stencil8:
                    return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
                case Depth32Float:
                    return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
                case R8Unorm:
                case Rg8Unorm:
                case Rgba8UnormSrgb:
                case Bgra8Unorm:
                case Bgra8UnormSrgb:
                case R16Float:
                case Rg16Float:
                case Rgba16Float:
                case R32Float:
                case Rg32Float:
                case Rgba32Float:
                case Depth16Unorm:
                case Depth32FloatStencil8:
                    return {};
            }
            return {};
        }

        [[nodiscard]] bool IsSupportedTextureDescriptor(const RenderTextureDescriptor &descriptor) noexcept {
            using enum RenderTextureUsage;
            constexpr std::byte supportedUsage =
                std::byte{static_cast<std::uint8_t>(Sampled)} | std::byte{static_cast<std::uint8_t>(RenderAttachment)};
            const auto requestedUsage = static_cast<std::byte>(descriptor.usage);
            return descriptor.IsValid() && descriptor.dimension == RenderTextureDimension::TwoD && descriptor.depth == 1 &&
                   descriptor.mipCount == 1 && descriptor.layerCount == 1 && descriptor.sampleCount == 1 &&
                   TextureFormat(descriptor.format).internal != 0 && (requestedUsage & ~supportedUsage) == std::byte{};
        }

        [[nodiscard]] bool AspectMatches(const RenderTextureFormat format, const RenderTextureAspect aspect) noexcept {
            using enum RenderTextureAspect;
            using enum RenderTextureFormat;
            if (format == Rgba8Unorm)
                return aspect == Color;
            if (format == Depth24Stencil8)
                return aspect == Depth || aspect == DepthStencil;
            return format == Depth32Float && aspect == Depth;
        }

        [[nodiscard]] bool IsCompatibleAttachment(const OpenGLTextureView &view, const RenderTargetDescriptor &target,
                                                  const bool colorView) noexcept {
            const bool aspectMatches = colorView ? view.aspect == RenderTextureAspect::Color : view.aspect != RenderTextureAspect::Color;
            return aspectMatches && view.extent == target.extent && view.sampleCount == target.sampleCount &&
                   HasTextureUsage(view.usage, RenderTextureUsage::RenderAttachment);
        }

        /** @brief OpenGL backend owning one presentation-port context lifecycle. */
        class OpenGLRenderBackend final : public IRenderBackend {  // NOSONAR(cpp:S1448)
        public:
            OpenGLRenderBackend(IOpenGLPresentationPort &presentationPort, const OpenGLBackendOptions options,
                                const Detail::OpenGLCommandFunctions &functions, std::shared_ptr<OpenGLContextLease> contextLease) noexcept
                : presentationPort_(&presentationPort), options_(options), functions_(functions), contextLease_(std::move(contextLease)),
                  execution_(functions_) {}

            /** @brief Releases a remaining OpenGL context as a lifecycle fallback. */
            ~OpenGLRenderBackend() override {
                try {
                    const auto begun = BeginShutdown();
                    if (const auto drained = PollDrain();
                        begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                        std::terminate();
                    DestroyContext();
                    ownerThread_ = {};

                } catch (...) {
                    // Destruction cannot release native owners without proven terminal retirement.
                    std::terminate();
                }
            }

            OpenGLRenderBackend(const OpenGLRenderBackend &) = delete;
            OpenGLRenderBackend &operator=(const OpenGLRenderBackend &) = delete;
            OpenGLRenderBackend(OpenGLRenderBackend &&) = delete;
            OpenGLRenderBackend &operator=(OpenGLRenderBackend &&) = delete;

            /** @copydoc IRenderBackend::Initialize */
            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (initialized_ || (closing_ && contextCreated_)) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::AlreadyInitialized, "Renderer backend is already initialized."));
                }
                if (!functions_.IsValid() || options_.majorVersion == 0 || !config.IsValid()) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL backend configuration is invalid."));
                }
                if (!config.HasSingleEffectiveQueue())
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                           "OpenGL does not implement distinct effective graph queues."));
                if (config.presentationOutput)
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));

                if (contextLease_->claimed) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::PresentationInUse,
                                                           "OpenGL presentation attachment is already owned by another backend."));
                }
                if (config.adapter)
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                           "This OpenGL presentation port cannot realize an explicit adapter selection; no "
                                                           "default device substitution occurs."));
                contextLease_->claimed = true;
                closing_ = false;
                drained_ = false;
                ownsContextLease_ = true;
                ownerThread_ = std::this_thread::get_id();
                if (Result<void> attempt = InitializeAttempt(config, config.enableValidation); attempt.HasError()) {
                    if (!config.enableValidation) {
                        ReleaseContextLease();
                        ownerThread_ = {};
                        return attempt;
                    }

                    Error debugFailure = attempt.ErrorValue();
                    if (Result<void> retry = InitializeAttempt(config, false); retry.HasError()) {
                        Error retryFailure = retry.ErrorValue();
                        ReleaseContextLease();
                        ownerThread_ = {};
                        return Result<void>::Failure(WithCause(std::move(retryFailure), std::move(debugFailure)));
                    }
                }
                const auto incarnation = AcquireRenderDeviceIncarnation();
                if (incarnation.HasError())
                    return Result<void>::Failure(incarnation.ErrorValue());
                capabilities_.support.deviceIncarnation = incarnation.Value();
                frameSlotCount_ = config.maxFramesInFlight;
                initialized_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

#include "OpenGLRenderBackendResources.inl"

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!IsOwnerThread())
                    return WrongThread<FrameToken>();
                if (!initialized_) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (frameActive_) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::FrameAlreadyActive, "A renderer frame is already active."));
                }
                if (constexpr auto maxViewportExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
                    descriptor.frameNumber == 0 || !descriptor.outputExtent.IsValid() ||
                    descriptor.outputExtent.width > maxViewportExtent || descriptor.outputExtent.height > maxViewportExtent) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidFrameDescriptor, "Frame number and output extent must be valid."));
                }

                if (nextFrameToken_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::FrameTokenExhausted, "Frame token space is exhausted."));
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError()) {
                    return Result<FrameToken>::Failure(current.ErrorValue());
                }

                if (auto retired = PollNativeLifetimes(); retired.HasError())
                    return Result<FrameToken>::Failure(std::move(retired).ErrorValue());
                const Result<std::size_t> slot = execution_.AdmitFrameSlot(frameSlotCount_);
                if (slot.HasError())
                    return Result<FrameToken>::Failure(slot.ErrorValue());
                activeFrameSlot_ = slot.Value();
                activeExtent_ = descriptor.outputExtent;
                frameActive_ = true;
                activeFrame_ = FrameToken{nextFrameToken_++};
                return Result<FrameToken>::Success(activeFrame_);
            }

            /** @copydoc IRenderBackend::Execute */
            Result<void> Execute(const RenderExecutionPlan &plan) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (const Result<void> valid = ValidatePlan(plan); valid.HasError()) {
                    return valid;
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError())
                    return current;
                return execution_.Execute(plan, activeExtent_);
            }

#include "OpenGLRenderBackendGraph.inl"
#include "OpenGLRenderBackendMaterial.inl"

            /** @copydoc IRenderBackend::Present */
            Result<void> Present(const FrameToken frame) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (const Result<void> state = ValidateActiveFrame(frame); state.HasError()) {
                    return state;
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError())
                    return current;
                if (!execution_.FenceFrame(activeFrameSlot_))
                    return SynchronizationFailure<void>();
                if (const Result<void> presented = presentationPort_->SwapBuffers(); presented.HasError()) {
                    return Result<void>::Failure(presented.ErrorValue());
                }

                CompleteActiveFrame();
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::AbortFrame */
            void AbortFrame(const FrameToken frame) noexcept override {
                if (!IsOwnerThread())
                    return;
                if (frameActive_ && frame == activeFrame_) {
                    AbortActiveFrame();
                }
            }

            /** @copydoc IRenderBackend::AbortActiveFrame */
            void AbortActiveFrame() noexcept override {
                if (!IsOwnerThread())
                    return;
                if (frameActive_)
                    static_cast<void>(execution_.FenceFrame(activeFrameSlot_));
                CompleteActiveFrame();
            }

            /** @copydoc IRenderBackend::PrepareGuiComposition */
            Result<void> PrepareGuiComposition(const GuiCompositionRequest &request) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (const auto frame = ValidateActiveFrame(request.frame); frame.HasError())
                    return frame;
                if (request.mode != GuiCompositionMode::BorrowedActiveRecording || request.submission.has_value() ||
                    !request.buffers.empty() ||
                    std::ranges::any_of(request.resources,
                                        [](const GuiSampledResourceInstance &sample) {
                    return sample.uploadSource != 0;
                }) ||
                    request.resources.size() > GuiCompositionPreparation::MaximumResources ||
                    (!request.resources.empty() && request.lease == nullptr))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
                if (request.lease != nullptr) {
                    if (request.transfer == nullptr || !execution_.RetainFrameLease(activeFrameSlot_, *request.lease))
                        return SynchronizationFailure<void>();
                    request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
                }
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(const FramebufferExtent extent) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (!initialized_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!extent.IsValid() || !std::in_range<std::int32_t>(extent.width) || !std::in_range<std::int32_t>(extent.height)) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidExtent, "Expected nonzero int32 viewport extent."));
                }
                if (frameActive_) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::FrameActive, "Renderer output cannot resize while a frame is active."));
                }

                // A validated host extent is not proof that the attached native context is still usable.
                return presentationPort_->MakeCurrent();
            }

            /** @copydoc IRenderBackend::BeginShutdown */
            Result<void> BeginShutdown() override {
                if (ownerThread_ != std::thread::id{} && !IsOwnerThread())
                    return WrongThread<void>();
                if (drained_)
                    return Result<void>::Success();
                closing_ = true;
                initialized_ = false;
                if (!contextCreated_)
                    return Result<void>::Success();
                if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
                    return current;
                AbortActiveFrame();
                CompleteActiveFrame();
                if (functions_.sync.IsValid() && closingFence_ == 0) {
                    closingFence_ = functions_.sync.fence();
                    if (closingFence_ == 0)
                        return SynchronizationFailure<void>();
                    functions_.sync.flush();
                }
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::PollDrain */
            Result<RenderBackendDrainProgress> PollDrain() override {
                using DrainResult = Result<RenderBackendDrainProgress>;
                if (ownerThread_ != std::thread::id{} && !IsOwnerThread())
                    return WrongThread<RenderBackendDrainProgress>();
                if (drained_)
                    return DrainResult::Success(RenderBackendDrainProgress::Drained);
                if (!closing_)
                    return DrainResult::Failure(MakeError(OpenGLBackendErrors::InvalidExecutionPlan,
                                                          "BeginShutdown must close the OpenGL owner before drain polling."));
                if (contextCreated_) {
                    if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
                        return DrainResult::Failure(current.ErrorValue());
                    if (functions_.sync.IsValid()) {
                        if (closingFence_ == 0)
                            return DrainResult::Success(RenderBackendDrainProgress::Pending);
                        const auto status = functions_.sync.poll(closingFence_);
                        if (status == GL_TIMEOUT_EXPIRED)
                            return DrainResult::Success(RenderBackendDrainProgress::Pending);
                        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
                            return SynchronizationFailure<RenderBackendDrainProgress>();
                        functions_.sync.destroy(std::exchange(closingFence_, 0));
                    }
                }
                execution_.Reset();
                prepared_.DestroyPrepared(functions_.shaders, functions_.samplers);
                DestroyRemainingResources();
                ReleaseDrainedNativeLifetimes();
                // Host GUI/viewport teardown still needs this current context after renderer work drains.
                // Context/port attachment retire only with the final backend owner.
                drained_ = true;
                return DrainResult::Success(RenderBackendDrainProgress::Drained);
            }

        private:
            enum class NativeResourceKind : std::uint8_t {
                Buffer,
                Texture,
                Mesh,
                View,
                Target
            };
            using NativeLifetimeKey = std::pair<NativeResourceKind, std::uint64_t>;

            struct NativeLifetimeRecord {
                RenderResourceNativeLifetime lifetime;
                std::uintptr_t fence{};
                bool retiring{false};
            };

            static constexpr std::size_t MaximumNativeLifetimes = 65'535;
            std::map<NativeLifetimeKey, NativeLifetimeRecord> nativeLifetimes_;
            NativeLifetimeKey nativeLifetimeCursor_{};

            /** @brief Allocates tracking before any upload command can be accepted; lifetime remains charged on native failure. */
            [[nodiscard]] Result<void> TrackNativeLifetime(const NativeResourceKind kind, const std::uint64_t identity,
                                                           RenderResourceNativeLifetime lifetime) {
                if (!lifetime.HasLease())
                    return Result<void>::Success();
                if (!functions_.sync.IsValid())
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                  "Charged native allocations require real nonblocking retirement-fence dispatch."));
                if (auto polled = PollNativeLifetimes(); polled.HasError())
                    return polled;
                if (nativeLifetimes_.size() >= MaximumNativeLifetimes || nativeLifetimes_.contains({kind, identity}))
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::ResourceCreationFailed,
                                  "Native lifetime storage or a recycled native identity remains owned by pending retirement."));
                try {
                    nativeLifetimes_.try_emplace({kind, identity}, std::move(lifetime));
                } catch (const std::bad_alloc &) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed,
                                                           "Native lifetime tracking allocation failed before upload."));
                }
                return Result<void>::Success();
            }

            /** @brief Marks deletion after all native references are dropped; pending/failed fences retain the exact charge. */
            void RetireNativeLifetime(const NativeResourceKind kind, const std::uint64_t identity) noexcept {
                const auto found = nativeLifetimes_.find({kind, identity});
                if (found == nativeLifetimes_.end() || found->second.retiring)
                    return;
                found->second.retiring = true;
                found->second.fence = functions_.sync.fence();
                functions_.sync.flush();
            }

            /** @brief Polls a bounded owner-thread retirement batch without CPU waiting or callback mutation. */
            [[nodiscard]] Result<void> PollNativeLifetimes() {
                if (nativeLifetimes_.empty())
                    return Result<void>::Success();
                auto current = nativeLifetimes_.upper_bound(nativeLifetimeCursor_);
                if (current == nativeLifetimes_.end())
                    current = nativeLifetimes_.begin();
                const auto count = std::min(std::size_t{64}, nativeLifetimes_.size());
                for (std::size_t examined = 0; examined < count && !nativeLifetimes_.empty(); ++examined) {
                    nativeLifetimeCursor_ = current->first;
                    const auto &record = current->second;
                    bool complete = false;
                    if (record.retiring) {
                        if (record.fence == 0)
                            return SynchronizationFailure<void>();
                        const auto status = functions_.sync.poll(record.fence);
                        if (status != GL_TIMEOUT_EXPIRED && status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
                            return SynchronizationFailure<void>();
                        complete = status != GL_TIMEOUT_EXPIRED;
                    }
                    if (complete) {
                        functions_.sync.destroy(record.fence);
                        current = nativeLifetimes_.erase(current);
                    } else
                        ++current;
                    if (current == nativeLifetimes_.end())
                        current = nativeLifetimes_.begin();
                }
                return Result<void>::Success();
            }

            /** @pre The final closing fence has proved all previously accepted GPU work terminal. */
            void ReleaseDrainedNativeLifetimes() noexcept {
                for (const auto &[key, record] : nativeLifetimes_) {
                    (void)key;
                    if (record.fence != 0)
                        functions_.sync.destroy(record.fence);
                }
                nativeLifetimes_.clear();
                nativeLifetimeCursor_ = {};
            }

            void CompleteActiveFrame() noexcept {
                frameActive_ = false;
                activeFrame_ = {};
                activeExtent_ = {};
            }

            template <typename T> [[nodiscard]] Result<T> SynchronizationFailure() const {
                return Result<T>::Failure(MakeError(OpenGLBackendErrors::SynchronizationFailed,
                                                    "OpenGL could not establish or poll a frame completion fence; shut down the backend."));
            }

            [[nodiscard]] Result<void> InitializeAttempt(const RenderBackendConfig &config, const bool enableDebugContext) {
                // Assume ownership before crossing the platform boundary. A typed failure is
                // contractually non-retaining; an exception may occur after native creation,
                // so the frontend's rollback must still call DestroyContext().
                contextCreated_ = true;
                if (const Result<void> created = presentationPort_->CreateContext(OpenGLContextDescriptor{
                        .majorVersion = options_.majorVersion,
                        .minorVersion = options_.minorVersion,
                        .profile = OpenGLContextProfile::Core,
                        .enableDebugContext = enableDebugContext,
                    });
                    created.HasError()) {
                    contextCreated_ = false;
                    return Result<void>::Failure(created.ErrorValue());
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError())
                    return RollbackAttempt(current.ErrorValue());
                if (const Result<void> loaded = presentationPort_->LoadCommandDispatch(); loaded.HasError())
                    return RollbackAttempt(loaded.ErrorValue());
                if (!functions_.isAvailable())
                    return RollbackAttempt(
                        MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints,
                                  "Required OpenGL state, command or synchronization entry points are absent after dispatch loading."));
                const Result<OpenGLContextFacts> facts = presentationPort_->QueryContextFacts();
                if (facts.HasError())
                    return RollbackAttempt(facts.ErrorValue());
                if (const Result<void> admitted = Detail::ValidateOpenGLContextFacts(facts.Value(), options_); admitted.HasError())
                    return RollbackAttempt(admitted.ErrorValue());
                if (const Result<void> presentMode = presentationPort_->SetPresentMode(config.presentMode); presentMode.HasError())
                    return RollbackAttempt(presentMode.ErrorValue());

                contextFacts_ = facts.Value();
                const bool resourcesAvailable = functions_.HasResourceFunctions();
                capabilities_.support = Detail::MakeOpenGLCapabilitySnapshot(contextFacts_, resourcesAvailable);
                capabilities_.supportsOffscreenTargets = resourcesAvailable && contextFacts_.maxColorAttachments > 0;
                capabilities_.supportsBufferResources = resourcesAvailable;
                capabilities_.supportsExactTransientResourceReuse = resourcesAvailable && functions_.isGraphAvailable != nullptr &&
                                                                    functions_.isGraphAvailable() &&
                                                                    functions_.buffers.copyBufferSubData != nullptr;
                capabilities_.support.queues.copy = capabilities_.supportsExactTransientResourceReuse;
                capabilities_.supportsMeshResources = resourcesAvailable && contextFacts_.maxVertexAttributes > 0;
                capabilities_.supportsTextureResources = resourcesAvailable && contextFacts_.maxTexture2DSize > 0;
                capabilities_.supportsRenderTargetResources = resourcesAvailable && contextFacts_.maxColorAttachments > 0;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> RollbackAttempt(Error error) {
                DestroyNativeContext();
                return Result<void>::Failure(std::move(error));
            }

            template <typename T> [[nodiscard]] Result<T> WrongThread() const {
                return Result<T>::Failure(MakeError(OpenGLBackendErrors::WrongThread,
                                                    "OpenGL operation must execute on the thread that initialized the context."));
            }

            [[nodiscard]] bool IsOwnerThread() const noexcept {
                return ownerThread_ == std::thread::id{} || ownerThread_ == std::this_thread::get_id();
            }

            [[nodiscard]] Result<std::uint64_t> ResourceUnavailable(std::string message) const {
                return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation, std::move(message)));
            }

            [[nodiscard]] std::optional<Result<std::uint64_t>> ResourceCreationFailure() const {
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL resource creation is unavailable in the current backend state.");
                return std::nullopt;
            }

            template <typename Destroy> static void DeleteObject(const Destroy destroy, const std::uint64_t backendInstance) noexcept {
                if (destroy != nullptr && backendInstance != 0 && backendInstance <= std::numeric_limits<std::uint32_t>::max()) {
                    const auto object = static_cast<std::uint32_t>(backendInstance);
                    destroy(1, &object);
                }
            }

            template <typename Destroy>
            static void DeleteTrackedObject(const Destroy destroy, std::unordered_set<std::uint32_t> &objects,
                                            const std::uint64_t backendInstance) noexcept {
                if (backendInstance <= std::numeric_limits<std::uint32_t>::max() &&
                    objects.erase(static_cast<std::uint32_t>(backendInstance)) > 0)
                    DeleteObject(destroy, backendInstance);
            }

            void TryDestroyPendingTexture(const std::uint32_t texture) noexcept {
                const auto tracked = textureDescriptors_.find(texture);
                if (tracked == textureDescriptors_.end() || !tracked->second.destroyRequested)
                    return;
                if (const bool hasViews = std::ranges::any_of(textureViews_,
                                                              [texture](const auto &entry) {
                    return entry.second.texture == texture;
                });
                    hasViews)
                    return;
                if (tracked->second.graphFramebuffer != 0)
                    functions_.framebuffers.deleteFramebuffers(1, &tracked->second.graphFramebuffer);
                textureDescriptors_.erase(tracked);
                DeleteTrackedObject(functions_.textures.deleteTextures, textures_, texture);
                RetireNativeLifetime(NativeResourceKind::Texture, texture);
            }

            void DestroyRemainingResources() noexcept {
                const auto destroyAll = [](const auto destroy, std::unordered_set<std::uint32_t> &objects) {
                    for (const std::uint32_t object : objects)
                        destroy(1, &object);
                    objects.clear();
                };
                if (functions_.HasResourceFunctions()) {
                    for (const auto &[texture, descriptor] : textureDescriptors_) {
                        if (descriptor.graphFramebuffer != 0)
                            functions_.framebuffers.deleteFramebuffers(1, &descriptor.graphFramebuffer);
                    }
                    destroyAll(functions_.framebuffers.deleteFramebuffers, renderTargets_);
                    destroyAll(functions_.vertexArrays.deleteVertexArrays, meshes_);
                    destroyAll(functions_.textures.deleteTextures, textures_);
                    destroyAll(functions_.buffers.deleteBuffers, buffers_);
                }
                textureViews_.clear();
                textureDescriptors_.clear();
                bufferDescriptors_.clear();
                prepared_.meshDescriptors.clear();
                prepared_.materialTargets.clear();
            }

            [[nodiscard]] Result<void> ValidateActiveFrame(const FrameToken frame) const {
                if (!initialized_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::FrameTokenMismatch, "Frame token does not match the active frame."));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidatePlan(const RenderExecutionPlan &plan) const {
                if (const Result<void> state = ValidateActiveFrame(plan.frame); state.HasError()) {
                    return state;
                }

                return Detail::ValidateOpenGLExecutionPlan(plan);
            }

            void DestroyContext() noexcept {
                DestroyNativeContext();
                ReleaseContextLease();
            }

            void DestroyNativeContext() noexcept {
                if (contextCreated_)
                    presentationPort_->DestroyContext();
                contextCreated_ = false;
                contextFacts_ = {};
                capabilities_.supportsOffscreenTargets = false;
                capabilities_.supportsBufferResources = false;
                capabilities_.supportsExactTransientResourceReuse = false;
                capabilities_.supportsMeshResources = false;
                capabilities_.supportsTextureResources = false;
                capabilities_.supportsRenderTargetResources = false;
                capabilities_.support = {};
            }

            void ReleaseContextLease() noexcept {
                if (ownsContextLease_) {
                    contextLease_->claimed = false;
                    ownsContextLease_ = false;
                }
            }

            IOpenGLPresentationPort *presentationPort_{nullptr};
            OpenGLBackendOptions options_{};
            Detail::OpenGLCommandFunctions functions_{};
            std::shared_ptr<OpenGLContextLease> contextLease_;
            std::unordered_map<std::uint32_t, RenderBufferDescriptor> bufferDescriptors_;
            std::unordered_map<std::uint32_t, Detail::OpenGLGraphTexture> textureDescriptors_;
            std::unordered_map<std::uint64_t, OpenGLTextureView> textureViews_;
            Detail::OpenGLPreparedMaterialState prepared_;
            std::uint32_t nextTextureViewIdentity_{1};
            std::unordered_set<std::uint32_t> buffers_;
            std::unordered_set<std::uint32_t> meshes_;
            std::unordered_set<std::uint32_t> textures_;
            std::unordered_set<std::uint32_t> renderTargets_;
            RenderBackendCapabilities capabilities_{
                .backend = RenderBackendId{"opengl"},
                .presentsToWindow = true,
                .supportsOffscreenTargets = false,
                .supportsTimestampQueries = false,
                .supportsCompute = false,
                .supportsBindlessResources = false,
                .supportsRayTracing = false,
                .support = {},
            };
            OpenGLContextFacts contextFacts_{};
            std::thread::id ownerThread_{};
            Detail::OpenGLExecutionAdapter execution_;
            std::size_t frameSlotCount_{1};
            std::size_t activeFrameSlot_{0};
            FramebufferExtent activeExtent_{};
            FrameToken activeFrame_{};
            std::uint64_t nextFrameToken_{1};
            bool initialized_{false};
            bool contextCreated_{false};
            bool ownsContextLease_{false};
            bool frameActive_{false};
            bool closing_{false};
            bool drained_{false};
            std::uintptr_t closingFence_{};
        };

        /** @brief Provides inert OpenGL backend instances bound to one borrowed presentation port. */
        class OpenGLBackendProvider final : public IRenderBackendProvider {
        public:
            OpenGLBackendProvider(IOpenGLPresentationPort &presentationPort, const OpenGLBackendOptions options,
                                  const Detail::OpenGLCommandFunctions &functions)
                : presentationPort_(&presentationPort), options_(options), functions_(functions) {}

            /** @copydoc IRenderBackendProvider::Create */
            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                return Result<std::unique_ptr<IRenderBackend>>::Success(
                    std::make_unique<OpenGLRenderBackend>(*presentationPort_, options_, functions_, contextLease_));
            }

        private:
            IOpenGLPresentationPort *presentationPort_{nullptr};
            OpenGLBackendOptions options_{};
            Detail::OpenGLCommandFunctions functions_{};
            std::shared_ptr<OpenGLContextLease> contextLease_{std::make_shared<OpenGLContextLease>()};
        };
    }  // namespace

    namespace Detail {
        Result<void> RegisterOpenGLRenderBackendWithFunctions(RenderBackendRegistry &registry, IOpenGLPresentationPort &presentationPort,
                                                              const OpenGLBackendOptions options, const OpenGLCommandFunctions &functions) {
            if (!functions.IsValid() || options.majorVersion < 4 || (options.majorVersion == 4 && options.minorVersion < 1)) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::InvalidRegistration, "OpenGL backend registration options are invalid."));
            }
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"opengl"},
                .displayName = "OpenGL",
                .provider = std::make_unique<OpenGLBackendProvider>(presentationPort, options, functions),
            });
        }
    }  // namespace Detail

}  // namespace Horo::Render
