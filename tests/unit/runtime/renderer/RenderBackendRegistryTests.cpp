#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "renderer/RenderBackendContractSuite.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    [[nodiscard]] RenderMemoryPlacement TestPlacement(const std::size_t bytes, const std::uint64_t compatibility = 1,
                                                      const std::uint64_t attempt = 1) {
        return {.pool = {{1}, compatibility},
                .scope = {1, 1},
                .attempt = {attempt},
                .compatibility = {compatibility},
                .budgetRevision = 1,
                .payloadBytes = bytes,
                .requiredBytes = bytes,
                .backingBytes = bytes,
                .allocationClass = RenderMemoryAllocationClass::Dedicated};
    }

    enum class ProviderBehavior {
        ReturnNull,
        ReturnFailure,
        Throw,
    };

    struct ProviderProbe {
        int createCount{0};
        int destroyCount{0};
    };

    class TestBackendProvider final : public IRenderBackendProvider {
    public:
        explicit TestBackendProvider(const ProviderBehavior behavior, ProviderProbe *probe = nullptr) noexcept
            : behavior_(behavior), probe_(probe) {}

        ~TestBackendProvider() override {
            if (probe_ != nullptr) {
                ++probe_->destroyCount;
            }
        }

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            if (probe_ != nullptr) {
                ++probe_->createCount;
            }
            switch (behavior_) {
                case ProviderBehavior::ReturnNull:
                    return Result<std::unique_ptr<IRenderBackend>>::Success(nullptr);
                case ProviderBehavior::ReturnFailure:
                    return Result<std::unique_ptr<IRenderBackend>>::Failure(Error{
                        .code = ErrorCode{"render.test.provider_failure"},
                        .domain = ErrorDomainId{"horo.render.test"},
                        .severity = ErrorSeverity::Critical,
                        .message = "Expected provider failure",
                        .diagnostics = {{
                            .code = DiagnosticCode{"render.test.provider_diagnostic"},
                            .severity = DiagnosticSeverity::Warning,
                            .message = "Expected provider diagnostic",
                            .location = {.source = "provider", .line = 7, .column = 3},
                        }},
                    });
                case ProviderBehavior::Throw:
                    throw std::runtime_error("Expected provider exception");
            }
            std::abort();
        }

    private:
        ProviderBehavior behavior_;
        ProviderProbe *probe_;
    };

    [[nodiscard]] std::unique_ptr<IRenderBackendProvider> MakeTestProvider(const ProviderBehavior behavior,
                                                                           ProviderProbe *probe = nullptr) {
        return std::make_unique<TestBackendProvider>(behavior, probe);
    }

    [[nodiscard]] std::unique_ptr<IRenderBackend> CreateNullBackend() {
        RenderBackendRegistry registry;
        Check(RegisterNullRenderBackend(registry).HasValue());
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"null"});
        Check(created.HasValue());
        return std::move(created).Value();
    }

    TEST_CASE("Registry Owns Provider And Defers Invocation Until Create", "[unit][runtime][renderer]") {
        ProviderProbe probe;
        {
            RenderBackendRegistry registry;
            Check(registry
                      .Register(RenderBackendDescriptor{
                          .id = RenderBackendId{"probed"},
                          .displayName = "Probed",
                          .provider = MakeTestProvider(ProviderBehavior::ReturnNull, &probe),
                      })
                      .HasValue());
            Check(probe.createCount == 0);
            Check(probe.destroyCount == 0);
            Check(registry.Seal().HasValue());
            Check(probe.createCount == 0);

            const auto created = registry.Create(RenderBackendId{"probed"});
            Check(created.HasError());
            Check(probe.createCount == 1);
            Check(probe.destroyCount == 0);
        }
        Check(probe.createCount == 1);
        Check(probe.destroyCount == 1);
    }

    TEST_CASE("Registry Destroys A Provider Rejected During Registration", "[unit][runtime][renderer]") {
        ProviderProbe probe;
        RenderBackendRegistry registry;
        const Result<void> rejected = registry.Register(RenderBackendDescriptor{
            .id = RenderBackendId{"Invalid"},
            .displayName = "Rejected",
            .provider = MakeTestProvider(ProviderBehavior::ReturnNull, &probe),
        });
        Check(rejected.HasError());
        Check(probe.createCount == 0);
        Check(probe.destroyCount == 1);
    }

    TEST_CASE("Registry Rejects Invalid And Duplicate Descriptors", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;

        const Result<void> invalid =
            registry.Register(RenderBackendDescriptor{.id = RenderBackendId{"broken"}, .displayName = "Broken", .provider = nullptr});
        Check(invalid.HasError());
        Check(invalid.ErrorValue().code.Value() == "render.registry.invalid_descriptor");

        const std::array<std::string, 6> invalidIds{
            "", "Invalid", "invalid backend", "1invalid", "invalid-", std::string(65, 'a'),
        };
        for (const std::string &id : invalidIds) {
            Check(registry
                      .Register(RenderBackendDescriptor{
                          .id = RenderBackendId{id},
                          .displayName = "Invalid ID",
                          .provider = MakeTestProvider(ProviderBehavior::ReturnNull),
                      })
                      .HasError());
        }

        Check(RegisterNullRenderBackend(registry).HasValue());
        const Result<void> duplicate = RegisterNullRenderBackend(registry);
        Check(duplicate.HasError());
        Check(duplicate.ErrorValue().code.Value() == "render.registry.duplicate_backend");
    }

    TEST_CASE("Registry Seals Deterministically And Rejects Late Mutation", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(RegisterNullRenderBackend(registry).HasValue());
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"secondary"},
                      .displayName = "Secondary",
                      .provider = MakeTestProvider(ProviderBehavior::ReturnNull),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());
        Check(registry.Seal().HasValue());

        const auto descriptors = registry.Descriptors();
        Check(descriptors.size() == 2);
        Check(descriptors.front().id == RenderBackendId{"null"});
        Check(descriptors.front().displayName == "Null");
        Check(descriptors.back().id == RenderBackendId{"secondary"});

        const Result<void> lateRegistration = RegisterNullRenderBackend(registry);
        Check(lateRegistration.HasError());
        Check(lateRegistration.ErrorValue().code.Value() == "render.registry.sealed");
    }

    TEST_CASE("Registry Requires Seal And Returns Typed Unknown Backend Errors", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(RegisterNullRenderBackend(registry).HasValue());

        auto beforeSeal = registry.Create(RenderBackendId{"null"});
        Check(beforeSeal.HasError());
        Check(beforeSeal.ErrorValue().code.Value() == "render.registry.not_sealed");

        Check(registry.Seal().HasValue());
        auto missing = registry.Create(RenderBackendId{"missing"});
        Check(missing.HasError());
        Check(missing.ErrorValue().code.Value() == "render.registry.backend_not_found");
    }

    TEST_CASE("Registry Rejects A Provider That Returns No Backend Instance", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"null-pointer"},
                      .displayName = "Null Pointer",
                      .provider = MakeTestProvider(ProviderBehavior::ReturnNull),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = registry.Create(RenderBackendId{"null-pointer"});
        Check(created.HasError());
        Check(created.ErrorValue().code.Value() == "render.registry.provider_returned_null");
    }

    TEST_CASE("Registry Preserves Provider Failures And Translates Exceptions", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"failing"},
                      .displayName = "Failing",
                      .provider = MakeTestProvider(ProviderBehavior::ReturnFailure),
                  })
                  .HasValue());
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"throwing"},
                      .displayName = "Throwing",
                      .provider = MakeTestProvider(ProviderBehavior::Throw),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto failed = registry.Create(RenderBackendId{"failing"});
        Check(failed.HasError());
        const Error &failure = failed.ErrorValue();
        Check(failure.code.Value() == "render.test.provider_failure");
        Check(failure.domain.Value() == "horo.render.test");
        Check(failure.severity == ErrorSeverity::Critical);
        Check(failure.message == "Expected provider failure");
        Check(failure.diagnostics.size() == 1);
        Check(failure.diagnostics.front().code.Value() == "render.test.provider_diagnostic");
        Check(failure.diagnostics.front().severity == DiagnosticSeverity::Warning);
        Check(failure.diagnostics.front().message == "Expected provider diagnostic");
        Check(failure.diagnostics.front().location.source == "provider");
        Check(failure.diagnostics.front().location.line == 7);
        Check(failure.diagnostics.front().location.column == 3);

        auto threw = registry.Create(RenderBackendId{"throwing"});
        Check(threw.HasError());
        Check(threw.ErrorValue().code.Value() == "render.registry.provider_exception");
    }

    TEST_CASE("Null Provider Is Inert Until Explicit Initialization", "[unit][runtime][renderer]") {
        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();

        const FrameDescriptor frame{.frameNumber = 1, .outputExtent = {1280, 720}};
        auto beforeInitialize = backend->BeginFrame(frame);
        Check(beforeInitialize.HasError());
        Check(beforeInitialize.ErrorValue().code.Value() == "render.backend.not_initialized");

        const RenderBackendConfig config{.requirePresentation = false, .enableValidation = true, .maxFramesInFlight = 2};
        Check(backend->Initialize(config).HasValue());
        Check(backend->Capabilities().backend == RenderBackendId{"null"});
        Check(!backend->Capabilities().presentsToWindow);
    }

    TEST_CASE("Null Backend Validates Frame Lifecycle Without Gpu Work", "[unit][runtime][renderer]") {
        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());

        const RenderExecutionPlan inactivePlan{};
        Check(backend->Execute(inactivePlan).ErrorValue().code.Value() == "render.backend.no_active_frame");
        Check(backend->Present(FrameToken{1}).ErrorValue().code.Value() == "render.backend.no_active_frame");

        auto begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 7, .outputExtent = {640, 360}});
        Check(begun.HasValue());
        const FrameToken token = begun.Value();

        auto nested = backend->BeginFrame(FrameDescriptor{.frameNumber = 8, .outputExtent = {640, 360}});
        Check(nested.HasError());
        Check(nested.ErrorValue().code.Value() == "render.backend.frame_already_active");

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
            RenderPassDescriptor{.id = RenderPassId{2}, .kind = RenderPassKind::Copy},
        };
        const std::array computePass{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Compute},
        };
        const auto unsupportedCompute = backend->Execute(RenderExecutionPlan{.frame = token, .orderedPasses = computePass});
        Check(unsupportedCompute.HasError());
        Check(unsupportedCompute.ErrorValue().code.Value() == "render.backend.unsupported_pass_kind");

        const std::array invalidPassKind{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = static_cast<RenderPassKind>(255)},
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = token, .orderedPasses = invalidPassKind}).ErrorValue().code.Value() ==
              "render.backend.invalid_execution_plan");

        const std::array duplicatePasses{
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Graphics},
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Copy},
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = token, .orderedPasses = duplicatePasses}).ErrorValue().code.Value() ==
              "render.backend.invalid_execution_plan");
        Check(backend->Resize(FramebufferExtent{800, 600}).ErrorValue().code.Value() == "render.backend.frame_active");
        Check(backend->Execute(RenderExecutionPlan{.frame = token, .orderedPasses = passes}).HasValue());

        const Result<void> wrongPresent = backend->Present(FrameToken{token.value + 1});
        Check(wrongPresent.HasError());
        Check(wrongPresent.ErrorValue().code.Value() == "render.backend.frame_token_mismatch");

        Check(backend->Present(token).HasValue());
        Check(backend->Resize(FramebufferExtent{1920, 1080}).HasValue());
        backend->Shutdown();
        backend->Shutdown();
    }

    TEST_CASE("Null backend satisfies the shared backend contract", "[unit][runtime][renderer][contract]") {
        Test::RunBackendContractSuite(
            Test::BackendContractExpectations{
                .id = RenderBackendId{"null"},
                .presentsToWindow = false,
            },
            &CreateNullBackend);
    }

    TEST_CASE("Null Backend Rejects Presentation Requirements", "[unit][runtime][renderer]") {
        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();

        const Result<void> initialized = backend->Initialize(RenderBackendConfig{.requirePresentation = true});
        Check(initialized.HasError());
        Check(initialized.ErrorValue().code.Value() == "render.null.presentation_unsupported");
    }

    TEST_CASE("Null Backend Validates And Realizes Generic Resources", "[unit][runtime][renderer][resource]") {
        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();

        const std::array<std::byte, 12> bytes{};
        const RenderBufferDescriptor vertexDescriptor{
            .byteSize = bytes.size(),
            .usage = RenderBufferUsage::Vertex,
            .access = RenderBufferAccess::DeviceLocal,
        };
        Check(backend->CreateBuffer(vertexDescriptor, bytes, TestPlacement(bytes.size())).ErrorValue().code.Value() ==
              "render.backend.not_initialized");
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());
        Check(backend->Capabilities().supportsBufferResources);
        Check(backend->Capabilities().supportsMeshResources);

        Check(backend->CreateBuffer({}, {}, TestPlacement(bytes.size())).ErrorValue().code.Value() == "render.backend.invalid_config");
        Check(backend->CreateBuffer(vertexDescriptor, std::span{bytes}.first<4>(), TestPlacement(bytes.size())).ErrorValue().code.Value() ==
              "render.backend.invalid_config");
        auto vertex = backend->CreateBuffer(vertexDescriptor, bytes, TestPlacement(bytes.size()));
        auto index =
            backend->CreateBuffer({.byteSize = bytes.size(), .usage = RenderBufferUsage::Index, .access = RenderBufferAccess::DeviceLocal},
                                  bytes, TestPlacement(bytes.size(), 1, 2));
        Check(vertex.HasValue());
        Check(index.HasValue());
        Check(vertex.Value() != index.Value());

        const RenderMeshDescriptor meshDescriptor{
            .vertexBuffer = {.owner = {1}, .slot = 1, .generation = 1},
            .indexBuffer = {.owner = {1}, .slot = 2, .generation = 1},
            .vertexStride = 4,
            .vertexCount = 3,
            .indexFormat = RenderIndexFormat::UInt32,
            .indexCount = 3,
            .topology = RenderPrimitiveTopology::Triangles,
            .localBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},
        };
        Check(backend->CreateMesh({}, vertex.Value(), index.Value()).ErrorValue().code.Value() == "render.backend.invalid_config");
        Check(backend->CreateMesh(meshDescriptor, 0, index.Value()).ErrorValue().code.Value() == "render.backend.invalid_config");
        Check(backend->CreateMesh(meshDescriptor, vertex.Value(), 0).ErrorValue().code.Value() == "render.backend.invalid_config");
        auto mesh = backend->CreateMesh(meshDescriptor, vertex.Value(), index.Value());
        Check(mesh.HasValue());
        backend->DestroyMesh(mesh.Value());
        backend->DestroyBuffer(vertex.Value());
        backend->DestroyBuffer(index.Value());
        backend->Shutdown();
    }

    TEST_CASE("Null Backend Rejects Invalid Configuration And Frame Extent", "[unit][runtime][renderer]") {
        Check(CreateNullBackend()->Initialize(RenderBackendConfig{.maxFramesInFlight = 0}).ErrorValue().code.Value() ==
              "render.backend.invalid_config");

        Check(
            CreateNullBackend()->Initialize(RenderBackendConfig{.presentMode = static_cast<PresentMode>(0xFF)}).ErrorValue().code.Value() ==
            "render.backend.invalid_config");

        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());
        Check(backend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {0, 720}}).ErrorValue().code.Value() ==
              "render.backend.invalid_frame_descriptor");
    }

    TEST_CASE("Null Backend Validates Primary Output Attachments", "[unit][runtime][renderer]") {
        std::unique_ptr<IRenderBackend> backend = CreateNullBackend();
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());

        auto begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 9, .outputExtent = {640, 360}});
        Check(begun.HasValue());
        const FrameToken frame = begun.Value();
        const std::array validPasses{
            RenderPassDescriptor{
                .id = RenderPassId{10},
                .kind = RenderPassKind::Graphics,
                .primaryOutput =
                    PrimaryOutputAttachment{
                        .loadOperation = AttachmentLoadOperation::Clear,
                        .storeOperation = AttachmentStoreOperation::Store,
                        .clearColor = ClearColor{0.05F, 0.1F, 0.2F, 1.0F},
                    },
            },
            RenderPassDescriptor{
                .id = RenderPassId{11},
                .kind = RenderPassKind::Graphics,
                .primaryOutput =
                    PrimaryOutputAttachment{
                        .loadOperation = AttachmentLoadOperation::Load,
                        .storeOperation = AttachmentStoreOperation::Store,
                    },
            },
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = validPasses}).HasValue());

        const std::array invalidCopyPass{
            RenderPassDescriptor{
                .id = RenderPassId{12},
                .kind = RenderPassKind::Copy,
                .primaryOutput = PrimaryOutputAttachment{},
            },
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = invalidCopyPass}).ErrorValue().code.Value() ==
              "render.backend.invalid_execution_plan");

        const std::array invalidClearPass{
            RenderPassDescriptor{
                .id = RenderPassId{13},
                .kind = RenderPassKind::Graphics,
                .primaryOutput =
                    PrimaryOutputAttachment{
                        .loadOperation = AttachmentLoadOperation::Clear,
                        .clearColor = ClearColor{0.0F, 0.0F, std::numeric_limits<float>::infinity(), 1.0F},
                    },
            },
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = invalidClearPass}).ErrorValue().code.Value() ==
              "render.backend.invalid_execution_plan");
        backend->AbortFrame(frame);
    }
}  // namespace
