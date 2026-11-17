#pragma once
/** @file VfxGpuGraphScheduling.h @brief Renderer-owned same-graph GPU particle scheduling and immutable view inputs. */
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderReadback.h"

#include <array>
#include <optional>

namespace Horo::Render {
    /** @brief One immutable emitter contribution; all allowances are the remaining GLOBAL scene budget.
     * @details The preparation receipt is the only simulation handle. It retains no live world pointer or native API object.
     * New work is dispatched once for this builder/frame; repeated views depend on that same compute pass.
     * The host merges Workloads/InitialStates and reserves one actual scene submission with RequiredWaits after compiling passes.
     */
    struct VfxGpuGraphRequest final {
        VfxGpuPreparationReceipt preparation;
        std::uint64_t generation{1}; /**< Last accepted generation captured in the scene snapshot; never inferred from encoding. */
        std::optional<GpuVfxSimulationWork> pending;
        std::optional<CancellationToken>
            lifetimeCancellation; /**< Required captured unit token; omission is invalid. Explicit empty is standalone ownership only. */
        VfxGpuDrawView view;
        RenderGraphResourceId color, depth;
        PrimaryOutputAttachment colorOperations;
        RenderGraphPassRef lightingComplete, outputTransform;
        std::uint32_t remainingDrawCalls{}, remainingPasses{}, remainingResources{}, remainingUsages{};
    };

    /** @brief Owner-thread observation-only facts; excludes cold seed uploads and unrelated framebuffer test readback. */
    struct VfxGpuReadbackDiagnostics final {
        RenderReadbackSnapshot queue;
        std::size_t preparedCpuBytes{}; /**< Known fixed queue/quota-index payload, not allocator/driver/RSS. */
        std::uint32_t reservedSlots{};
        std::uint64_t observationCopyCalls{}, copiedBytes{}, observationDiscardCalls{};
    };

    /** @brief Allocation-free emitter graph contribution; returned records own scalar payloads and native opaque leases. */
    struct VfxGpuGraphSchedule final {
        std::array<RenderGraphPassWorkload, 2> records;
        std::array<RenderGraphImportedState, 4> initialStates;
        std::array<RenderTimelinePoint, 1> requiredWaits;
        std::uint32_t recordCount{}, initialStateCount{}, requiredWaitCount{};
        std::uint32_t drawCalls{}, resources{}, usages{};
        bool dispatched{};
        bool observationOmitted{}; /**< An explicitly optional requested observation was omitted by unavailable/pressure admission. */

        /** @brief Borrows only inserted semantic passes. @return Stable records owned by this result. */
        [[nodiscard]] std::span<const RenderGraphPassWorkload> Workloads() const noexcept {
            return std::span{records}.first(recordCount);
        }

        /** @brief Borrows exact imported state evidence for newly imported issued buffers. @return Existing ready/preserved or discarded
         * states. */
        [[nodiscard]] std::span<const RenderGraphImportedState> InitialStates() const noexcept {
            return std::span{initialStates}.first(initialStateCount);
        }

        /** @brief Borrows actual accepted producer points required by the scene submission. @return No fabricated or encoding-only point.
         */
        [[nodiscard]] std::span<const RenderTimelinePoint> RequiredWaits() const noexcept {
            return std::span{requiredWaits}.first(requiredWaitCount);
        }
    };

    /** @brief Allocation-free expected scheduling failure; owned error projection belongs outside measured frame work. */
    class VfxGpuGraphSchedulingOutcome final {
    public:
        /** @brief Publishes a complete contribution. @return Success with fixed owned records. */
        [[nodiscard]] static VfxGpuGraphSchedulingOutcome Success(VfxGpuGraphSchedule schedule) noexcept {
            VfxGpuGraphSchedulingOutcome result;
            result.schedule_ = std::move(schedule);
            return result;
        }

        /** @brief Records a static failure without allocating. @return Exact expected failure descriptor and static context. */
        [[nodiscard]] static VfxGpuGraphSchedulingOutcome Failure(const ErrorCodeDescriptor &descriptor,
                                                                  std::string_view context = {}) noexcept {
            VfxGpuGraphSchedulingOutcome result;
            result.descriptor_ = &descriptor;
            result.context_ = context;
            return result;
        }

        /** @brief Preserves an unexpected builder/native error. @return Original owned failure. */
        [[nodiscard]] static VfxGpuGraphSchedulingOutcome Failure(Error error) noexcept {
            VfxGpuGraphSchedulingOutcome result;
            result.original_ = std::move(error);
            return result;
        }

        /** @brief Checks complete admission. @return True only after every required append succeeded. */
        [[nodiscard]] bool HasValue() const noexcept {
            return descriptor_ == nullptr && !original_;
        }

        /** @brief Checks failed admission. @return True when no partial schedule was published. */
        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        /** @brief Borrows owned schedule records. @return Complete immutable contribution. @pre HasValue(). */
        [[nodiscard]] const VfxGpuGraphSchedule &Value() const noexcept {
            return schedule_;
        }

        /** @brief Reads expected failure identity without allocating. @return Static descriptor or null for success/unexpected error. */
        [[nodiscard]] const ErrorCodeDescriptor *FailureDescriptor() const noexcept {
            return descriptor_;
        }

        /** @brief Materializes an owned diagnostic outside measured frame work. @return Exact failure. @pre HasError(). */
        [[nodiscard]] Error ErrorValue() const {
            return original_ ? *original_ : MakeError(*descriptor_, std::string{context_});
        }

    private:
        VfxGpuGraphSchedule schedule_;
        const ErrorCodeDescriptor *descriptor_{};
        std::string_view context_;
        std::optional<Error> original_;
    };
}  // namespace Horo::Render
