#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "AllocationProbe.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Vfx/VfxWorld.h"
#include "OpenGLBackendInternal.h"
#include "OpenGLMaterialFixture.h"
#include "OpenGLRenderBackendErrors.h"
#include "OpenGLRenderTestSupport.h"
#include "OpenGLResourceGraphTestSupport.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderTransientGraphTestSupport.h"
#include "TypedIdentityTestSupport.h"

#include <functional>
#include <limits>
#include <optional>

namespace Horo::Render::OpenGLResourceTests {
    /** @brief Borrows the existing deterministic resource dispatch; never creates a real context. */
    Detail::OpenGLCommandFunctions ResourceProbeFunctions() noexcept;
}  // namespace Horo::Render::OpenGLResourceTests

namespace Horo::Render::OpenGLMaterialTests {
    enum class DrawException {
        None,
        Standard,
        Nonstandard,
        Allocation
    };

    struct NativeState {
        std::uint32_t nextShader{1000}, nextProgram{2000}, nextSampler{3000};
        std::size_t shadersDeleted{}, programsDeleted{}, samplersDeleted{}, draws{};
        bool failDraw{}, throwDraw{}, failLink{};
        DrawException exception{DrawException::None};
        bool allocationWindow{};
        std::size_t uniformBytes{16}, uploads{}, uniformQueries{};
        std::uint32_t instances{};
        bool failUpload{};
        std::function<void()> onUniformQuery;
        std::function<void()> onUpload;
        std::function<void()> onDraw;
    };

    thread_local NativeState native;
    thread_local IRenderBackend *borrowedBackend{nullptr};

    /** @brief Scoped typed one-shot failures; records cleanup before the public translation's second allocation fault. */
    struct TranslationFailureProbe {
        std::optional<Horo::Tests::AllocationProbe::ScopedFailure> failure;
        std::size_t faults{};
        int fencesAtTranslation{};

        void Observe(std::size_t) noexcept {
            if (!native.allocationWindow || faults == 2)
                return;
            ++faults;
            if (faults == 2)
                fencesAtTranslation = OpenGLBackendTests::commandState.fenceCount;
            failure.emplace();
        }
    };

