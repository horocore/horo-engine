#include "D3D12Initialization.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    struct State {
        std::vector<std::string> calls;
        std::string failure;
        std::vector<D3D12AdapterCandidate> adapters{{RenderAdapterId{"d3d12:0000000000000001"}, 100, false, false}};
        CancellationSource cancellation;
        std::string cancelAfter;
        RenderAdapterId selected;
        bool debug{false};
        std::optional<RenderAdapterId> unsupported;
        std::function<void()> onVerify;
        int liveObjects{0};
        ID3D12Resources *resourceService{nullptr};
        ID3D12Commands *commandService{nullptr};
        bool resourcesPrepared{false};
        bool commandsPrepared{false};
        std::uint64_t commandIncarnation{};
        std::vector<D3D12CommandQueueBinding> commandQueues;
        D3D12BarrierMode commandMode{D3D12BarrierMode::Legacy};

        void AttachResources(ID3D12Resources &service) {
            resourceService = &service;
        }

        void CancelAfter(std::string step) {
            cancelAfter = std::move(step);
        }
    };

    Result<void> Step(State &state, const std::string &name) {
        state.calls.push_back(name);
        if (state.cancelAfter == name) {
            state.cancellation.RequestCancellation();
        }
        if (state.failure == name) {
            return Result<void>::Failure(D3D12InitializationError("test.injected", name.c_str(), -123));
        }
        return Result<void>::Success();
    }

    class Host final : public ID3D12HostAdmission {
    public:
        explicit Host(State &state) : state_(state) {}

        Result<void> VerifyRuntime() override {
            if (state_.onVerify) {
                state_.onVerify();
            }
            return Step(state_, "verify");
        }

        Result<void> AdmitDriver(const RenderAdapterId &, std::uint64_t) override {
            return Step(state_, "driver");
        }

    private:
        State &state_;
    };

    // Exercises the portable session drain protocol only. Real copy/backing ownership
    // is separately tested on WARP; any allocation call here is an unexpected test failure.
    class DrainService final : public ID3D12Resources {
    public:
        void Complete() noexcept {
            complete_ = true;
        }

        void SetFailure(bool failed) noexcept {
            failed_ = failed;
        }

        unsigned stops{0};
        unsigned drains{0};

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &) const override {
            return NotCalled<RenderMemoryCostPlan>();
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &) const override {
            return NotCalled<RenderMemoryCostPlan>();
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                           RenderResourceNativeLifetime) override {
            return NotCalled<std::uint64_t>();
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                            RenderResourceNativeLifetime) override {
            return NotCalled<std::uint64_t>();
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t,
                                         RenderResourceNativeLifetime) override {
            return NotCalled<std::uint64_t>();
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t, RenderResourceNativeLifetime) override {
            return NotCalled<std::uint64_t>();
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t,
                                                 RenderResourceNativeLifetime) override {
            return NotCalled<std::uint64_t>();
        }

        void DestroyBuffer(std::uint64_t) noexcept override {}

        void DestroyTexture(std::uint64_t) noexcept override {}

        void DestroyMesh(std::uint64_t) noexcept override {}

        void DestroyTextureView(std::uint64_t) noexcept override {}

        void DestroyRenderTarget(std::uint64_t) noexcept override {}

        Result<D3D12TransferId> Upload(std::uint64_t, std::span<const std::byte>, std::size_t, const CancellationToken &) override {
            return NotCalled<D3D12TransferId>();
        }

        Result<D3D12TransferId> Readback(std::uint64_t, std::size_t, std::size_t, const CancellationToken &) override {
            return NotCalled<D3D12TransferId>();
        }

        Result<std::vector<std::byte>> TakeReadback(D3D12TransferId) override {
            return NotCalled<std::vector<std::byte>>();
        }

        Result<void> Cancel(D3D12TransferId) override {
            return NotCalled<void>();
        }

        Result<void> Poll() override {
            return NotCalled<void>();
        }

        void StopAdmission() noexcept override {
            ++stops;
        }

        Result<bool> PollDrain() override {
            ++drains;
            if (failed_)
                return Result<bool>::Failure(D3D12InitializationError("test.drain_failed", "Injected native drain failure."));
            return Result<bool>::Success(complete_);
        }

        Result<D3D12ResidencyObservation> QueryResidency() const override {
            return NotCalled<D3D12ResidencyObservation>();
        }

        D3D12ResourceSnapshot Snapshot() const noexcept override {
            return {.deviceIncarnation = 42, .accepting = stops == 0};
        }

    private:
        bool complete_{false};
        bool failed_{false};

        template <typename T> static Result<T> NotCalled() {
            FAIL("Portable drain protocol tests must not perform native resource operations");
            return Result<T>::Failure(D3D12InitializationError("test.unexpected_resource_call", "Unexpected native operation."));
        }
    };

    // Native-free ownership probe: only the initialization session decides when
    // its runtime parents may be released; this does not simulate GPU completion.
    class CommandDrainService final : public ID3D12Commands {
    public:
        bool accepting{true};
        bool complete{false};
        unsigned drains{};

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &, const CancellationToken &) override {
            FAIL("Initialization tests must not submit graph work");
            return Result<void>::Failure(D3D12InitializationError("test.unexpected_command", "Unexpected graph work."));
        }

        Result<bool> Observe(const RenderCpuWait &) override {
            FAIL("Initialization tests must not observe native timelines");
            return Result<bool>::Success(false);
        }

        Result<void> Poll() override {
            FAIL("Initialization tests must use the session drain protocol");
            return Result<void>::Success();
        }

        Result<void> BeginShutdown() override {
            accepting = false;
            return Result<void>::Success();
        }

        Result<RenderBackendDrainProgress> PollDrain() override {
            ++drains;
            return Result<RenderBackendDrainProgress>::Success(complete ? RenderBackendDrainProgress::Drained
                                                                        : RenderBackendDrainProgress::Pending);
        }
    };

    class Runtime final : public ID3D12InitializationRuntime {
    public:
        explicit Runtime(State &state) : state_(state) {}

        Result<void> Open(const bool debug) override {
            state_.debug = debug;
            ++state_.liveObjects;
            return Step(state_, "open");
        }

        Result<std::vector<D3D12AdapterCandidate>> Enumerate(std::uint32_t) override {
            auto result = Step(state_, "enumerate");
            if (result.HasError()) {
                return Result<std::vector<D3D12AdapterCandidate>>::Failure(result.ErrorValue());
            }
            return Result<std::vector<D3D12AdapterCandidate>>::Success(state_.adapters);
        }

        Result<void> CreateDevice(const RenderAdapterId &adapter) override {
            state_.selected = adapter;
            if (state_.unsupported == adapter) {
                state_.calls.emplace_back("unsupported-device");
                return Result<void>::Failure(D3D12InitializationError("render.d3d12.shader_model_unsupported", "SM 6.0 is unavailable."));
            }
            ++state_.liveObjects;
            return Step(state_, "device");
        }

        Result<void> CreateQueue(const D3D12QueueKind kind) override {
            ++state_.liveObjects;
            return Step(state_, kind == D3D12QueueKind::Direct ? "direct" : kind == D3D12QueueKind::Compute ? "compute" : "copy");
        }

        Result<void> InitializeResources(const D3D12ResourceLimits &) override {
            auto result = Step(state_, "resources");
            state_.resourcesPrepared = result.HasValue();
            return result;
        }

        Result<void> InitializeCommands(const D3D12CommandConfiguration &configuration) override {
            REQUIRE(state_.resourcesPrepared);
            REQUIRE(state_.resourceService != nullptr);
            REQUIRE(state_.liveObjects == 3);
            state_.commandIncarnation = configuration.deviceIncarnation;
            state_.commandQueues.assign(configuration.queues.begin(), configuration.queues.end());
            state_.commandMode = configuration.barrierMode;
            auto result = Step(state_, "commands");
            state_.commandsPrepared = result.HasValue();
            return result;
        }

        ID3D12Commands *Commands() noexcept override {
            return state_.commandsPrepared ? state_.commandService : nullptr;
        }

        ID3D12Resources *Resources() noexcept override {
            return state_.resourceService;
        }

        void Release() noexcept override {
            state_.calls.emplace_back("release");
            state_.resourceService = nullptr;
            state_.commandService = nullptr;
            state_.resourcesPrepared = false;
            state_.commandsPrepared = false;
            state_.liveObjects = 0;
        }

    private:
        State &state_;
    };
}  // namespace

