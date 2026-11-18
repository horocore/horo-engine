#pragma once

/** @file NativeReconstructionViewHistory.h @brief Scene-owned native reconstruction history and exact graph handoff. */
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <array>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::Render {
    class RenderFrontend;
    class RenderFrameScope;

    namespace Detail {
        class RenderGraphResourceLeasePool;
    }

    /** @brief Exact resident whole texture and its graph-visible native view generation. */
    struct NativeReconstructionSceneImage final {
        RenderTextureHandle texture;
        RenderTextureViewHandle view;
    };

    /** @brief Scene-allocated ping-pong attachment pair, retained by the scene through retirement. */
    struct NativeReconstructionHistoryImages final {
        NativeReconstructionSceneImage color;
        NativeReconstructionSceneImage depth;
    };

    /** @brief Exact current-frame view handles for the already-declared canonical color, depth and motion inputs. */
    struct NativeReconstructionSceneViews final {
        RenderTextureViewHandle color;
        RenderTextureViewHandle depth;
        RenderTextureViewHandle motion;
    };

    /** @brief Actual final logical state of the exact rendered color and depth history images. */
    struct NativeReconstructionHistoryStates final {
        RenderGraphLogicalState color;
        RenderGraphLogicalState depth;
    };

    /** @brief One synchronous graph authoring packet with native image roles in canonical binding order. */
    struct NativeReconstructionScenePlan final {
        NativeReconstructionDispatchPlan dispatch;
        std::array<NativeReconstructionSceneImage, static_cast<std::size_t>(NativeReconstructionImage::Count)> images;
        std::optional<NativeReconstructionHistoryStates> previousStates; /**< Exact preserved imports for a history dispatch. */
    };

    /** @brief Latest successful view-local frame and its exact history image pair. */
    struct NativeReconstructionScenePublication final {
        NativeReconstructionHistoryImages images;
        NativeReconstructionHistoryStates states;
        std::uint64_t frameId{};
        std::uint64_t generation{};
    };

    /** @brief One scene-owned view history; the renderer's existing lease keeps graph resources alive through native completion.
     * @details All operations are owner-thread only. The scene provisions its exact two resident image pairs before frame work
     * and keeps those resources alive through pending submissions. A replacement is explicit and must wait for an idle owner by
     * polling; successful old commands cannot publish into a different cache epoch. No CPU wait occurs here.
     */
    class NativeReconstructionViewHistory final {
    private:
        friend class RenderFrameScope;
        enum class FrameDisposition : std::uint8_t {
            Cancelled,
            Submitted,
            SubmissionUncertain
        };

    public:
        using ImagePairs = std::array<NativeReconstructionHistoryImages, 2>;

        /** @brief Prepares a bounded history for the selected renderer and one scene cache generation.
         * @param frontend Live selected renderer on the render owner.
         * @param compatibility Exact scene-origin-stamped view/surface/recipe generation and extents.
         * @param images Two distinct resident resolved color/depth pairs.
         * @return Owned idle view history or typed readiness, capability or descriptor failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<NativeReconstructionViewHistory>> Create(
            RenderFrontend &frontend, const TemporalHistoryCompatibility &compatibility, const ImagePairs &images);

        /** @brief Stages one exact view frame using the nonpublished pair as the destination.
         * @param frontend Same live renderer used to prepare the view.
         * @param request Current real-frame request, including compatibility stamped by the scene cache and exact source textures.
         * @param views Exact source texture views; compared to the compiled graph at handoff.
         * @param tuning Explicit blend and depth settings.
         * @return Synchronous plan for authoring a graph, or typed stale, busy or incompatible failure.
         */
        [[nodiscard]] Result<NativeReconstructionScenePlan> Stage(RenderFrontend &frontend, ReconstructionFrameRequest request,
                                                                  NativeReconstructionSceneViews views,
                                                                  const NativeReconstructionTemporalSettings &tuning);

        /** @brief Abandons a plan before it has been handed off to a frame.
         * @return Success or a typed lifecycle or owner-thread error.
         */
        [[nodiscard]] Result<void> CancelStaged();

        /** @brief Polls the exact accepted real frame's native timeline and publishes only in the current scene cache epoch.
         * @param frontend Same live renderer.
         * @param current Current scene-origin-stamped view compatibility.
         * @return True when this poll publishes; false for no publishable completion; typed timeline/lifecycle failure otherwise.
         */
        [[nodiscard]] Result<bool> Poll(RenderFrontend &frontend, const TemporalHistoryCompatibility &current);

        /** @brief Replaces the view's image pairs and compatibility only after all pending work is terminal.
         * @param frontend Same renderer owner.
         * @param compatibility New exact scene-origin-stamped identity.
         * @param images Replacement live texture pairs.
         * @param cause Explicit resize, scene discontinuity or other reset.
         * @return Success or typed lifecycle/resource failure; prior configuration remains on failure.
         */
        [[nodiscard]] Result<void> Reset(RenderFrontend &frontend, const TemporalHistoryCompatibility &compatibility,
                                         const ImagePairs &images, TemporalHistoryResetCause cause);

        /** @brief Borrows the last view-local publication while the exact cache epoch is still current. */
        [[nodiscard]] std::optional<NativeReconstructionScenePublication> Published(
            const TemporalHistoryCompatibility &current) const noexcept;

    private:
        friend class Detail::RenderGraphResourceLeasePool;
        enum class Phase : std::uint8_t {
            Idle,
            Staged,
            Recording,
            Submitted,
            Discarding
        };

        NativeReconstructionViewHistory(TemporalHistoryStore store, TemporalHistoryHandle handle, const ImagePairs &images,
                                        TemporalHistoryCompatibility compatibility) noexcept;
        bool OnOwner() const noexcept;
        bool SameFrontend(RenderFrontend &frontend) const;
        void BeginRecording(RenderTimelinePoint completion, NativeReconstructionHistoryStates states) noexcept;
        void FinishRecording(FrameDisposition disposition) noexcept;
        void NativeLeaseRetired() noexcept;
        void Abandon() noexcept;

        TemporalHistoryStore store_;
        TemporalHistoryHandle handle_;
        ImagePairs images_;
        TemporalHistoryCompatibility compatibility_;
        std::thread::id owner_;
        std::optional<TemporalHistoryFrame> pending_;
        NativeReconstructionScenePlan plan_;
        std::optional<NativeReconstructionScenePublication> publication_;
        std::optional<std::size_t> publishedSlot_;
        std::size_t candidateSlot_{};
        RenderTimelinePoint completion_;
        NativeReconstructionHistoryStates candidateStates_;
        Phase phase_{Phase::Idle};
        bool leaseRetired_{};
        bool requiresReset_{};
    };
}  // namespace Horo::Render
