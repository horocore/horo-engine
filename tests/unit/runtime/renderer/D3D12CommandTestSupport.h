#pragma once
/** @file D3D12CommandTestSupport.h @brief Real WARP command fixtures; no simulated GPU completion. */
#include "D3D12NativeCommands.h"
#include "D3D12ResourceTestSupport.h"
#include "RenderGraphTestUtils.h"

#include <atomic>
#include <iostream>

namespace Horo::Render::D3D12CommandTests {
#if defined(_WIN32) && defined(_M_X64)
    using Microsoft::WRL::ComPtr;
    inline std::atomic<std::uint32_t> executeCalls{};  // Observation only, never native ownership/completion authority.

    inline void ObserveExecute(ID3D12Device *) noexcept {
        executeCalls.fetch_add(1);
    }

    inline void RemoveAfterExecute(ID3D12Device *device) noexcept {
        ObserveExecute(device);
        D3D12Tests::RemoveAfterExecute(device);
    }

#endif

    struct Lease final : IRenderGraphResourceLease {
        std::uint32_t releases{};

        void Release() noexcept override {
            ++releases;
        }
    };

    inline RenderGraphLogicalState CopyState(RenderQueueId queue, bool source) {
        return {source ? RenderGraphSynchronizationAccess::Read : RenderGraphSynchronizationAccess::Write,
                source ? RenderGraphSynchronizationOperation::CopySource : RenderGraphSynchronizationOperation::CopyDestination,
                RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::NotApplicable, queue};
    }