TEST_CASE("D3D12 startup owns exact device queues and closes admission", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(state.calls.empty());
    auto result = session.Initialize(host, {.enableDebugLayer = true, .enableComputeQueue = true, .enableCopyQueue = true});
    REQUIRE(result.HasValue());
    CHECK(result.Value() == state.adapters.front().id);
    CHECK(state.debug);
    CHECK_FALSE(state.commandsPrepared);
    CHECK(session.Commands().HasError());
    CHECK(state.calls ==
          std::vector<std::string>{"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy", "resources"});
    CHECK(session.Initialize(host, {}).HasError());
    REQUIRE(session.Shutdown().HasValue());
    CHECK(state.liveObjects == 0);
    REQUIRE(session.Shutdown().HasValue());
    CHECK(session.Initialize(host, {}).HasError());
}

TEST_CASE("D3D12 failed stages preserve native causes and release partial state", "[renderer][d3d12]") {
    for (const std::string stage : {"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy", "resources"}) {
        INFO(stage);
        State state;
        state.failure = stage;
        Host host{state};
        D3D12Initialization session{std::make_unique<Runtime>(state)};
        auto result = session.Initialize(host, {.enableComputeQueue = true, .enableCopyQueue = true});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "test.injected");
        CHECK(result.ErrorValue().message.find("-123") != std::string::npos);
        CHECK(state.liveObjects == 0);
        CHECK(state.calls.back() == (stage == "verify" ? "verify" : "release"));
        state.failure.clear();
        REQUIRE(session.Initialize(host, {}).HasValue());
    }
}

