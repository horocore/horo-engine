#include "Horo/Application/ShaderBuildService.h"
#include "support/ShaderCompilerToolchainFixture.h"

using namespace Horo::Tests::ShaderCompilerFixture;

namespace {
    Application::ShaderBuildRequest BuildRequest(const TemporaryDirectory &temporary) {
        Application::ShaderBuildRequest request;
        request.compilation = Request(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
                                      {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::DxilValidator)});
        request.sources = {{request.compilation.manifest.sourceIdentity, temporary.Path() / "original shader.hlsl"},
                           {"include/portable_constants.hlsli", temporary.Path() / "original include.hlsli"}};
        request.operationId = 37;
        return request;
    }

    std::shared_ptr<const IShaderCompilerAdapter> BuildAdapter(const TemporaryDirectory &temporary,
                                                               const std::shared_ptr<FixtureProcessRunner> &processes) {
        auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
        REQUIRE(adapter.HasValue());
        return std::make_shared<ExternalShaderCompilerAdapter>(std::move(adapter).Value());
    }

    std::future<Result<ShaderCompilationBatch>> CompileWorker(Application::ShaderBuildService &service,
                                                              Application::ShaderBuildRequest request,
                                                              CancellationToken cancellation = {}) {
        return std::async(std::launch::async, [&service, request = std::move(request), cancellation]() mutable {
            return service.Compile(std::move(request), cancellation);
        });
    }

    void RequireTerminal(const BuildOutputSnapshot &snapshot, const BuildOutputResult result) {
        CHECK(std::ranges::count_if(snapshot.records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
        REQUIRE_FALSE(snapshot.records.empty());
        CHECK(snapshot.records.back().result == result);
        const auto session = snapshot.records.back().sessionId;
        REQUIRE(session);
        CHECK(session->IsValid());
        for (const auto &record : snapshot.records) {
            CHECK(record.timestampUtc.time_since_epoch() > std::chrono::system_clock::duration::zero());
            CHECK(record.sessionId == session);
            CHECK(record.operationId == 37);
        }
    }
}  // namespace

TEST_CASE("Production shader diagnostics reach the host output store with navigable warnings", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::Warning);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasValue());
    auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Succeeded);
    const auto warning = std::ranges::find_if(snapshot->records, [](const auto &record) {
        return record.toolCode == "W1234";
    });
    REQUIRE(warning != snapshot->records.end());
    CHECK(warning->severity == DiagnosticSeverity::Warning);
    CHECK(warning->result == BuildOutputResult::None);
    CHECK(warning->code.Value() == "render.shader_compiler.source");
    REQUIRE(warning->source);
    CHECK(warning->source->absolutePath == (temporary.Path() / "original shader.hlsl").string());
    CHECK(warning->source->line == 2);
    CHECK(warning->source->column == 3);
    CHECK(warning->stage.find("shader/compile") != std::string::npos);
    CHECK(warning->stage.find("entry=VertexMain") != std::string::npos);
    CHECK(warning->stage.find("revision=1") != std::string::npos);
    CHECK(warning->stage.find("build=sha256:") != std::string::npos);
    CHECK(warning->message.find(temporary.Path().string()) == std::string::npos);
}

TEST_CASE("Shader failures retain native codes include locations and one terminal", "[shader][build_output]") {
    TemporaryDirectory temporary;
    const auto mode = GENERATE(FixtureProcessRunner::Output::Failure, FixtureProcessRunner::Output::IncludeFailure,
                               FixtureProcessRunner::Output::Timeout);
    auto processes = std::make_shared<FixtureProcessRunner>(mode);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, mode == FixtureProcessRunner::Output::Timeout ? BuildOutputResult::TimedOut : BuildOutputResult::Failed);
    if (mode != FixtureProcessRunner::Output::Timeout) {
        const auto diagnostic = std::ranges::find_if(snapshot->records, [](const auto &record) {
            return record.toolCode == "X3000";
        });
        REQUIRE(diagnostic != snapshot->records.end());
        CHECK(diagnostic->severity == DiagnosticSeverity::Error);
        REQUIRE(diagnostic->source);
        CHECK(diagnostic->source->absolutePath ==
              (temporary.Path() / (mode == FixtureProcessRunner::Output::Failure ? "original shader.hlsl" : "original include.hlsli"))
                  .string());
        CHECK(diagnostic->source->line == 2);
        CHECK(diagnostic->source->column == 3);
        CHECK(diagnostic->code.Value() ==
              (mode == FixtureProcessRunner::Output::Failure ? "render.shader_compiler.source" : "render.shader_compiler.include"));
    }
}

TEST_CASE("Shader output limits are visible and unmapped diagnostics remain non-navigable", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::Flood);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    auto request = BuildRequest(temporary);
    request.sources.clear();
    request.limits.maximumDiagnosticsPerTarget = 2;
    request.limits.maximumDiagnosticMessageBytes = 64;
    auto result = CompileWorker(service, std::move(request)).get();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().artifacts.front().diagnostics.size() == 2);
    CHECK(result.Value().artifacts.front().diagnostics.back().truncated);
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Succeeded);
    CHECK(std::ranges::any_of(snapshot->records, [](const auto &record) {
        return record.message.find("truncated") != std::string::npos;
    }));
    CHECK(std::ranges::none_of(snapshot->records, [](const auto &record) {
        return record.source.has_value();
    }));
}

