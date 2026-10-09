#pragma once

#include "Horo/Foundation/JobSystem.h"
#include "RenderFrameInputInternal.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Render::Detail {
    enum class RenderWorkProgress : std::uint8_t {
        Pending,
        Ready,
    };

    /** @brief Owner-held scheduler records for native capsules; callbacks never borrow the owner/controller. */
    class RenderParallelGraphWorkState final {
        struct ConstructionKey final {};

    public:
        /** @brief Admits one bounded host job per native record, cancelling accepted siblings on failure. */
        [[nodiscard]] static Result<std::unique_ptr<RenderParallelGraphWorkState>> Start(
            const JobSystem &jobs, std::shared_ptr<IRenderParallelGraphRecording> recording, const CancellationToken &cancellation);
        /** @brief Constructs through an unforgeable internal admission key; permits make_unique ownership. */
        RenderParallelGraphWorkState(ConstructionKey, std::shared_ptr<IRenderParallelGraphRecording> recording,
                                     const CancellationToken &cancellation);
        ~RenderParallelGraphWorkState();
        RenderParallelGraphWorkState(const RenderParallelGraphWorkState &) = delete;
        RenderParallelGraphWorkState &operator=(const RenderParallelGraphWorkState &) = delete;
        RenderParallelGraphWorkState(RenderParallelGraphWorkState &&) = delete;
        RenderParallelGraphWorkState &operator=(RenderParallelGraphWorkState &&) = delete;
        /** @brief Observes durable records in canonical order, with no scheduler wait or pump. */
        [[nodiscard]] Result<RenderWorkProgress> Poll() const;
        /** @brief Borrows the exact recording for owner-only backend acceptance. */
        [[nodiscard]] const std::shared_ptr<IRenderParallelGraphRecording> &Recording() const noexcept;

    private:
        std::shared_ptr<IRenderParallelGraphRecording> recording_;
        CancellationSource cancellation_;
        std::vector<JobHandle> jobs_;
    };

    /**
     * @brief Owner-thread controller for bounded CPU command recording on the host's worker pool.
     *
     * Workers retain only immutable input and disjoint output-slot leases, never this controller,
     * a frontend, an executor, or a backend. Destroying the controller closes publication and
     * cancels callbacks without joining on the render owner. Outstanding callback storage remains
     * owned until the host scheduler acknowledges cancellation or finishes that callback.
     * Native submission and GPU completion are deliberately not inferred from Ready.
     */
    class RenderParallelWorkState final {
        struct ConstructionKey final {};

    public:
        /** @brief Captures and admits ordered pass jobs; partial admission failure cancels accepted siblings. */
        [[nodiscard]] static Result<std::unique_ptr<RenderParallelWorkState>> Start(JobSystem &jobs, FrameToken frame,
                                                                                    std::span<const RenderPassDescriptor> passes,
                                                                                    const RenderFrameInputLimits &limits,
                                                                                    const CancellationToken &parentCancellation);
        /** @brief Constructs through internal capture admission only, with standard unique ownership. */
        RenderParallelWorkState(ConstructionKey, std::shared_ptr<const CapturedRenderFrame> inputs,
                                const CancellationToken &parentCancellation);

        ~RenderParallelWorkState();
        RenderParallelWorkState(const RenderParallelWorkState &) = delete;
        RenderParallelWorkState &operator=(const RenderParallelWorkState &) = delete;
        RenderParallelWorkState(RenderParallelWorkState &&) = delete;
        RenderParallelWorkState &operator=(RenderParallelWorkState &&) = delete;

        /** @brief Checks at most the admitted job count without waiting or pumping scheduler work. */
        [[nodiscard]] Result<RenderWorkProgress> Poll();
        /** @brief Closes publication and requests cooperative cancellation without a normal-frame join. */
        void Cancel() noexcept;
        /** @brief Borrows canonical commands only after Ready; storage remains owned by this controller. */
        [[nodiscard]] std::span<const RenderPassDescriptor> Commands() const noexcept;
        /** @brief Returns the captured active-frame generation to revalidate at owner publication. */
        [[nodiscard]] FrameToken Frame() const noexcept;

    private:
        [[nodiscard]] Result<void> Admit(const JobSystem &jobs);

        std::shared_ptr<const CapturedRenderFrame> inputs_;
        std::shared_ptr<std::vector<RenderPassDescriptor>> commands_;
        CancellationSource cancellation_;
        std::vector<JobHandle> jobs_;
        bool ready_{};
        bool closed_{};
    };
}  // namespace Horo::Render::Detail