    /** @brief Transfers exactly one inert real backend to the test host while retaining a borrowed fault-injection view. */
    class TestProvider final : public IRenderBackendProvider {
    public:
        explicit TestProvider(std::unique_ptr<IRenderBackend> backend) : backend_(std::move(backend)) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            if (!backend_)
                return Result<std::unique_ptr<IRenderBackend>>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig));
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::move(backend_));
        }

    private:
        mutable std::unique_ptr<IRenderBackend> backend_;
    };

    /** @brief Returns selected-context compatibility facts from a deterministic fake, never device evidence. */
    RenderPipelineDeviceIdentity Identity() {
        Sha256Digest digest;
        digest.bytes[0] = 1;
        return {RenderBackendId{"opengl"}, 1, RenderAdapterId{"opengl.test"}, digest, "test-driver", "1"};
    }

    /** @brief Supplies shader faults independently of material and completion dispatch. */
    Detail::OpenGLShaderFunctions ShaderFunctions() {
        return {.isAvailable = []() noexcept {
            return true;
        }, .createShader = [](std::uint32_t) noexcept {
            return native.nextShader++;
        }, .shaderSource = [](std::uint32_t, std::span<const std::uint8_t>) noexcept {
        }, .compileShader = [](std::uint32_t) noexcept {
        }, .shaderParameter = [](std::uint32_t, std::uint32_t, std::int32_t &value) noexcept {
            value = 1;
        }, .shaderLog = [](std::uint32_t, std::span<char>) noexcept -> std::size_t {
            return 0;
        }, .deleteShader = [](std::uint32_t) noexcept {
            ++native.shadersDeleted;
        }, .createProgram = []() noexcept {
            return native.nextProgram++;
        }, .attachShader = [](std::uint32_t, std::uint32_t) noexcept {
        }, .linkProgram = [](std::uint32_t) noexcept {
        }, .programParameter = [](std::uint32_t, std::uint32_t, std::int32_t &value) noexcept {
            value = native.failLink ? 0 : 1;
        }, .programLog = [](std::uint32_t, std::span<char>) noexcept -> std::size_t {
            return 0;
        }, .deleteProgram = [](std::uint32_t) noexcept {
            ++native.programsDeleted;
        }, .error = []() noexcept -> std::uint32_t {
            return 0;
        }};
    }

    /** @brief Executes the native draw fault window independently of inert resource-dispatch composition. */
    Result<void> ExecuteNativeMaterial(const RenderMaterialExecutionRequest &,
                                       std::span<const Detail::OpenGLMaterialDrawCommand> commands) {
        using enum DrawException;
        native.draws += commands.size();
        for (const auto &command : commands)
            native.instances += command.instanceCount;
        if (native.onDraw) {
            auto callback = std::move(native.onDraw);
            callback();
        }
        native.allocationWindow = true;
        if (native.exception == Standard)
            throw std::length_error{"native material metadata"};
        if (native.exception == Nonstandard)
            throw 17;
        if (native.exception == Allocation || native.throwDraw)
            throw std::bad_alloc{};
        if (native.failDraw)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed, "partial native draw"));
        return Result<void>::Success();
    }

    Detail::OpenGLCommandFunctions Functions() {
        auto functions = OpenGLResourceTests::GraphProbeFunctions();
        functions.shaders = ShaderFunctions();
        functions.pipelines = {.isAvailable = []() noexcept {
            return true;
        }, .validateAndMap = [](std::uint32_t, const RenderGraphicsPipelineDescriptor &) {
            return Result<void>::Success();
        }, .queryDeviceIdentity = []() {
            return Result<RenderPipelineDeviceIdentity>::Success(Identity());
        }};
        functions.samplers = {.isAvailable = []() noexcept {
            return true;
        }, .generate = [](std::uint32_t &value) noexcept {
            value = native.nextSampler++;
        }, .destroy = [](std::uint32_t) noexcept {
            ++native.samplersDeleted;
        }, .parameterInteger = [](std::uint32_t, std::uint32_t, std::int32_t) noexcept {
        }, .parameterFloat = [](std::uint32_t, std::uint32_t, float) noexcept {
        }, .error = []() noexcept -> std::uint32_t {
            return 0;
        }};
        functions.materials = {.uniformBlockSize = [](std::uint32_t, const std::string &) {
            ++native.uniformQueries;
            if (native.onUniformQuery) {
                auto callback = std::move(native.onUniformQuery);
                callback();
            }
            return Result<std::size_t>::Success(native.uniformBytes);
        }, .textureComparison = [](std::uint32_t, const std::string &) {
            return Result<bool>::Success(false);
        }, .uniformOffsetAlignment = []() {
            return Result<std::size_t>::Success(16);
        }};
        functions.materialExecution = {.isAvailable = []() noexcept {
            return true;
        }, .queryFeatures = []() {
            return Result<Detail::OpenGLMaterialContextFeatures>::Success({1, 8, false});
        }, .execute = &ExecuteNativeMaterial};
        functions.buffers.bufferSubData = [](std::uint32_t, std::span<const std::byte>) noexcept {
            ++native.uploads;
            if (native.onUpload) {
                auto callback = std::move(native.onUpload);
                callback();
            }
            if (native.failUpload)
                OpenGLBackendTests::commandState.error = 0x0502;
        };
        return functions;
    }

    std::unique_ptr<RenderFrontend> Create(OpenGLResourceTests::ResourcePresentationPort &port) {
        native = {};
        OpenGLResourceTests::resourceCommandState = {};
        OpenGLResourceTests::graphCommandState = {};
        OpenGLBackendTests::commandState = {};
        RenderBackendRegistry registry;
        REQUIRE(Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, {}, Functions()).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto backend = registry.Create(RenderBackendId{"opengl"});
        REQUIRE(backend.HasValue());
        borrowedBackend = backend.Value().get();
        RenderBackendRegistry host;
        REQUIRE(host.Register({.id = RenderBackendId{"opengl"},
                               .displayName = "OpenGL native fault fixture",
                               .provider = std::make_unique<TestProvider>(std::move(backend).Value())})
                    .HasValue());
        REQUIRE(host.Seal().HasValue());
        auto frontend = RenderFrontend::Create(host, RenderBackendId{"opengl"}, {});
        REQUIRE(frontend.HasValue());
        return std::move(frontend).Value();
    }

    struct Fixture {
        PipelineFixture pipeline;
        RenderMeshHandle mesh;
        TargetFixture target;
        TextureFixture sample;
        RenderSamplerHandle sampler;
        std::unique_ptr<MaterialBindingTable> table;
        MaterialBindingGenerationId generation;
        std::shared_ptr<const ResidentMaterialBinding> material;
    };

    Fixture Prepare(RenderFrontend &frontend) {
        auto pipeline = PreparePipeline(frontend);
        const auto mesh = PrepareMesh(frontend);
        const auto target = PrepareTarget(frontend);
        const auto sample =
            PrepareTexture(frontend, {.extent = {1, 1}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled},
                           RenderTextureAspect::Color);
        const auto sampler = frontend.CreateSampler({});
        REQUIRE(sampler.HasValue());
        auto table = MaterialBindingTable::Create(pipeline.pipeline.owner, frontend);
        REQUIRE(table.HasValue());
        const auto published = table.Value()->Publish(Material(pipeline, sample.view, sampler.Value().handle));
        REQUIRE(published.HasValue());
        const auto acquired = table.Value()->Acquire(published.Value().binding);
        REQUIRE(acquired.HasValue());
        return {std::move(pipeline), mesh, target, sample, sampler.Value().handle, std::move(table).Value(), published.Value().binding,
                acquired.Value()};
    }

    /** @brief Removes CPU discovery while submitted-frame retention remains the only live material owner. */
    void ReleaseCpu(RenderFrontend &frontend, Fixture &fixture) {
        REQUIRE(frontend.ReleaseSampler(fixture.sampler).HasValue());
        REQUIRE(frontend.ReleaseGraphicsPipeline(fixture.pipeline.pipeline).HasValue());
        REQUIRE(frontend.ReleaseShaderModule(fixture.pipeline.vertex).HasValue());
        REQUIRE(frontend.ReleaseShaderModule(fixture.pipeline.fragment).HasValue());
        REQUIRE(fixture.table->Release(fixture.generation).HasValue());
        fixture.material.reset();
        REQUIRE(fixture.table->Shutdown().HasValue());
        fixture.table.reset();
    }

    TEST_CASE("Prepared OpenGL material consumption retains sampler and program through exact completion", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        REQUIRE(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}).HasValue());
        REQUIRE(native.draws == 1);
        Horo::Tests::RequireError(frontend->ReleaseSampler(fixture.sampler), FrontendErrors::ResourceChangeDuringFrame);
        Horo::Tests::RequireError(frontend->ReleaseGraphicsPipeline(fixture.pipeline.pipeline), FrontendErrors::ResourceChangeDuringFrame);
        Horo::Tests::RequireError(frontend->ReleaseShaderModule(fixture.pipeline.vertex), FrontendErrors::ResourceChangeDuringFrame);
        frame.Cancel();
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        CHECK(native.shadersDeleted == 0);
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.samplersDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        // Completion authority polls fences at frame admission; upload/registry drains do not advance native completion.
        auto completed = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(completed.HasValue());
        auto nextFrame = std::move(completed).Value();
        nextFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

    TEST_CASE("Partial OpenGL material failure and exception retain generations through fence failure and shutdown",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        SECTION("Native failure") {
            native.failDraw = true;
        }
        SECTION("Owned native metadata exception") {
            native.throwDraw = true;
        }
        OpenGLBackendTests::commandState.fenceFails = true;
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        CHECK(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}).HasError());
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        const auto failedClose = frontend->BeginShutdown();
        Horo::Tests::RequireError(failedClose, OpenGLBackendErrors::SynchronizationFailed);
        const auto pending = frontend->PollDrain();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        OpenGLBackendTests::commandState.fenceFails = false;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        REQUIRE(frontend->BeginShutdown().HasValue());
        const auto closing = frontend->PollDrain();
        REQUIRE(closing.HasValue());
        CHECK(closing.Value() == RenderBackendDrainProgress::Pending);
        CHECK(native.programsDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        frontend.reset();
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

    TEST_CASE("OpenGL material exception boundary cleans up before and after lease transfer", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        SECTION("Before transfer") {
            port.throwOnMakeCurrent = true;
        }
        SECTION("Standard native exception after transfer") {
            native.exception = DrawException::Standard;
        }
        SECTION("Nonstandard native exception after transfer") {
            native.exception = DrawException::Nonstandard;
        }
        SECTION("Allocation exception after transfer") {
            native.exception = DrawException::Allocation;
        }
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        const auto result = frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws});
        Horo::Tests::RequireError(result, FrontendErrors::FrameException);
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        frame.Cancel();
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        const bool transferred = !port.throwOnMakeCurrent;
        port.throwOnMakeCurrent = false;
        native.exception = DrawException::None;
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.programsDeleted == (transferred ? 0 : 1));
        CHECK(native.samplersDeleted == (transferred ? 0 : 1));
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        auto recovered = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(recovered.HasValue());
        auto recoveredFrame = std::move(recovered).Value();
        recoveredFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.programsDeleted == 1);
        CHECK(native.samplersDeleted == 1);
    }

    TEST_CASE("OpenGL material double allocation failure completes cleanup before public error construction escapes",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        native.failDraw = true;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        TranslationFailureProbe probe;
        bool allocationEscaped{};
        {
            // The existing observer ABI borrows opaque context; this capture-free adapter restores its exact stack-owned type.
            const Horo::Tests::AllocationProbe::AllocationObserver adapter = [](std::size_t bytes, auto *opaque) noexcept {
                static_cast<TranslationFailureProbe *>(opaque)->Observe(bytes);
            };
            Horo::Tests::AllocationProbe::ScopedObserver observer{adapter, &probe};
            try {
                static_cast<void>(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}));
            } catch (const std::bad_alloc &) {
                allocationEscaped = true;
            }
        }
        probe.failure.reset();
        CHECK(allocationEscaped);
        CHECK(probe.faults == 2);
        CHECK(probe.fencesAtTranslation == 1);
        frame.Cancel();
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.programsDeleted == 0);
        CHECK(native.samplersDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        auto recovered = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(recovered.HasValue());
        auto recoveredFrame = std::move(recovered).Value();
        recoveredFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.programsDeleted == 1);
        CHECK(native.samplersDeleted == 1);
    }

    TEST_CASE("OpenGL material admission rejects stale generations and mismatched layout before parameter allocation",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto invalid = Material(fixture.pipeline, fixture.sample.view, fixture.sampler);
        const auto buffers = OpenGLResourceTests::resourceCommandState.generatedBuffers;
        SECTION("Exact native layout mismatch") {
            invalid.layout.targetBindings[0].nativeName = "OtherBlock";
        }
        SECTION("Stale sampler discovery") {
            REQUIRE(frontend->ReleaseSampler(fixture.sampler).HasValue());
        }
        SECTION("Stale pipeline generation") {
            ++invalid.pipeline.generation;
        }
        CHECK(frontend->Realize(invalid).HasError());
        CHECK(OpenGLResourceTests::resourceCommandState.generatedBuffers == buffers);
        CHECK(native.draws == 0);
        fixture.material.reset();
        REQUIRE(fixture.table->Shutdown().HasValue());
        fixture.table.reset();
    }

    TEST_CASE("OpenGL graphics pipeline rejects stale device and shader identities without native linking",
              "[renderer][opengl][pipeline]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto pipeline = PreparePipeline(*frontend);
        auto invalid = pipeline.descriptor;
        SECTION("Changed device identity") {
            invalid.compatibility.driverVersion = "other-driver";
        }
        SECTION("Stale shader generation") {
            ++invalid.vertex.generation;
        }
        const auto programs = native.nextProgram;
        CHECK(frontend->CreateGraphicsPipeline(invalid).HasError());
        CHECK(native.nextProgram == programs);
    }

    TEST_CASE("OpenGL material consumption rejects missing native instances before encoding", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        // Deliberate backend-loss injection in a fake context, not a supported CPU release path. Registry generations
        // remain live so this exercises the native-instance admission checks independently of frontend stale-handle checks.
        SECTION("Missing native sampler") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.sampler);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroySampler(instance.Value());
        }
        SECTION("Missing native program") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.pipeline.pipeline);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroyGraphicsPipeline(instance.Value());
        }
        SECTION("Missing native mesh") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.mesh);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroyMesh(instance.Value());
        }
        SECTION("Missing native parameter buffer") {
            borrowedBackend->DestroyBuffer(OpenGLResourceTests::resourceCommandState.nextObject - 1);
        }
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        const auto result = frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws});
        Horo::Tests::RequireError(result, OpenGLBackendErrors::ResourceIdentityInvalid);
        CHECK(native.draws == 0);
        draws[0].material.reset();
        fixture.material.reset();
        REQUIRE(fixture.table->Shutdown().HasValue());
        fixture.table.reset();
    }

    TEST_CASE("Cancelling before OpenGL material encoding transfers no completion ownership", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        frame.Cancel();
        ReleaseCpu(*frontend, fixture);
        CHECK(native.draws == 0);
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

#include "OpenGLVfxFrameTests.inl"

// Draw preparation consumes the material/slot fixture declarations from the preceding chunk.
#include "OpenGLVfxDrawPreparationTests.inl"
}  // namespace Horo::Render::OpenGLMaterialTests
