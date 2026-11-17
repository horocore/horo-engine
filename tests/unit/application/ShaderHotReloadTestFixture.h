#pragma once

#include "../runtime/renderer/ShaderCompilerPipelineTestFixture.h"
#include "Horo/Application/ShaderHotReloadService.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <future>
#include <thread>

namespace Horo::Application::ShaderHotReloadTests {
    using namespace Render;

    class Adapter final : public IShaderCompilerAdapter {
    public:
        mutable std::atomic<unsigned> entered{};
        std::atomic<bool> hold{};
        std::atomic<bool> fail{};
        std::atomic<std::uint8_t> failureMarker{};
        mutable std::atomic<bool> wrongThread{};
        const std::thread::id owner{std::this_thread::get_id()};

        Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                    const CancellationToken &token) const override {
            if (std::this_thread::get_id() == owner)
                wrongThread = true;
            ++entered;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (hold && !token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            if (fail || invocation.source.front() == failureMarker || token.IsCancellationRequested())
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::AdapterFailure));
            ShaderCompilerAdapterOutput result;
            result.backend = invocation.target.requirement.backend;
            result.payloadFormat = invocation.target.requirement.payloadFormat;
            result.payload = {invocation.source.front()};
            return Result<ShaderCompilerAdapterOutput>::Success(std::move(result));
        }
    };

    class Candidate final : public IShaderHotReloadCandidate {
    public:
        std::vector<std::uint8_t> markers;
    };

    class Target final : public IShaderHotReloadTarget {
    public:
        ShaderHotReloadTargetState state{ShaderTargetBackend::Null, 1, 1};
        std::vector<std::uint8_t> live;
        unsigned preparations{}, adoptions{};
        bool failPreparation{}, failAdoption{}, wrongThread{};
        const std::thread::id owner{std::this_thread::get_id()};

        Result<ShaderHotReloadTargetState> CaptureState() const override {
            return Result<ShaderHotReloadTargetState>::Success(state);
        }

        Result<std::unique_ptr<IShaderHotReloadCandidate>> Prepare(std::span<const ShaderHotReloadProgramView> programs,
                                                                   ShaderHotReloadTargetState expected,
                                                                   const CancellationToken &) override {
            ++preparations;
            wrongThread = std::this_thread::get_id() != owner;
            if (failPreparation || expected != state)
                return Result<std::unique_ptr<IShaderHotReloadCandidate>>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            auto candidate = std::make_unique<Candidate>();
            for (const auto &program : programs)
                candidate->markers.push_back(program.compiled->artifacts.front().payload.front());
            return Result<std::unique_ptr<IShaderHotReloadCandidate>>::Success(std::move(candidate));
        }

        Result<ShaderHotReloadTargetState> Adopt(std::unique_ptr<IShaderHotReloadCandidate> candidate,
                                                 ShaderHotReloadTargetState expected) override {
            if (failAdoption || expected != state)
                return Result<ShaderHotReloadTargetState>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            ++adoptions;
            live = std::move(static_cast<Candidate &>(*candidate).markers);
            ++state.liveGeneration;
            return Result<ShaderHotReloadTargetState>::Success(state);
        }
    };

    inline ShaderHotReloadRequest Request(std::uint8_t marker = 1, unsigned count = 2) {
        ShaderHotReloadRequest request{{1, 1, 1, 1, 1}, {}};
        for (unsigned index = 0; index < count; ++index) {
            ShaderBuildRequest build;
            build.compilation = ShaderCompilerPipelineTests::ValidRequest();
            build.compilation.targets.resize(1);
            build.compilation.manifest.targets.resize(1);
            build.compilation.source = {static_cast<std::uint8_t>(marker + index)};
            build.limits.maximumPayloadBytes = 1024;
            build.limits.maximumDebugPayloadBytes = 1024;
            request.programs.push_back({ShaderHotReloadProgramId{index + 1}, std::move(build)});
        }
        return request;
    }

    struct Fixture final {
        BuildOutputStore output{128};
        OperationStore operations{8, 16};
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        std::shared_ptr<Adapter> adapter{std::make_shared<Adapter>()};
        ShaderBuildService compiler{adapter, output};
        Target target;
        ShaderHotReloadConfiguration configuration{ShaderHotReloadPolicy::Development, {1, 1, 1, 1, 1}};
        std::unique_ptr<ShaderHotReloadService> service;

        void Create() {
            auto created = ShaderHotReloadService::Create(compiler, jobs, operations, output, target, configuration);
            REQUIRE(created.HasValue());
            service = std::move(created).Value();
        }

        ShaderHotReloadSnapshot Snapshot() {
            const auto result = service->SnapshotIfChanged(0);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            return *result.Value();
        }

        void Drain() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (Snapshot().activeOperation && std::chrono::steady_clock::now() < deadline) {
                REQUIRE(service->Poll().HasValue());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE_FALSE(Snapshot().activeOperation);
            REQUIRE_FALSE(Snapshot().pendingOperation);
        }

        void AwaitCompiler() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (adapter->entered == 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            REQUIRE(adapter->entered > 0);
        }
    };
}  // namespace Horo::Application::ShaderHotReloadTests
