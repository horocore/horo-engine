#pragma once
#include "D3D12NativeCommandBarriers.h"
#include "D3D12NativeCommands.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @brief Shared only across one accepted graph's submissions; frontend lease releases exactly once after the last native pin. */
    struct CommandGraphLease final {
        IRenderGraphResourceLease *lease{};
        bool owned{};

        void Retire() noexcept {
            auto *captured = lease;
            const bool release = owned;
            owned = false;
            lease = nullptr;
            if (release && captured)
                captured->Release();
        }

        ~CommandGraphLease() {
            Retire();
        }
    };

    /** @brief One bounded allocator/list pair, never reset while native execution can still reference it. */
    struct CommandSlot final {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12GraphicsCommandList7> enhanced;
        std::uint64_t signal{};
        bool submitted{};
        bool signalPending{};
        std::vector<RenderTimelinePoint> waits;
        std::vector<std::shared_ptr<Resource>> pins;
        std::vector<std::size_t> accessed;
        std::shared_ptr<CommandGraphLease> graphLease;
    };

    struct CommandObservationEventCloser final {
        void operator()(void *event) const noexcept {
            if (event)
                CloseHandle(event);
        }
    };

    struct CommandQueue final {
        RenderQueueId identity;
        D3D12QueueKind kind{D3D12QueueKind::Direct};
        ComPtr<ID3D12CommandQueue> native;
        ComPtr<ID3D12Fence> fence;
        std::unique_ptr<void, CommandObservationEventCloser> observationEvent;
        std::uint64_t issued{};
        std::vector<CommandSlot> slots;
    };

    struct PreparedCommandSlot final {
        std::size_t queue{};
        std::size_t slot{};
    };

    /** @brief Native implementation of the sole portable owner; no frontend/UI borrow survives synchronous preparation. */
    class NativeCommandRuntime final : public ID3D12CommandRuntime {
    public:
        NativeCommandRuntime(ID3D12Device *device, const std::array<ID3D12CommandQueue *, 3> &queues, Resources &resources,
                             D3D12CommandTestHooks hooks);
        ~NativeCommandRuntime() override;
        Result<void> Initialize(const D3D12CommandConfiguration &configuration) override;
        Result<void> PrepareGraph(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation) override;
        Result<void> SubmitPrepared(IRenderGraphResourceLease *lease, RenderGraphLeaseTransfer *transfer) override;
        void AbortPrepared() noexcept override;
        Result<bool> Observe(const RenderCpuWait &wait) override;
        Result<void> Poll() override;
        void StopAdmission() noexcept override;
        Result<RenderBackendDrainProgress> PollDrain() override;

    private:
        Result<void> CheckDevice() const;
        Result<void> ClaimResourceOwner(const D3D12CommandConfiguration &configuration);
        void ReserveStorage(std::size_t queueCount);
        Result<void> InitializeQueue(CommandQueue &queue, std::uint32_t slots);
        Result<void> InitializeSlot(CommandQueue &queue, CommandSlot &slot);
        Result<void> CheckGraphAdmission(const RenderGraphExecutionRequest &request);
        Result<void> AdmitGraphResources(const RenderGraphExecutionRequest &request);
        Result<void> AdmitDistinctResource(const std::shared_ptr<Resource> &resource) const;
        Result<void> AcquireGraphLease();
        Result<void> BindResources(const RenderGraphExecutionRequest &request);
        Result<void> ValidateResourceBoundaries(const RenderGraphExecutionRequest &request) const;
        Result<void> ValidatePassStates(const RenderGraphExecutionRequest &request) const;
        Result<void> ValidateWorkloads(const RenderGraphExecutionRequest &request) const;
        Result<void> PrepareSubmission(const RenderGraphExecutionRequest &request, const RenderGraphQueueSubmission &submission);
        Result<void> RecordSubmission(const RenderGraphExecutionRequest &request, const RenderGraphQueueSubmission &submission,
                                      CommandQueue &queue, CommandSlot &slot);
        Result<void> RecordPass(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass, CommandQueue &queue,
                                CommandSlot &slot, std::vector<RenderGraphLogicalState> &nativeStates);
        Result<void> RecordCopy(const RenderGraphBufferCopy &copy, ID3D12GraphicsCommandList &list);
        Result<void> SubmitSlot(CommandQueue &queue, CommandSlot &slot);
        Result<void> WaitForDependencies(CommandQueue &queue, const CommandSlot &slot);
        Result<void> WaitForTransferAccess(CommandQueue &queue);
        Result<void> WaitForPriorResourceAccess(CommandQueue &queue, const CommandSlot &slot);
        Result<void> SignalSlot(CommandQueue &queue, CommandSlot &slot);
        Result<void> PollQueue(CommandQueue &queue);
        void ClearSlot(CommandSlot &slot) noexcept;
        CommandQueue *FindQueue(RenderQueueId identity) noexcept;
        const CommandQueue *FindQueue(RenderQueueId identity) const noexcept;
        std::size_t ResourceIndex(RenderGraphResourceId identity) const noexcept;

        ComPtr<ID3D12Device> device_;
        std::array<ComPtr<ID3D12CommandQueue>, 3> nativeQueues_;
        Resources &resources_;
        D3D12CommandTestHooks hooks_;
        D3D12CommandLimits limits_;
        D3D12BarrierMode mode_{D3D12BarrierMode::Legacy};
        std::uint64_t incarnation_{};
        bool accepting_{true};
        std::vector<CommandQueue> queues_;
        std::vector<PreparedCommandSlot> prepared_;
        std::vector<std::shared_ptr<Resource>> boundResources_;
        std::vector<RenderGraphResourceId> resourceIds_;
        std::vector<RenderGraphLogicalState> logicalStates_;
        std::vector<bool> activationRecorded_;
        std::vector<RenderGraphLogicalState> nativeStates_;
        std::vector<std::shared_ptr<CommandGraphLease>> leaseGroups_;
        std::shared_ptr<CommandGraphLease> preparedLease_;
    };

    D3D12_COMMAND_LIST_TYPE CommandListType(D3D12QueueKind kind) noexcept;
}  // namespace Horo::Render::D3D12Detail
#endif