TEST_CASE("Shader producer enforces owner affinity typed missing tools and source admission", "[shader][build_output]") {
    TemporaryDirectory temporary;
    BuildOutputStore output{128};
    Application::ShaderBuildService service{{}, output};
    RequireError(service.Compile(BuildRequest(temporary)), Application::ShaderBuildErrors::OwnerThread);
    CHECK_FALSE(output.SnapshotIfChanged(0));
    auto missing = CompileWorker(service, BuildRequest(temporary)).get();
    RequireError(missing, ShaderCompilerPipelineErrors::ToolMissing);
    auto request = BuildRequest(temporary);
    request.sources.front().absolutePath = "relative shader.hlsl";
    RequireError(CompileWorker(service, std::move(request)).get(), Application::ShaderBuildErrors::InvalidSources);
    service.Shutdown();
    RequireError(CompileWorker(service, BuildRequest(temporary)).get(), Application::ShaderBuildErrors::Closed);
}

TEST_CASE("Shader cancellation stays live and shutdown drains concurrent compiler calls", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::WaitForCancellation);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    CancellationSource parent;
    auto first = CompileWorker(service, BuildRequest(temporary), parent.Token());
    auto secondRequest = BuildRequest(temporary);
    secondRequest.operationId = 38;
    auto second = CompileWorker(service, std::move(secondRequest));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (processes->started < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(processes->started == 2);
    parent.RequestCancellation();
    REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(first.get().HasError());
    service.Shutdown();
    REQUIRE(second.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(second.get().HasError());
    service.Shutdown();
    auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    std::vector<BuildOutputSessionId> sessions;
    for (const auto &record : snapshot->records) {
        if (record.result != BuildOutputResult::None) {
            CHECK(record.result == BuildOutputResult::Cancelled);
            REQUIRE(record.sessionId);
            sessions.push_back(*record.sessionId);
        }
    }
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0] != sessions[1]);
    CHECK(snapshot->records.front().operationId != snapshot->records.back().operationId);
}

TEST_CASE("Shader producer rejects excess concurrent admission without allocating an output session", "[shader][build_output]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>(FixtureProcessRunner::Output::WaitForCancellation);
    BuildOutputStore output{128};
    Application::ShaderBuildService service{BuildAdapter(temporary, processes), output};
    std::vector<std::future<Result<ShaderCompilationBatch>>> workers;
    for (std::size_t index = 0; index < 8; ++index)
        workers.push_back(CompileWorker(service, BuildRequest(temporary)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (processes->started < 8 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(processes->started == 8);
    RequireError(CompileWorker(service, BuildRequest(temporary)).get(), Application::ShaderBuildErrors::Capacity);
    service.Shutdown();
    for (auto &worker : workers)
        REQUIRE(worker.get().HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    CHECK(std::ranges::count_if(snapshot->records, [](const auto &record) {
        return record.result != BuildOutputResult::None;
    }) == 8);
}

TEST_CASE("Shader adapter allocation failure produces one failed terminal and releases admission", "[shader][build_output]") {
    class AllocatingAdapter final : public IShaderCompilerAdapter {
    public:
        Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &, const CancellationToken &) const override {
            throw std::bad_alloc{};
        }
    };

    TemporaryDirectory temporary;
    BuildOutputStore output{128};
    Application::ShaderBuildService service{std::make_shared<AllocatingAdapter>(), output};
    auto result = CompileWorker(service, BuildRequest(temporary)).get();
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::AllocationFailed.domain,
                             ShaderCompilerPipelineErrors::AllocationFailed.code));
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Failed);
    service.Shutdown();
}

#ifndef _WIN32
TEST_CASE("Shader producer shutdown cancels and joins an actual native compiler process", "[shader][build_output][process]") {
    TemporaryDirectory temporary;
    auto configuration = FixtureConfiguration(temporary);
    auto &tool = configuration.tools.front();
    {
        std::ofstream executable(tool.executable, std::ios::binary | std::ios::trunc);
        executable << "#!/bin/sh\nprintf started > process-started\nexec /bin/sleep 30\n";
    }
    std::filesystem::permissions(tool.executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    const auto bytes = Read(tool.executable);
    tool.executableDigest = ComputeSha256(std::as_bytes(std::span{bytes}));
    tool.identity.buildDigest = tool.executableDigest;
    configuration.processTimeout = std::chrono::seconds{5};
    auto request = BuildRequest(temporary);
    request.compilation.targets.front().tools.front() = tool.identity;
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());
    BuildOutputStore output{128};
    Application::ShaderBuildService service{std::make_shared<ExternalShaderCompilerAdapter>(std::move(adapter).Value()), output};
    auto worker = CompileWorker(service, std::move(request));
    bool started = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!started && std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        std::filesystem::recursive_directory_iterator entries{temporary.Path(), error};
        const std::filesystem::recursive_directory_iterator end;
        while (!error && entries != end) {
            if (entries->path().filename() == "process-started")
                started = true;
            entries.increment(error);
        }
        if (!started)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(started);
    service.Shutdown();
    REQUIRE(worker.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(worker.get().HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    RequireTerminal(*snapshot, BuildOutputResult::Cancelled);
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}
#endif
