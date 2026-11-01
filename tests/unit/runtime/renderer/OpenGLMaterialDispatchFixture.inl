/** @file
 * @brief Deterministic material dispatch, fault injection and retained-resource setup for the owning OpenGL test translation unit.
 */

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
Result<void> ExecuteNativeMaterial(const RenderMaterialExecutionRequest &, std::span<const Detail::OpenGLMaterialDrawCommand> commands) {
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