TEST_CASE("D3D12 session destruction releases a ready device without explicit shutdown", "[renderer][d3d12]") {
    State state;
    Host host{state};
    {
        D3D12Initialization session{std::make_unique<Runtime>(state)};
        REQUIRE(session.Initialize(host, {.maxAdapters = 64}).HasValue());
        CHECK(state.liveObjects == 3);
    }
    CHECK(state.liveObjects == 0);
    CHECK(state.calls.back() == "release");
}

TEST_CASE("D3D12 host admission cannot reenter initialization or release an active attempt", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    state.onVerify = [&] {
        CHECK(session.Initialize(host, {}).HasError());
        auto shutdown = session.Shutdown();
        REQUIRE(shutdown.HasError());
        CHECK(shutdown.ErrorValue().code.Value() == "render.d3d12.initialization_in_progress");
        CHECK(state.calls.empty());
    };
    REQUIRE(session.Initialize(host, {}).HasValue());
    CHECK(state.liveObjects == 3);
}

TEST_CASE("D3D12 exact adapter and software policy never silently fall back", "[renderer][d3d12]") {
    State state;
    state.adapters.insert(state.adapters.begin(), {RenderAdapterId{"d3d12:software"}, 1, true, false});
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    SECTION("default skips software") {
        REQUIRE(session.Initialize(host, {}).HasValue());
        CHECK(state.selected == state.adapters.back().id);
    }
    SECTION("software requires explicit test admission") {
        REQUIRE(session.Initialize(host, {.adapter = state.adapters.front().id}).HasError());
        REQUIRE(session.Initialize(host, {.adapter = state.adapters.front().id, .allowSoftwareForTests = true}).HasValue());
    }
    SECTION("missing exact adapter fails") {
        REQUIRE(session.Initialize(host, {.adapter = RenderAdapterId{"d3d12:missing"}}).HasError());
        CHECK(state.selected.Value().empty());
    }
    SECTION("remote adapters never satisfy interactive admission") {
        state.adapters.back().remote = true;
        REQUIRE(session.Initialize(host, {}).HasError());
    }
}

TEST_CASE("D3D12 rejects malformed or excessive discovery facts", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    SECTION("duplicate") {
        state.adapters.push_back(state.adapters.front());
    }
    SECTION("invalid identity") {
        state.adapters.front().id = RenderAdapterId{};
    }
    SECTION("overflow") {
        state.adapters.push_back({RenderAdapterId{"d3d12:second"}});
    }
    SECTION("empty") {
        state.adapters.clear();
    }
    REQUIRE(session.Initialize(host, {.maxAdapters = 1}).HasError());
    CHECK(state.liveObjects == 0);
    CHECK(state.selected.Value().empty());
}

