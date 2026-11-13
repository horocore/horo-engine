#pragma once
/** @file ShaderHotReloadService.h @brief Bounded asynchronous compilation and owner-bound atomic program-set adoption. */
#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Application {
    enum class ShaderHotReloadPolicy : std::uint8_t {
        Disabled,
        Development,
        Packaged
    };

    /** @brief Explicit host publication fence, never inferred from paths or compiler completion.
     * sourceGeneration is the accepted source-request occurrence, not a fabricated persisted source revision
     * or a promise of instantaneous disk freshness. Manifest revisions and content digests remain canonical.
     */
    struct ShaderHotReloadFence final {
        std::uint64_t projectGeneration{}, configurationRevision{}, lifecycleGeneration{}, sourceGeneration{}, deviceIncarnation{};
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const ShaderHotReloadFence &) const noexcept = default;
    };

    /** @brief One required stable host program role and its complete immutable compiler input. */
    struct ShaderHotReloadProgram final {
        Render::ShaderHotReloadProgramId id;
        ShaderBuildRequest build;
    };

    /** @brief Complete atomic program set; every required role must compile and prepare before adoption. */
    struct ShaderHotReloadRequest final {
        ShaderHotReloadFence fence;
        std::vector<ShaderHotReloadProgram> programs;
    };

    struct ShaderHotReloadConfiguration final {
        ShaderHotReloadPolicy policy{ShaderHotReloadPolicy::Disabled};
        ShaderHotReloadFence context;
        std::size_t maximumInputBytes{64U * 1024U * 1024U};     /**< Aggregate owned input across all at most eight programs. */
        std::size_t maximumCompiledBytes{128U * 1024U * 1024U}; /**< Aggregate compiler-output allowance, not per program. */
    };

    enum class ShaderHotReloadOutcome : std::uint8_t {
        Adopted,
        Failed,
        Cancelled,
        Stale
    };

    struct ShaderHotReloadAdmission final {
        OperationId operation{};
        std::uint64_t requestGeneration{};
    };

    struct ShaderHotReloadReceipt final {
        OperationId operation{};
        std::optional<JobId> job;
        std::uint64_t requestGeneration{};
        ShaderHotReloadFence fence;
        ShaderHotReloadOutcome outcome{ShaderHotReloadOutcome::Failed};
        std::optional<Render::ShaderHotReloadTargetState> adopted;
        std::optional<Error> error;
    };

    /** @brief Small owned presentation view; diagnostics remain in the sole BuildOutputStore. */
    struct ShaderHotReloadSnapshot final {
        std::uint64_t revision{};
        ShaderHotReloadFence context;
        bool closed{};
        std::optional<OperationId> activeOperation, pendingOperation;
        std::optional<JobId> activeJob;
        std::optional<ShaderHotReloadReceipt> lastResult, lastGood;
    };

    namespace ShaderHotReloadErrors {
        extern const ErrorCodeDescriptor Disabled, Packaged, WrongThread, Busy, Closed, InvalidRequest, Capacity, Stale, TargetFailure;
    }

    /** @brief One selected target's asynchronous owner over existing compiler, scheduler and operation authorities.
     * At most one compiler job, one latest pending request and one completion are retained. Requests are
     * canonically validated and aggregate-budgeted before copying. A new accepted request cancels older
     * work; no worker borrows this service, target, renderer, project document or filesystem provider.
     * CPU compilation sequentially builds the complete required program set using ShaderBuildService.
     * Poll alone prepares/adopts the complete set on the target owner, outside an active frame. It performs
     * no filesystem I/O or job wait. Failure, cancellation and stale results preserve the target's last good state.
     * Borrowed jobs/compiler/stores/target must outlive Shutdown and service destruction. The actual compiler
     * adapter must honor cancellation and finite process deadlines; teardown never detaches admitted work.
     */
    class ShaderHotReloadService final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<ShaderHotReloadService>> Create(ShaderBuildService &compiler, const JobSystem &jobs,
                                                                                    OperationStore &operations, BuildOutputStore &output,
                                                                                    Render::IShaderHotReloadTarget &target,
                                                                                    const ShaderHotReloadConfiguration &configuration);
        ~ShaderHotReloadService();
        ShaderHotReloadService(const ShaderHotReloadService &) = delete;
        ShaderHotReloadService &operator=(const ShaderHotReloadService &) = delete;
        /** @brief Changes explicit host scope and cancels obsolete work before replacement capture can complete. */
        [[nodiscard]] Result<void> SetContext(const ShaderHotReloadFence &context);
        /** @brief Validates and owns the full input before superseding work; never compiles on the calling owner. */
        [[nodiscard]] Result<ShaderHotReloadAdmission> Request(const ShaderHotReloadRequest &request,
                                                               const CancellationToken &cancellation = {});
        /** @brief Processes at most one completion and starts at most one latest pending job at the graphics safe point. */
        [[nodiscard]] Result<void> Poll();
        /** @brief Requests cancellation of an exact active/pending operation without waiting. */
        [[nodiscard]] Result<void> Cancel(OperationId operation);
        [[nodiscard]] Result<std::optional<ShaderHotReloadSnapshot>> SnapshotIfChanged(std::uint64_t knownRevision) const;
        /** @brief Teardown-only close/cancel/join; called on the owner outside callbacks, never ordinary frame polling. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct State;
        explicit ShaderHotReloadService(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Application
