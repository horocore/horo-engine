#pragma once

/** @file D3D12CommandInternals.h
 * @brief Bounded native recording slots borrowing the sole resource/device context.
 */
#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @brief Fixed allocator/list slots; completion and identity are borrowed from Resources. */
    class CommandService final : public ID3D12Commands {
    public:
        /** @brief Borrows the one native context; no device identity or fence is created. */
        explicit CommandService(Resources &context) : context_(context) {}

        /** @brief Releases native slots before their borrowed completion leases, after terminal proof. */
        ~CommandService() override;
        /** @brief Prepares every bounded allocator/list slot during resource-context initialization. */
        Result<void> Initialize();
        /** @copydoc ID3D12Commands::Configure */
        Result<void> Configure(RenderQueueId queue) override;
        /** @copydoc ID3D12Commands::Begin */
        Result<D3D12CommandId> Begin() override;
        /** @copydoc ID3D12Commands::Close */
        Result<void> Close(D3D12CommandId id) override;
        /** @copydoc ID3D12Commands::Submit */
        Result<void> Submit(D3D12CommandId id, const RenderQueueSubmission &submission, IRenderGraphResourceLease *lease,
                            RenderGraphLeaseTransfer &transfer, const CancellationToken &cancellation) override;
        /** @copydoc ID3D12Commands::Poll */
        Result<bool> Poll(D3D12CommandId id) override;
        /** @copydoc ID3D12Commands::Cancel */
        Result<void> Cancel(D3D12CommandId id) override;
        /** @brief Returns the native recording-only borrow after exact identity/thread checks. */
        Result<ID3D12GraphicsCommandList *> Borrow(D3D12CommandId id);
        /** @brief Retries an accepted stream's signal before any later shared-fence submission. */
        Result<void> RetrySignals();
        /** @brief Consumes the context's real non-removed fence observation and releases completed leases once. */
        void Reclaim(std::uint64_t completed) noexcept;
        /** @brief Abandons all unsent tickets and closes observation of accepted work without early release. */
        void StopAdmission() noexcept;
        /** @brief Reports whether any native stream still requires the resource context's retirement authority. */
        [[nodiscard]] bool HasSubmitted() const noexcept;

    private:
        enum class State {
            Free,
            Recording,
            Closed,
            Submitted,
            Complete,
            Failed
        };

        struct Recording {
            D3D12CommandId id;
            State state{State::Free};
            std::uint64_t fence{0};
            bool signalPending{false};
            bool abandoned{false};
            IRenderGraphResourceLease *lease{nullptr};
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
        };

        /** @brief Validates the existing incarnation and non-reused ticket before any native query. */
        Result<Recording *> Find(D3D12CommandId id);
        /** @brief Validates exact frontend ordering, unique lease authority and the supported no-wait envelope. */
        Result<void> ValidateSubmission(const Recording &recording, const RenderQueueSubmission &submission,
                                        IRenderGraphResourceLease *lease, const RenderGraphLeaseTransfer &transfer,
                                        const CancellationToken &cancellation) const;
        /** @brief Clears the borrowed pointer before its idempotent canonical release callback. */
        static void ReleaseLease(Recording &recording) noexcept;
        /** @brief Signals the context fence; only explicit test hooks may inject a skipped E_FAIL attempt. */
        HRESULT Signal(const Recording &recording) noexcept;
        Resources &context_;
        std::vector<Recording> recordings_;
        RenderQueueId queue_;
        std::uint64_t nextId_{1};
        std::uint64_t lastOrder_{0};
        std::uint64_t lastSignal_{0};
    };
}  // namespace Horo::Render::D3D12Detail
#endif
