#include "RenderFrontendTestSupport.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

namespace Horo::Tests::RenderFrontendTesting {
    void Check(const bool condition) {
        REQUIRE((condition));
    }

    BackendLifecycleState lifecycleState;

    class InjectedBackendException final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class TrackingBackend final : public IRenderBackend {
    public:
        TrackingBackend()
            : capabilities_{.backend = RenderBackendId{"tracking"},
                            .supportsBufferResources = lifecycleState.supportsBufferResources,
                            .supportsMeshResources = lifecycleState.supportsMeshResources,
                            .supportsTextureResources = lifecycleState.supportsTextureResources,
                            .supportsRenderTargetResources = lifecycleState.supportsRenderTargetResources} {}

        Result<void> Initialize(const RenderBackendConfig &) override {
            ++lifecycleState.initializeCount;
            initialized_ = true;
            const auto incarnation = AcquireRenderDeviceIncarnation();
            if (incarnation.HasError())
                return Result<void>::Failure(incarnation.ErrorValue());
            capabilities_.support.deviceIncarnation = incarnation.Value();
            capabilities_.supportsSubmissionObservation = lifecycleState.supportsSubmissionObservation;
            if (lifecycleState.throwDuringInitialize) {
                throw InjectedBackendException{"Injected initialization failure."};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            return Result<RenderMemoryCostPlan>::Success(TestSupport::DedicatedMemoryCost(descriptor.byteSize, 1));
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            const std::size_t bytes = RenderTextureBaseLevelByteSize(descriptor).value_or(0);
            return Result<RenderMemoryCostPlan>::Success(TestSupport::DedicatedMemoryCost(bytes, 2));
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, const std::span<const std::byte> initialData,
                                           const RenderMemoryPlacement &, RenderResourceNativeLifetime lifetime = {}) override {
            ++lifecycleState.createBufferCount;
            lifecycleState.lastBufferInitialData.assign(initialData.begin(), initialData.end());
            return CompleteResourceCreation();
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t,
                                         RenderResourceNativeLifetime lifetime = {}) override {
            ++lifecycleState.createMeshCount;
            return CompleteResourceCreation();
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, const std::span<const std::byte> initialData,
                                            const RenderMemoryPlacement &, RenderResourceNativeLifetime lifetime = {}) override {
            ++lifecycleState.createTextureCount;
            lifecycleState.lastTextureInitialData.assign(initialData.begin(), initialData.end());
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t,
                                                RenderResourceNativeLifetime lifetime = {}) override {
            ++lifecycleState.createTextureViewCount;
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t,
                                                 RenderResourceNativeLifetime lifetime = {}) override {
            ++lifecycleState.createRenderTargetCount;
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        void DestroyBuffer(std::uint64_t) noexcept override {
            ++lifecycleState.destroyBufferCount;
        }

        void DestroyMesh(std::uint64_t) noexcept override {
            ++lifecycleState.destroyMeshCount;
        }

        void DestroyTexture(std::uint64_t) noexcept override {
            ++lifecycleState.destroyTextureCount;
        }

        void DestroyTextureView(std::uint64_t) noexcept override {
            ++lifecycleState.destroyTextureViewCount;
        }

        void DestroyRenderTarget(std::uint64_t) noexcept override {
            ++lifecycleState.destroyRenderTargetCount;
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
            Check(!lifecycleState.frameActive);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Begin) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
                throw InjectedBackendException{"Injected begin failure."};
            }
            if (lifecycleState.failBeginAfterActivation) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
                return Result<FrameToken>::Failure({ErrorCode{"render.test.begin_failed"},
                                                    ErrorDomainId{"render.test"},
                                                    ErrorSeverity::Error,
                                                    "Injected begin failure.",
                                                    {}});
            }
            if (lifecycleState.returnInvalidFrameToken) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = {};
                return Result<FrameToken>::Success({});
            }
            lifecycleState.frameActive = true;
            lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
            return Result<FrameToken>::Success(lifecycleState.activeFrame);
        }

        Result<void> Execute(const RenderExecutionPlan &plan) override {
            ++lifecycleState.executeCount;
            Check(lifecycleState.frameActive);
            Check(plan.frame == lifecycleState.activeFrame);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Execute) {
                throw InjectedBackendException{"Injected execution failure."};
            }
            return Result<void>::Success();
        }

        Result<void> PrepareGuiComposition(const GuiCompositionRequest &request) override {
            ++lifecycleState.guiAdmissions;
            if (!lifecycleState.frameActive || request.frame != lifecycleState.activeFrame || lifecycleState.failGuiAdmission)
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            if (request.lease != nullptr) {
                if (request.transfer == nullptr || (lifecycleState.graphLease != nullptr && lifecycleState.graphLease != request.lease))
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                lifecycleState.graphLease = request.lease;
                request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
            }
            if (request.submission.has_value()) {
                lifecycleState.guiOrder = request.submission->order;
                lifecycleState.guiSignal = request.submission->signal;
            }
            return Result<void>::Success();
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
            if (!request.submissions.empty())
                return IRenderBackend::ExecuteGraph(request);
            lifecycleState.graphResources.assign(request.resources.begin(), request.resources.end());
            lifecycleState.graphLease = request.lease;
            return Execute({request.frame, {}});
        }

        Result<void> Present(const FrameToken frame) override {
            ++lifecycleState.presentCount;
            Check(lifecycleState.frameActive);
            Check(frame == lifecycleState.activeFrame);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Present) {
                throw InjectedBackendException{"Injected presentation exception."};
            }
            if (lifecycleState.failPresentation) {
                return Result<void>::Failure({ErrorCode{"render.test.present_failed"},
                                              ErrorDomainId{"render.test"},
                                              ErrorSeverity::Error,
                                              "Injected presentation failure.",
                                              {}});
            }
            if (lifecycleState.completeGraphOnPresent && lifecycleState.graphLease != nullptr) {
                lifecycleState.graphLease->Release();
                lifecycleState.graphLease = nullptr;
            }
            lifecycleState.frameActive = false;
            lifecycleState.activeFrame = {};
            return Result<void>::Success();
        }

        void AbortFrame(const FrameToken frame) noexcept override {
            if (lifecycleState.frameActive && frame == lifecycleState.activeFrame) {
                ++lifecycleState.abortCount;
                if (lifecycleState.graphLease != nullptr) {
                    lifecycleState.graphLease->Release();
                    lifecycleState.graphLease = nullptr;
                }
                lifecycleState.frameActive = false;
                lifecycleState.activeFrame = {};
            }
        }

        void AbortActiveFrame() noexcept override {
            if (lifecycleState.frameActive) {
                ++lifecycleState.abortCount;
                if (lifecycleState.graphLease != nullptr) {
                    lifecycleState.graphLease->Release();
                    lifecycleState.graphLease = nullptr;
                }
                lifecycleState.frameActive = false;
                lifecycleState.activeFrame = {};
            }
        }

        Result<void> Resize(FramebufferExtent) override {
            ++lifecycleState.resizeCount;
            if (lifecycleState.throwDuringResize) {
                throw InjectedBackendException{"Injected resize failure."};
            }
            if (lifecycleState.failResize) {
                return Result<void>::Failure({ErrorCode{"render.test.resize_failed"},
                                              ErrorDomainId{"render.test"},
                                              ErrorSeverity::Critical,
                                              "Injected typed resize failure.",
                                              {}});
            }
            return Result<void>::Success();
        }

        Result<bool> ObserveSubmission(const RenderCpuWait &wait) override {
            if (!capabilities_.supportsSubmissionObservation)
                return IRenderBackend::ObserveSubmission(wait);
            ++lifecycleState.submissionObservations;
            switch (lifecycleState.observationFailure) {
                case ObservationFailure::StandardException:
                    throw InjectedBackendException{"Injected observation failure."};
                case ObservationFailure::ForeignException:
                    throw ObservationFailure::ForeignException;
                case ObservationFailure::TypedError:
                    return Result<bool>::Failure(Error{ErrorCode{"render.test.observation"},
                                                       ErrorDomainId{"horo.render.test"},
                                                       ErrorSeverity::Error,
                                                       "Injected typed observation failure.",
                                                       {}});
                case ObservationFailure::None:
                    break;
            }
            return Result<bool>::Success(!lifecycleState.drainPending);
        }

        Result<void> BeginShutdown() override {
            return Result<void>::Success();
        }

        Result<RenderBackendDrainProgress> PollDrain() override {
            if (lifecycleState.drainPending)
                return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Pending);
            if (lifecycleState.graphLease != nullptr) {
                lifecycleState.graphLease->Release();
                lifecycleState.graphLease = nullptr;
            }
            if (initialized_) {
                ++lifecycleState.shutdownCount;
                initialized_ = false;
            }
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
        }

    private:
        [[nodiscard]] Result<std::uint64_t> CompleteResourceCreation() {
            if (lifecycleState.throwDuringResourceCreation) {
                throw InjectedBackendException{"Injected resource creation exception."};
            }
            if (lifecycleState.failResourceCreation) {
                return Result<std::uint64_t>::Failure({ErrorCode{"render.test.resource_failed"},
                                                       ErrorDomainId{"render.test"},
                                                       ErrorSeverity::Error,
                                                       "Injected resource creation failure.",
                                                       {}});
            }
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        RenderBackendCapabilities capabilities_;
        bool initialized_{false};
    };

    class TrackingBackendProvider final : public IRenderBackendProvider {
    public:
        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<TrackingBackend>());
        }
    };

    [[nodiscard]] std::unique_ptr<IRenderBackendProvider> MakeTrackingBackendProvider() {
        return std::make_unique<TrackingBackendProvider>();
    }

    void RegisterTrackingBackend(RenderBackendRegistry &registry) {
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());
    }

    static_assert(!std::is_copy_constructible_v<RenderFrameScope>);
    static_assert(!std::is_copy_assignable_v<RenderFrameScope>);
    static_assert(std::is_nothrow_move_constructible_v<RenderFrameScope>);
    static_assert(std::is_nothrow_move_assignable_v<RenderFrameScope>);
    static_assert(!std::is_same_v<RenderMeshHandle, RenderMeshSourceHandle>);

    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateTrackingFrontend(const RenderFrontendMemoryConfig &memoryConfig,
                                                                         const RenderResourceUploadLimits &uploadLimits) {
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);
        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{}, uploadLimits, memoryConfig);
        Check(created.HasValue());
        return std::move(created).Value();
    }

    void VerifyUploadRequestLimit(RenderFrontend &frontend) {
        const auto requestFull =
            frontend.CreateTexture({.extent = {1, 1}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled});
        REQUIRE(requestFull.HasError());
        CHECK(requestFull.ErrorValue().code.Value() == "render.frontend.resource.upload_capacity_exceeded");
    }

    void VerifyUploadCompaction(RenderFrontend &frontend, const RenderBufferHandle first, const std::span<const std::byte> secondBytes) {
        REQUIRE(frontend.ReleaseBuffer(first).HasValue());
        const RenderResourceUploadSnapshot compacted = frontend.UploadSnapshot();
        CHECK(compacted.pendingPayloadBytes == secondBytes.size());
        CHECK(compacted.occupiedStagingBytes == secondBytes.size());
        CHECK(compacted.pendingRequests == 1);
        CHECK(compacted.cancelledRequestCount == 1);

        const std::array<std::byte, 9> paddingOverflow{};
        const auto alignmentFull = frontend.CreateBuffer({.byteSize = paddingOverflow.size(),
                                                          .usage = RenderBufferUsage::Vertex,
                                                          .access = RenderBufferAccess::HostVisible},
                                                         paddingOverflow);
        REQUIRE(alignmentFull.HasError());
        CHECK(alignmentFull.ErrorValue().code.Value() == "render.frontend.resource.upload_capacity_exceeded");

        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.lastBufferInitialData == std::vector<std::byte>(secondBytes.begin(), secondBytes.end()));
        const RenderResourceUploadSnapshot completed = frontend.UploadSnapshot();
        CHECK(completed.pendingPayloadBytes == 0);
        CHECK(completed.occupiedStagingBytes == 0);
        CHECK(completed.completedBatchCount == 1);
        CHECK(completed.lastBatchPayloadBytes == secondBytes.size());
        CHECK(completed.lastBatchRequestCount == 1);
    }

    [[nodiscard]] RenderMeshDescriptor MeshDescriptor(const RenderBufferHandle vertexBuffer, const RenderBufferHandle indexBuffer) {
        return {.vertexBuffer = vertexBuffer,
                .indexBuffer = indexBuffer,
                .vertexStride = sizeof(float) * 3,
                .vertexCount = 3,
                .indexFormat = RenderIndexFormat::UInt32,
                .indexCount = 3,
                .topology = RenderPrimitiveTopology::Triangles,
                .localBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}};
    }

    [[nodiscard]] PendingTriangleBuffers QueueTriangleBuffers(RenderFrontend &frontend) {
        const std::array<float, 9> vertices{};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        auto vertex = frontend.CreateBuffer({.byteSize = sizeof(vertices),
                                             .usage = RenderBufferUsage::Vertex,
                                             .access = RenderBufferAccess::DeviceLocal},
                                            std::as_bytes(std::span{vertices}));
        auto index = frontend.CreateBuffer({.byteSize = sizeof(indices),
                                            .usage = RenderBufferUsage::Index,
                                            .access = RenderBufferAccess::DeviceLocal},
                                           std::as_bytes(std::span{indices}));
        Check(vertex.HasValue());
        Check(index.HasValue());
        return {.vertex = vertex.Value(), .index = index.Value()};
    }

    /** @brief Compiles an imported-resource graph to exercise frontend owner/generation resolution. */
    CompiledRenderGraphExecution CompileImportedBufferGraph(const RenderBufferHandle handle, RenderGraphPassRef &pass) {
        auto builder = Horo::Render::Test::RequireBuilder();
        pass = Horo::Render::Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto resource = Horo::Render::Test::RequireResource(builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent));
        auto graph = Horo::Render::Test::RequireGraph(builder);
        auto schedule = Horo::Render::Test::RequireSchedule(graph);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{resource,
                                                          {RenderGraphSynchronizationAccess::Read,
                                                           RenderGraphSynchronizationOperation::CopySource,
                                                           RenderGraphPipelineScope::Transfer,
                                                           RenderGraphTextureLayout::NotApplicable,
                                                           {1}}}};
        auto sync = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
        REQUIRE(sync.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, sync.Value(), queues);
        REQUIRE(execution.HasValue());
        return std::move(execution).Value();
    }
}  // namespace Horo::Tests::RenderFrontendTesting
