#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderParallelTestGeometry.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
    /** @brief Captures the actual synchronous executor borrow on the host owner thread. */
    class SnapshotExecutor final : public Horo::Render::IStaticMeshPassExecutor {
    public:
        Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &pass) override {
            calledOn = std::this_thread::get_id();
            ++calls;
            const auto &mesh = pass.scene.meshResources.front();
            vertices.assign(mesh.vertices.begin(), mesh.vertices.end());
            indices.assign(mesh.indices.begin(), mesh.indices.end());
            material = pass.scene.instances.front().material.value;
            return Horo::Result<void>::Success();
        }

        std::thread::id calledOn;
        std::size_t calls{};
        std::vector<Horo::Render::MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::string material;
    };

    [[nodiscard]] std::unique_ptr<Horo::Render::RenderFrontend> CreateFrontend() {
        Horo::Render::RenderBackendRegistry registry;
        REQUIRE(Horo::Render::RegisterNullRenderBackend(registry).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = Horo::Render::RenderFrontend::Create(registry, Horo::Render::RenderBackendId{"null"}, {});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] Horo::Render::RenderFrameScope BeginTestFrame(Horo::Render::RenderFrontend &frontend) {
        auto begun = frontend.BeginFrame({.frameNumber = 1, .outputExtent = {32, 32}});
        REQUIRE(begun.HasValue());
        return std::move(begun).Value();
    }

    /** @brief Ensures assertion failure cannot strand a real worker in the source-lifetime gate. */
    struct WorkerGate final {
        std::shared_ptr<std::atomic<bool>> released{std::make_shared<std::atomic<bool>>(false)};

        ~WorkerGate() {
            released->store(true);
        }

        [[nodiscard]] Horo::Result<Horo::JobHandle> Admit(Horo::JobSystem &jobs) const {
            return jobs.SubmitResult({}, [released = released](const Horo::CancellationToken &cancellation) {
                while (!released->load()) {
                    if (cancellation.IsCancellationRequested())
                        return Horo::JobCancelled();
                    std::this_thread::yield();
                }
                return Horo::Result<void>::Success();
            });
        }
    };

    [[nodiscard]] Horo::Result<Horo::Render::RenderParallelExecutionProgress> PollUntilTerminal(Horo::Render::RenderFrameScope &frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            auto progress = frame.PollParallelExecution();
            if (progress.HasError() || progress.Value() != Horo::Render::RenderParallelExecutionProgress::Pending)
                return progress;
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
}  // namespace

TEST_CASE("Parallel frame freezes source geometry and material bytes before real worker recording",
          "[unit][renderer][parallel][lifetime]") {
    Horo::JobSystem jobs{{.workerCount = 1}};
    WorkerGate gate;
    REQUIRE(gate.Admit(jobs).HasValue());
    SnapshotExecutor executor;
    auto frontend = CreateFrontend();
    REQUIRE(frontend->AttachStaticMeshPassExecutor(executor).HasValue());
    auto target = frontend->CreateOffscreenTarget({32, 32});
    REQUIRE(target.HasValue());
    auto frame = BeginTestFrame(*frontend);

    {
        const Horo::Render::RenderMeshSourceHandle handle{{7}, 3};
        auto vertices = Horo::Render::Test::MakeParallelTriangle();
        std::vector<std::uint32_t> indices{0, 1, 2};
        std::string material = "material-owned-by-destroyed-producer-not-the-frame";
        const std::array resources{Horo::Render::RenderMeshResourceView{.handle = handle,
                                                                        .vertices = vertices,
                                                                        .indices = indices,
                                                                        .localBounds = {{0, 0, 0}, {1, 1, 0}}}};
        const std::array instances{Horo::Render::RenderStaticMeshInstance{.mesh = handle, .material = {material}}};
        const std::array passes{
            Horo::Render::RenderPassDescriptor{.id = {4},
                                               .staticMesh = Horo::Render::StaticMeshPassDescriptor{.target = target.Value(),
                                                                                                    .extent = {32, 32},
                                                                                                    .scene = {.meshResources = resources,
                                                                                                              .instances = instances}}}};
        REQUIRE(frame.PrepareParallelExecution(jobs, passes).HasValue());
        REQUIRE(frame.PollParallelExecution().Value() == Horo::Render::RenderParallelExecutionProgress::Pending);
        vertices[0].position.x = 999;
        indices[0] = 99;
        material.assign(material.size(), 'x');
    }

    auto moved = std::move(frame);
    CHECK(frame.PollParallelExecution().HasError());
    gate.released->store(true);
    REQUIRE(PollUntilTerminal(moved).HasValue());
    CHECK(executor.calledOn == std::this_thread::get_id());
    CHECK(executor.calls == 1);
    REQUIRE(executor.vertices.size() == 3);
    CHECK(executor.vertices.front().position.x == 0);
    CHECK(executor.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(executor.material == "material-owned-by-destroyed-producer-not-the-frame");
    REQUIRE(moved.Present().HasValue());
    jobs.Shutdown(Horo::ShutdownPolicy::Drain);
}

TEST_CASE("Parallel input capacity and duplicate identity fail before frame execution", "[unit][renderer][parallel][boundary]") {
    Horo::JobSystem jobs;
    auto frontend = CreateFrontend();
    auto frame = BeginTestFrame(*frontend);
    const std::array passes{Horo::Render::RenderPassDescriptor{.id = {1}}, Horo::Render::RenderPassDescriptor{.id = {1}}};
    const auto capacity = frame.PrepareParallelExecution(jobs, passes, {.maximumPasses = 1});
    REQUIRE(capacity.HasError());
    CHECK(capacity.ErrorValue().code.Value() == "render.parallel_work.capacity_exceeded");
    const auto duplicate = frame.PrepareParallelExecution(jobs, passes);
    REQUIRE(duplicate.HasError());
    CHECK(duplicate.ErrorValue().code.Value() == "render.parallel_work.invalid_pass");
    CHECK(frame.Present().HasError());
    REQUIRE(frame.Execute({}).HasValue());
    REQUIRE(frame.Present().HasValue());
}

TEST_CASE("Cancellation before publication aborts the live host frame without waiting for its worker",
          "[unit][renderer][parallel][cancellation]") {
    Horo::JobSystem jobs{{.workerCount = 1}};
    WorkerGate gate;
    REQUIRE(gate.Admit(jobs).HasValue());
    auto frontend = CreateFrontend();
    auto frame = BeginTestFrame(*frontend);
    Horo::CancellationSource cancellation;
    const std::array passes{Horo::Render::RenderPassDescriptor{.id = {1}}};
    REQUIRE(frame.PrepareParallelExecution(jobs, passes, {}, cancellation.Token()).HasValue());
    cancellation.RequestCancellation();
    const auto cancelled = frame.PollParallelExecution();
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == "render.parallel_work.cancelled");
    CHECK(frame.Present().HasError());
    REQUIRE(frontend->SubmitFrame({.frameNumber = 2, .outputExtent = {32, 32}}, {}).HasValue());
    frontend.reset();
    jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
}

TEST_CASE("Frontend teardown invalidates parallel publication while the host scheduler owns pending work",
          "[unit][renderer][parallel][shutdown]") {
    Horo::JobSystem jobs{{.workerCount = 1}};
    WorkerGate gate;
    REQUIRE(gate.Admit(jobs).HasValue());
    auto frontend = CreateFrontend();
    auto frame = BeginTestFrame(*frontend);
    const std::array passes{Horo::Render::RenderPassDescriptor{.id = {1}}};
    REQUIRE(frame.PrepareParallelExecution(jobs, passes).HasValue());
    frontend.reset();
    CHECK(frame.PollParallelExecution().HasError());
    CHECK(frame.Present().HasError());
    jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
}
