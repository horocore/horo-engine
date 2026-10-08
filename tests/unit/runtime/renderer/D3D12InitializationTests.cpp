#include "runtime/renderer/modules/d3d12/D3D12Initialization.h"

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

        void Release() noexcept override {
            state_.calls.emplace_back("release");
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
    CHECK(state.calls == std::vector<std::string>{"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy"});
    CHECK(session.Initialize(host, {}).HasError());
    REQUIRE(session.Shutdown().HasValue());
    CHECK(state.liveObjects == 0);
    REQUIRE(session.Shutdown().HasValue());
    CHECK(session.Initialize(host, {}).HasError());
}

TEST_CASE("D3D12 failed stages preserve native causes and release partial state", "[renderer][d3d12]") {
    for (const std::string stage : {"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy"}) {
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
        CHECK(state.calls ==
              std::vector<std::string>{"verify", "open", "enumerate", "driver", "unsupported-device", "driver", "device", "direct"});
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
    for (const std::string stage : {"verify", "open", "enumerate", "driver", "device", "direct", "compute", "copy"}) {
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