    inline CompiledRenderGraphExecution Compile(RenderGraphBuilder &builder, std::span<const RenderQueueAssignment> queues,
                                                std::span<const RenderGraphImportedState> imports,
                                                std::span<const RenderGraphFinalState> final = {}) {
        auto graph = Test::RequireGraph(builder);
        auto schedule = Test::RequireSchedule(graph);
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, imports, final);
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
        REQUIRE(execution.HasValue());
        return std::move(execution).Value();
    }

    /** @brief Graph-local handles are explicit test frontend metadata; instances below are actual Resources-issued tokens. */
    struct CopyPacket final {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::vector<RenderGraphResourceInstance> resources;
        std::array<RenderGraphQueueSubmission, 1> submissions;
        RenderGraphLeaseTransfer transfer;
        Lease &lease;
        bool admitsTransient{};

        static CopyPacket Create(std::uint64_t sourceInstance, std::uint64_t destinationInstance, Lease &lease, RenderQueueId queue = {1},
                                 std::uint64_t order = 1, std::uint64_t signal = 1,
                                 RenderGraphResourceClass destinationClass = RenderGraphResourceClass::Persistent,
                                 bool externalFinal = false, bool unusedTransient = false) {
            auto builder = Test::RequireBuilder({.maxPasses = 3, .maxResources = 3, .maxUsages = 3, .maxDependencies = 2});
            const auto pass = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Transfer);
            const auto source = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(1), RenderGraphResourceClass::Persistent));
            const auto destination = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(2), destinationClass));
            Test::RequireUsage(builder, {pass, source, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
            Test::RequireUsage(builder, {pass, destination, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Transfer, queue}};
            const std::array imports{RenderGraphImportedState{source, CopyState(queue, true)},
                                     RenderGraphImportedState{destination, CopyState(queue, false)}};
            auto externalState = CopyState(queue, true);
            externalState.scope = RenderGraphPipelineScope::External;
            const std::array final{RenderGraphFinalState{destination, externalState}};
            std::vector<RenderGraphResourceInstance> instances{{source, sourceInstance}, {destination, destinationInstance}};
            if (unusedTransient) {
                const auto unused = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
                instances.push_back({unused, 0});
            }
            auto execution =
                Compile(builder, queues, imports,
                        externalFinal ? std::span<const RenderGraphFinalState>{final} : std::span<const RenderGraphFinalState>{});
            const std::array workloads{RenderGraphPassWorkload{pass, RenderGraphBufferCopy{source, destination, 0, 0, 32}}};
            const std::array submissions{RenderGraphQueueSubmission{{queue, {order}, {}, {queue, signal}}, {0, 1}}};
            return {std::move(execution), workloads, std::move(instances), submissions, {}, lease, unusedTransient};
        }

        RenderGraphExecutionRequest Request() {
            return {{1}, graph, workloads, resources, &lease, admitsTransient, &transfer, submissions};
        }
    };

    struct QueuePacket final {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 2> workloads;
        std::array<RenderGraphResourceInstance, 3> resources;
        std::array<RenderTimelinePoint, 1> waits{{{{1}, 7}}};
        RenderGraphLeaseTransfer transfer;
        Lease &lease;

        static QueuePacket Create(std::array<std::uint64_t, 3> instances, Lease &lease) {
            auto builder = Test::RequireBuilder({.maxPasses = 2, .maxResources = 3, .maxUsages = 4, .maxDependencies = 2});
            const auto producer = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Graphics);
            const auto consumer = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Transfer);
            const auto source = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(1), RenderGraphResourceClass::Persistent));
            const auto middle = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(2), RenderGraphResourceClass::Persistent));
            const auto destination =
                Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(3), RenderGraphResourceClass::Persistent));
            Test::RequireUsage(builder, {producer, source, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
            Test::RequireUsage(builder, {producer, middle, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            Test::RequireUsage(builder, {consumer, middle, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
            Test::RequireUsage(builder, {consumer, destination, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}},
                                    RenderQueueAssignment{RenderQueueRole::Transfer, {3}}};
            const std::array imports{RenderGraphImportedState{source, CopyState({1}, true)},
                                     RenderGraphImportedState{middle, CopyState({1}, false)},
                                     RenderGraphImportedState{destination, CopyState({3}, false)}};
            auto execution = Compile(builder, queues, imports);
            return {std::move(execution),
                    {{{producer, RenderGraphBufferCopy{source, middle, 0, 0, 32}},
                      {consumer, RenderGraphBufferCopy{middle, destination, 0, 0, 32}}}},
                    {{{source, instances[0]}, {middle, instances[1]}, {destination, instances[2]}}},
                    {{{{1}, 7}}},
                    {},
                    lease};
        }

        RenderGraphExecutionRequest Request(std::span<const RenderGraphQueueSubmission> submissions) {
            return {{1}, graph, workloads, resources, &lease, false, &transfer, submissions};
        }
    };

#if defined(_WIN32) && defined(_M_X64)
    /** @brief An independent test gate can park real queues; it never signals a backend completion fence. */
    class QueueGate final {
    public:
        QueueGate(ID3D12Device &device, ID3D12CommandQueue &queue) {
            REQUIRE(SUCCEEDED(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))));
            REQUIRE(SUCCEEDED(queue.Wait(fence_.Get(), 1)));
        }

        ~QueueGate() {
            if (fence_)
                (void)fence_->Signal(1);
        }

        void Open() {
            REQUIRE(SUCCEEDED(fence_->Signal(1)));
        }

    private:
        ComPtr<ID3D12Fence> fence_;
    };

    class Fixture final : public D3D12Tests::Fixture {
    public:
        Fixture() {
            CreateQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, compute);
            CreateQueue(D3D12_COMMAND_LIST_TYPE_COPY, copy);
            if (debugEnabled)
                REQUIRE(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&diagnostics))));
        }

        ~Fixture() {
            if (!commands)
                return;
            (void)commands->BeginShutdown();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto progress = commands->PollDrain();
                if (progress.HasError() || progress.Value() == RenderBackendDrainProgress::Drained)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            commands.reset();  // Lease storage and the parent Resources authority are still alive here.
        }

        Result<std::unique_ptr<ID3D12Commands>> Create(D3D12BarrierMode mode = D3D12BarrierMode::Legacy, D3D12CommandLimits limits = {},
                                                       D3D12CommandTestHooks hooks = {ObserveExecute}) {
            const D3D12CommandConfiguration configuration{resources->Snapshot().deviceIncarnation, bindings, mode, limits};
            return CreateD3D12Commands(device.Get(), {queue.Get(), compute.Get(), copy.Get()}, *resources, configuration, hooks);
        }

        void Start(D3D12BarrierMode mode = D3D12BarrierMode::Legacy, D3D12CommandLimits limits = {},
                   D3D12CommandTestHooks hooks = {ObserveExecute}) {
            auto created = Create(mode, limits, hooks);
            if (created.HasError())
                UNSCOPED_INFO(created.ErrorValue().message);
            REQUIRE(created.HasValue());
            commands = std::move(created).Value();
        }

        Lease &NextLease() {
            REQUIRE(nextLease < leases.size());
            return leases[nextLease++];
        }

        RenderCpuWait PollPoint(RenderTimelinePoint point) const {
            return {point, RenderCpuWaitPurpose::Poll, {}, resources->Snapshot().deviceIncarnation};
        }

        void Await(RenderTimelinePoint point) {
            auto wait = PollPoint(point);
            wait.purpose = RenderCpuWaitPurpose::DeterministicTest;
            wait.timeout = std::chrono::seconds{10};
            const auto result = commands->Observe(wait);
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            REQUIRE(commands->Poll().HasValue());
        }

        void Drain() {
            REQUIRE(commands->BeginShutdown().HasValue());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            for (;;) {
                const auto progress = commands->PollDrain();
                REQUIRE(progress.HasValue());
                if (progress.Value() == RenderBackendDrainProgress::Drained)
                    return;
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }

        /** @brief Complete setup uploads before testing command-only retention; no readbacks are outstanding here. */
        void SettleUploads() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (resources->Snapshot().transferCount != 0) {
                REQUIRE(resources->Poll().HasValue());
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }

        void ClearDiagnostics() {
            if (diagnostics)
                diagnostics->ClearStoredMessages();
        }

        void RequireCleanDiagnostics() {
            if (!diagnostics) {
                std::cout << "D3D12 command debug-layer evidence: unavailable; native byte/fence proof remains mandatory\n";
                return;
            }
            const auto count = diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            REQUIRE(count <= 256);
            for (UINT64 index{}; index < count; ++index) {
                SIZE_T bytes{};
                REQUIRE(SUCCEEDED(diagnostics->GetMessage(index, nullptr, &bytes)));
                REQUIRE(bytes >= sizeof(D3D12_MESSAGE));
                REQUIRE(bytes <= 65536);
                std::vector<std::byte> storage(bytes);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                REQUIRE(SUCCEEDED(diagnostics->GetMessage(index, message, &bytes)));
                CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
                CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
            }
            std::cout << "D3D12 command debug-layer evidence: checked\n";
        }

        static std::array<std::byte, 32> Bytes() {
            std::array<std::byte, 32> bytes{};
            for (std::size_t index{}; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>(index * 7 + 3);
            return bytes;
        }

        ComPtr<ID3D12CommandQueue> compute, copy;
        ComPtr<ID3D12InfoQueue> diagnostics;
        std::unique_ptr<ID3D12Commands> commands;
        const std::array<D3D12CommandQueueBinding, 3> bindings{
            {{{1}, D3D12QueueKind::Direct}, {{2}, D3D12QueueKind::Compute}, {{3}, D3D12QueueKind::Copy}}};

    private:
        void CreateQueue(D3D12_COMMAND_LIST_TYPE type, ComPtr<ID3D12CommandQueue> &output) {
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = type;
            REQUIRE(SUCCEEDED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&output))));
        }

        std::array<Lease, 16> leases;
        std::size_t nextLease{};
    };
#endif
}  // namespace Horo::Render::D3D12CommandTests
