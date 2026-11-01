#pragma once

/** @file ShaderHotReloadTarget.h
 * @brief Backend-neutral owner-thread preparation and atomic adoption of complete shader consumers.
 */

#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

namespace Horo::Render {
    /** @brief Strong nonzero identity of one program in the finite consumer family. */
    struct ShaderHotReloadProgramId final {
        std::uint32_t value{};
        [[nodiscard]] auto operator<=>(const ShaderHotReloadProgramId &) const noexcept = default;
    };

    /** @brief Synchronous immutable borrow of one complete compiled program. */
    struct ShaderHotReloadProgramView final {
        ShaderHotReloadProgramId id;
        const ShaderManifest *manifest{};
        const ShaderCompilationBatch *compiled{};
    };

    /** @brief Exact live consumer incarnation captured before dispatching immutable source work.
     * @details Device and generation identities are issued by the existing selected renderer owner.
     * Neither a frame number nor a compiler operation number proves that a consumer is still current.
     */
    struct ShaderHotReloadTargetState final {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        std::uint64_t deviceIncarnation{};
        std::uint64_t liveGeneration{};
        [[nodiscard]] bool operator==(const ShaderHotReloadTargetState &) const noexcept = default;
    };

    /** @brief Opaque complete unpublished program generation, owned and destroyed on its graphics owner.
     * @details Implementations retain all candidate native objects, reflected layouts and dependent bindings.
     * Destruction rolls back only the candidate; it cannot change the currently active program. The target,
     * renderer and graphics context must outlive the candidate. No native object crosses this interface.
     */
    class IShaderHotReloadCandidate {
    public:
        virtual ~IShaderHotReloadCandidate() = default;
    };

    /** @brief Actual selected shader consumer, not an independent pipeline registry or compiler owner.
     * @details Calls and candidate destruction are serialized on the render-capable owner thread outside
     * an active frame. Preparation may allocate and realize native objects at this cold boundary. Adoption
     * validates identity and reserves all retirement storage before changing live state; after the first
     * live-state mutation it performs only nonthrowing ownership swaps. Old accepted work retains its
     * complete program/binding generation through the existing backend's GPU retirement authority.
     */
    class IShaderHotReloadTarget {
    public:
        virtual ~IShaderHotReloadTarget() = default;
        /** @brief Captures the selected live consumer without mutating it.
         * @return Exact backend, device incarnation and live generation, or typed unavailable/thread failure.
         */
        [[nodiscard]] virtual Result<ShaderHotReloadTargetState> CaptureState() const = 0;
        /** @brief Prepares one complete unpublished replacement against an exact live consumer.
         * @param programs Complete finite program family and actual validated compiler output, borrowed synchronously.
         * @param expected Captured live consumer; stale or foreign identities fail before native preparation.
         * @param cancellation Checked before and between bounded preparation phases.
         * @return Owning complete candidate or typed unsupported/validation/native/cancellation failure.
         * @post Every failure preserves the current complete program, bindings and accepted work.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IShaderHotReloadCandidate>> Prepare(
            std::span<const ShaderHotReloadProgramView> programs, ShaderHotReloadTargetState expected,
            const CancellationToken &cancellation = {}) = 0;
        /** @brief Atomically adopts a complete candidate at the existing graphics preparation boundary.
         * @param candidate Candidate issued by this exact target; consumed on both success and failure.
         * @param expected Same captured live state used for preparation; revalidated immediately before mutation.
         * @return Newly live exact generation or typed stale/foreign/capacity/thread failure.
         * @post Failure retains the last good program. Success affects future recording only; old accepted
         * work retains its original objects. This method neither waits for GPU idle nor invokes a compiler.
         */
        [[nodiscard]] virtual Result<ShaderHotReloadTargetState> Adopt(std::unique_ptr<IShaderHotReloadCandidate> candidate,
                                                                       ShaderHotReloadTargetState expected) = 0;
    };
}  // namespace Horo::Render
