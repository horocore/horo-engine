#pragma once
/** @file D3D12CommandState.h @brief Private prepared command admission state; never a second graph policy. */
#include "D3D12CommandRuntime.h"

#include <array>
#include <thread>
#include <vector>

namespace Horo::Render::D3D12Detail {
    struct IssuedRange final {
        std::uint64_t first{};
        std::uint64_t last{};
    };

    struct CommandQueueHistory final {
        D3D12CommandQueueBinding binding;
        std::vector<IssuedRange> issued;
        std::uint64_t lastSignal{};
        std::uint64_t expiredThrough{};
        [[nodiscard]] bool Contains(std::uint64_t value) const noexcept;
    };

    /** @brief Scoped reentrancy closure; native work cannot recursively admit another batch. */
    struct CommandCall final {
        bool &active;

        explicit CommandCall(bool &value) noexcept : active(value) {
            active = true;
        }

        CommandCall(const CommandCall &) = delete;
        CommandCall &operator=(const CommandCall &) = delete;

        ~CommandCall() {
            active = false;
        }
    };

    class CommandService final : public ID3D12Commands {
    public:
        explicit CommandService(std::unique_ptr<ID3D12CommandRuntime> runtime, const D3D12CommandConfiguration &configuration);
        [[nodiscard]] Result<void> Initialize();
        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation) override;
        Result<bool> Observe(const RenderCpuWait &wait) override;
        Result<void> Poll() override;
        Result<void> BeginShutdown() override;
        Result<RenderBackendDrainProgress> PollDrain() override;

    private:
        [[nodiscard]] Result<void> Access(bool admission) const;
        [[nodiscard]] Result<void> ValidateRequest(const RenderGraphExecutionRequest &request) const;
        [[nodiscard]] Result<void> ValidateLimits(const RenderGraphExecutionRequest &request) const;
        [[nodiscard]] Result<void> ValidateBatch(std::span<const RenderGraphQueueSubmission> submissions) const;
        [[nodiscard]] Result<void> ValidateWaits(const RenderQueueSubmission &submission,
                                                 std::span<const RenderGraphQueueSubmission> earlier) const;
        [[nodiscard]] Result<void> ValidateSignal(const RenderQueueSubmission &submission,
                                                  std::span<const RenderGraphQueueSubmission> earlier) const;
        [[nodiscard]] Result<void> ValidatePoint(RenderTimelinePoint point) const;
        [[nodiscard]] Result<void> ReclaimHistory(std::span<const RenderGraphQueueSubmission> submissions);
        [[nodiscard]] Result<void> ReclaimQueueHistory(CommandQueueHistory &queue, std::span<const RenderGraphQueueSubmission> submissions);
        [[nodiscard]] Result<void> PollNative();
        [[nodiscard]] Result<void> Prepare(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation);
        [[nodiscard]] Result<void> Submit(const RenderGraphExecutionRequest &request);
        void CommitHistory(std::span<const RenderGraphQueueSubmission> submissions) noexcept;
        [[nodiscard]] CommandQueueHistory *FindQueue(RenderQueueId queue) noexcept;
        [[nodiscard]] const CommandQueueHistory *FindQueue(RenderQueueId queue) const noexcept;
        [[nodiscard]] D3D12CommandConfiguration Configuration() const noexcept;
        std::unique_ptr<ID3D12CommandRuntime> runtime_;
        std::array<D3D12CommandQueueBinding, 3> bindings_{};
        std::vector<CommandQueueHistory> queues_;
        D3D12CommandLimits limits_;
        D3D12BarrierMode mode_;
        std::uint64_t deviceIncarnation_{};
        std::uint64_t lastOrder_{};
        std::thread::id owner_{std::this_thread::get_id()};
        bool initialized_{};
        bool accepting_{};
        bool active_{};
    };

    [[nodiscard]] Result<void> ValidateCommandConfiguration(const D3D12CommandConfiguration &configuration);
}  // namespace Horo::Render::D3D12Detail