TEST_CASE("D3D12 default order qualifies required features but exact selection never retries", "[renderer][d3d12]") {
    State state;
    state.unsupported = state.adapters.front().id;
    state.adapters.push_back({RenderAdapterId{"d3d12:second"}, 101, false, false});
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    SECTION("automatic qualification advances only after baseline rejection") {
        auto result = session.Initialize(host, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value() == state.adapters.back().id);
        CHECK(state.calls == std::vector<std::string>{"verify", "open", "enumerate", "driver", "unsupported-device", "driver", "device",
                                                      "direct", "resources"});
    }
    SECTION("explicit baseline rejection is preserved") {
        auto result = session.Initialize(host, {.adapter = state.adapters.front().id});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.d3d12.shader_model_unsupported");
        CHECK(state.selected == state.adapters.front().id);
        CHECK(state.liveObjects == 0);
    }
}

TEST_CASE("D3D12 cancellation rolls back each synchronous acquisition boundary", "[renderer][d3d12]") {
    for (const std::string stage : {"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy", "resources"}) {
        INFO(stage);
        State state;
        state.cancelAfter = stage;
        Host host{state};
        D3D12Initialization session{std::make_unique<Runtime>(state)};
        auto result =
            session.Initialize(host, {.enableComputeQueue = true, .enableCopyQueue = true, .cancellation = state.cancellation.Token()});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.d3d12.cancelled");
        CHECK(state.liveObjects == 0);
    }
}

TEST_CASE("D3D12 owner thread and invalid requests fail before native acquisition", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Initialize(host, {.maxAdapters = 0}).HasError());
    REQUIRE(session.Initialize(host, {.maxAdapters = 65}).HasError());
    REQUIRE(session.Initialize(host, {.adapter = RenderAdapterId{}}).HasError());
    state.cancellation.RequestCancellation();
    REQUIRE(session.Initialize(host, {.cancellation = state.cancellation.Token()}).HasError());
    bool initializationRejected{false};
    bool shutdownRejected{false};
    std::thread other{[&] {
        initializationRejected = session.Initialize(host, {}).HasError();
        shutdownRejected = session.Shutdown().HasError();
    }};
    other.join();
    CHECK(initializationRejected);
    CHECK(shutdownRejected);
    CHECK(state.calls.empty());
}

#if !defined(_WIN32) || !defined(_M_X64)
TEST_CASE("D3D12 production runtime reports unsupported hosts without native acquisition", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{CreateD3D12InitializationRuntime()};
    auto result = session.Initialize(host, {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "render.d3d12.unsupported_host");
}
#endif

TEST_CASE("D3D12 resource startup preserves acquisition errors and cancellation", "[renderer][d3d12]") {
    State state;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Resources().HasError());
    state.failure = "resources";
    auto result = session.Initialize(host, {});
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "test.injected");
    REQUIRE(state.liveObjects == 0);
    state.failure.clear();
    state.CancelAfter("resources");
    result = session.Initialize(host, {.cancellation = state.cancellation.Token()});
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "render.d3d12.cancelled");
    REQUIRE(state.liveObjects == 0);
}

TEST_CASE("D3D12 session shutdown retains the native device until resource drain is actually complete", "[renderer][d3d12][lifecycle]") {
    State state;
    DrainService resources;
    state.AttachResources(resources);
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Initialize(host, {}).HasValue());
    REQUIRE(session.Resources().HasValue());
    REQUIRE(session.Resources().Value() == &resources);
    auto pending = session.Shutdown();
    REQUIRE(pending.HasError());
    REQUIRE(pending.ErrorValue().code.Value() == "render.d3d12.drain_pending");
    REQUIRE(state.liveObjects == 3);
    REQUIRE(resources.stops == 1);
    REQUIRE(resources.drains == 1);
    resources.Complete();
    REQUIRE(session.Shutdown().HasValue());
    REQUIRE(state.liveObjects == 0);
    REQUIRE(session.Resources().HasError());
    REQUIRE(session.Shutdown().HasValue());
    REQUIRE(resources.stops == 2);
    REQUIRE(resources.drains == 2);
}

