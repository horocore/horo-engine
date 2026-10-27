#pragma once
/** @file VfxGraphScheduling.h
 * @brief Renderer-owned recipe anchors and scene-wide VFX graph scheduling result.
 */
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/StandardPbrPassPlan.h"
#include "Horo/Runtime/Render/VfxDrawData.h"

#include <array>
#include <optional>

namespace Horo::Render {
    /** @brief Host-admitted renderer storage prepared before scene activation, including all views and retained frames. */
    struct VfxSchedulingBudget final {
        std::uint32_t maximumInstances{};
        std::uint32_t retainedViewSlots{};
        std::size_t maximumBytes{};   /**< Bound for prepared slot records, six orders and shared radix scratch. */
        std::uint32_t maximumDraws{}; /**< Optional native draw capacity per stage, derived from the host scene draw budget. */
    };

    /** @brief Host-selected canonical material/mesh slot for one exact order run; contains no backend selection.
     * @details Billboard geometry is an authored centered unit quad. Instanced mesh must equal the extracted batch mesh.
     * Slots are uniquely owned while preparing; move/retain one host handle rather than adding facade copies.
     */
    struct VfxGraphDrawBinding final {
        VfxGraphStage stage{VfxGraphStage::OpaqueForward};
        std::uint32_t firstOrder{};
        RenderMeshHandle mesh;
        RenderTargetHandle target;
        RenderGraphResourceId vertices;
        RenderGraphResourceId indices;
        RenderGraphResourceId parameters;
        RenderGraphResourceId sampledTexture;
        std::shared_ptr<PreparedVfxFrameSlot> slot;
    };

    /** @brief Frozen recipe/view inputs for one graph's VFX contribution.
     * @details The host supplies actual graph-local attachments and ordering anchors. Opaque VFX precedes
     * opaqueComplete, alpha follows completed lighting/depth, and both precede the output transform.
     * The draw allowance is the remaining global scene budget, not another per-view allowance.
     */
    struct VfxGraphRequest final {
        StandardPbrRasterFamily family{StandardPbrRasterFamily::Forward};
        bool depthPrepass{};
        std::uint64_t scene{};
        std::uint64_t frame{};
        std::uint64_t originRevision{};
        std::uint64_t view{};
        std::uint64_t viewGeneration{};
        Math::Vec3 camera{};
        Math::Vec3 forward{0, 0, -1}; /**< Same captured origin as the snapshot; forward must be normalized. */
        std::uint32_t maximumInstances{};
        std::uint32_t remainingDrawCalls{};
        RenderGraphPassRef opaqueBegin{};
        RenderGraphPassRef opaqueComplete{};
        RenderGraphPassRef outputTransform{};
        RenderGraphResourceId sceneColor{};
        RenderGraphResourceId sceneDepth{};
        RenderGraphResourceId shadowDepth{};
        RenderGraphResourceId gBuffer{};
    };

    /** @brief Exact typed work inserted into the graph; merge workloads by pass identity after graph compilation. */
    struct VfxGraphSchedule final {
        std::array<RenderGraphPassWorkload, 6> records{};
        std::uint32_t recordCount{};
        std::uint32_t drawCalls{};
        std::uint32_t instances{};

        /** @brief Returns only inserted semantic passes. @return Stable records owned by this result. */
        [[nodiscard]] std::span<RenderGraphPassWorkload> Workloads() noexcept {
            return {records.data(), recordCount};
        }

        /** @brief Returns only inserted semantic passes. @return Stable immutable records owned by this result. */
        [[nodiscard]] std::span<const RenderGraphPassWorkload> Workloads() const noexcept {
            return {records.data(), recordCount};
        }
    };

    /** @brief Allocation-free scheduling outcome; expected failures retain static descriptor/context references.
     * @details ErrorValue is a presentation boundary and may allocate. Callers inspect HasValue/FailureDescriptor
     * during frame work, and project an owned diagnostic afterwards. Unexpected builder failures retain their original Error.
     */
    class VfxGraphSchedulingOutcome final {
    public:
        /** @brief Captures complete bounded scheduling records. @return Successful outcome without allocation. */
        [[nodiscard]] static VfxGraphSchedulingOutcome Success(VfxGraphSchedule schedule) noexcept {
            VfxGraphSchedulingOutcome result;
            result.schedule_ = std::move(schedule);
            return result;
        }

        /** @brief Records a static expected failure. @return Failure without owning diagnostic storage. */
        [[nodiscard]] static VfxGraphSchedulingOutcome Failure(const ErrorCodeDescriptor &descriptor,
                                                               std::string_view context = {}) noexcept {
            VfxGraphSchedulingOutcome result;
            result.descriptor_ = &descriptor;
            result.context_ = context;
            return result;
        }

        /** @brief Preserves an unexpected builder failure. @return Original error without translation. */
        [[nodiscard]] static VfxGraphSchedulingOutcome Failure(Error error) noexcept {
            VfxGraphSchedulingOutcome result;
            result.original_ = std::move(error);
            return result;
        }

        /** @brief Reports complete admission. @return True only when every required stage was appended. */
        [[nodiscard]] bool HasValue() const noexcept {
            return descriptor_ == nullptr && !original_;
        }

        /** @brief Reports failed admission. @return True when prior graph records were preserved. */
        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        /** @brief Returns complete records. @return Owned schedule. @pre HasValue is true. */
        [[nodiscard]] VfxGraphSchedule &Value() noexcept {
            return schedule_;
        }

        /** @brief Returns complete records. @return Immutable schedule. @pre HasValue is true. */
        [[nodiscard]] const VfxGraphSchedule &Value() const noexcept {
            return schedule_;
        }

        /** @brief Returns expected failure identity without allocation. @return Static descriptor, or null for success/unexpected error. */
        [[nodiscard]] const ErrorCodeDescriptor *FailureDescriptor() const noexcept {
            return descriptor_;
        }

        /** @brief Materializes diagnostics outside measured frame work. @return Exact failure identity/context. @pre HasError is true. */
        [[nodiscard]] Error ErrorValue() const {
            return original_ ? *original_ : MakeError(*descriptor_, std::string{context_});
        }

    private:
        VfxGraphSchedule schedule_;
        const ErrorCodeDescriptor *descriptor_{};
        std::string_view context_;
        std::optional<Error> original_;
    };
}  // namespace Horo::Render