TEST_CASE("D3D12 session shutdown preserves resource drain failure and permits owner-thread recovery", "[renderer][d3d12][lifecycle]") {
    State state;
    DrainService resources;
    state.AttachResources(resources);
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Initialize(host, {}).HasValue());
    resources.SetFailure(true);
    const auto failed = session.Shutdown();
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == "test.drain_failed");
    REQUIRE(state.liveObjects == 3);
    resources.SetFailure(false);
    resources.Complete();
    REQUIRE(session.Shutdown().HasValue());
    REQUIRE(state.liveObjects == 0);
}

TEST_CASE("D3D12 initialization publishes commands only after canonical resources are prepared", "[renderer][d3d12][commands]") {
    State state;
    DrainService resources;
    CommandDrainService commands;
    state.AttachResources(resources);
    state.commandService = &commands;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Commands().HasError());
    REQUIRE(session.Initialize(host, {.commandQueues = {{{17}, D3D12QueueKind::Direct}}, .barrierMode = D3D12BarrierMode::Enhanced})
                .HasValue());
    REQUIRE(session.Commands().HasValue());
    CHECK(session.Commands().Value() == &commands);
    REQUIRE(session.Resources().HasValue());
    CHECK(session.Resources().Value() == &resources);
    CHECK(state.commandIncarnation == resources.Snapshot().deviceIncarnation);
    REQUIRE(state.commandQueues.size() == 1);
    CHECK(state.commandQueues.front().queue == RenderQueueId{17});
    CHECK(state.commandQueues.front().kind == D3D12QueueKind::Direct);
    CHECK(state.commandMode == D3D12BarrierMode::Enhanced);
    commands.complete = true;
    resources.Complete();
    REQUIRE(session.Shutdown().HasValue());
}

TEST_CASE("D3D12 failed command initialization rolls back without publishing either service", "[renderer][d3d12][commands]") {
    State state;
    DrainService resources;
    CommandDrainService commands;
    state.AttachResources(resources);
    state.commandService = &commands;
    state.failure = "commands";
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    const auto failed = session.Initialize(host, {.commandQueues = {{{17}, D3D12QueueKind::Direct}}});
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == "test.injected");
    CHECK(failed.ErrorValue().message.find("-123") != std::string::npos);
    CHECK(state.commandIncarnation == 42);
    CHECK(state.liveObjects == 0);
    CHECK_FALSE(state.resourcesPrepared);
    CHECK_FALSE(state.commandsPrepared);
    CHECK(session.Resources().HasError());
    CHECK(session.Commands().HasError());
}

TEST_CASE("D3D12 shutdown retains runtime parents through command and then resource drain", "[renderer][d3d12][commands][lifecycle]") {
    State state;
    DrainService resources;
    CommandDrainService commands;
    state.AttachResources(resources);
    state.commandService = &commands;
    Host host{state};
    D3D12Initialization session{std::make_unique<Runtime>(state)};
    REQUIRE(session.Initialize(host, {.commandQueues = {{{17}, D3D12QueueKind::Direct}}}).HasValue());
    const auto commandPending = session.Shutdown();
    REQUIRE(commandPending.HasError());
    CHECK(commandPending.ErrorValue().code.Value() == "render.d3d12.drain_pending");
    CHECK_FALSE(commands.accepting);
    CHECK_FALSE(resources.Snapshot().accepting);
    CHECK(resources.drains == 0);
    CHECK(state.liveObjects == 3);
    CHECK(state.resourceService == &resources);
    CHECK(state.commandService == &commands);
    commands.complete = true;
    const auto resourcePending = session.Shutdown();
    REQUIRE(resourcePending.HasError());
    CHECK(resourcePending.ErrorValue().code.Value() == "render.d3d12.drain_pending");
    CHECK(resources.drains == 1);
    CHECK(state.liveObjects == 3);
    resources.Complete();
    REQUIRE(session.Shutdown().HasValue());
    CHECK(state.liveObjects == 0);
    CHECK(state.resourceService == nullptr);
    CHECK(state.commandService == nullptr);
    CHECK(session.Resources().HasError());
    CHECK(session.Commands().HasError());
    REQUIRE(session.Shutdown().HasValue());
}
