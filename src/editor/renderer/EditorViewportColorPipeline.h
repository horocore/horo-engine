#pragma once
/** @file EditorViewportColorPipeline.h @brief Explicit editor-owned color programs and submitted-view exposure lifecycle. */
#include "EditorViewportColorTypes.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

#include <array>
#include <optional>

namespace Horo::Render::Internal {
    struct ColorOutputCookPackage;
}

namespace Horo::Editor {
    /** @brief Shared value/resource owner; actual frame and backend remain the only submit/completion authorities.
     * @details Calls are serialized on the selected frontend's render owner. Initialization, settings and preparation occur outside
     * an active frame; Finish follows actual host Present/cancel. Source submission is a semantic per-view accepted ordinal, never
     * a fabricated frontend timeline point. The native kernel journal separately correlates it with real accepted command buffers.
     */
    class EditorViewportColorPipeline final {
    public:
        static constexpr std::size_t SlotCount = 4;
        /** @brief Owns frontend-issued handles only. @param frontend Existing selected renderer. @param port Actual private
         * completion/writes. */
        EditorViewportColorPipeline(Render::RenderFrontend &frontend, EditorViewportColorResourcePort port) noexcept;
        /** @brief Releases owned resources through ordinary deferred frontend retirement. */
        ~EditorViewportColorPipeline();
        EditorViewportColorPipeline(const EditorViewportColorPipeline &) = delete;
        EditorViewportColorPipeline &operator=(const EditorViewportColorPipeline &) = delete;
        /** @brief Realizes genuine cold programs, lookup tables and bounded history storage. @param inputs Host-decoded immutable family.
         * @return Original typed failure; no partial owner is published by its caller. */
        [[nodiscard]] Result<void> Initialize(const EditorViewportColorInputs &inputs);
        /** @brief Selects authored settings without mutating an in-flight candidate. @param settings Exact next settings.
         * @return Validated admission or original failure; unchanged settings preserve history. */
        [[nodiscard]] Result<void> RequestSettings(const Render::ExposureSettings &settings);
        /** @brief Prepares one next submitted-view candidate without advancing acceptance. @param view Frontend-issued view.
         * @param extent Exact unexposed scene shape. @param deltaSeconds Host-selected clock delta in [0,1].
         * @return Success, typed pending/capacity or malformed input. */
        [[nodiscard]] Result<void> Prepare(Render::RenderViewId view, Render::FramebufferExtent extent, float deltaSeconds);
        /** @brief Publishes candidate metadata only after the actual host accepted its native frame. @param accepted Actual Present result.
         * @details False cancels only the unsent candidate and preserves the last accepted state. */
        void Finish(bool accepted) noexcept;
        /** @brief Marks only successful actual graph encoding, before host acceptance. */
        void Encoded() noexcept;
        /** @brief Prepares immutable material bindings at the pre-frame resource boundary.
         * @param scene Exact canonical scene view. @param display Exact display-linear view.
         * @return Original bounded resource/material failure. */
        [[nodiscard]] Result<void> PrepareBindings(Render::RenderTextureViewHandle scene, Render::RenderTextureViewHandle display);
        /** @brief Returns current scene/encode material leases. @param role Zero scene or one encode. @return Synchronous borrow. */
        [[nodiscard]] const std::shared_ptr<const Render::ResidentMaterialBinding> &Material(std::size_t role) const noexcept;
        /** @brief Returns actual fullscreen mesh and its owned vertex/index handles. @return Exact frontend-issued recipe. */
        [[nodiscard]] Render::RenderMeshHandle Mesh() const noexcept;
        [[nodiscard]] std::array<Render::RenderBufferHandle, 2> GeometryBuffers() const noexcept;
        /** @brief Returns exact auxiliary textures/views. @param index Zero/one analytic table, or two for an enabled creative atlas.
         * @return Actual owned handles. */
        [[nodiscard]] Render::RenderTextureHandle TableTexture(std::size_t index) const noexcept;
        [[nodiscard]] Render::RenderTextureViewHandle TableView(std::size_t index) const noexcept;
        /** @brief Requests explicit reset at the next safe candidate boundary. */
        void Reset() noexcept;
        /** @brief Releases all handles and program leases on the render owner. @return First original retirement failure. */
        [[nodiscard]] Result<void> Close();
        /** @brief Returns exact pending produced metadata. @return Borrow valid until Finish/Close. */
        [[nodiscard]] const Render::ExposureState &Produced() const noexcept;
        /** @brief Returns actual accepted predecessor metadata or empty initial state. @return Borrowed value. */
        [[nodiscard]] const Render::ExposureState &Previous() const noexcept;
        /** @brief Returns actual pending histogram/reset resources. @return Three buffer handles: histogram/seed/output. */
        [[nodiscard]] std::array<Render::RenderBufferHandle, 3> MeterBuffers() const noexcept;
        /** @brief Returns validated pending dispatch ABI. @return Borrowed fixed uniform value. */
        [[nodiscard]] const Render::ExposureGpuDispatchParameters &Dispatch() const noexcept;
        /** @brief Returns the real native exposure lease. @return Existing selected-backend kernel. */
        [[nodiscard]] const std::shared_ptr<Render::IResidentExposureGpuKernel> &Kernel() const noexcept;
        /** @brief Returns exact selected mode/settings. @return Owned settings value. */
        [[nodiscard]] const Render::ExposureSettings &Settings() const noexcept;
        /** @brief Reports metadata only, without claiming GPU contents/completion. @return Bounded diagnostic snapshot. */
        [[nodiscard]] EditorViewportColorDiagnostics Diagnostics() const noexcept;

    private:
        friend class EditorViewportColorGraph;

        struct BufferState final {
            Render::RenderBufferHandle buffer;
            Render::RenderGraphLogicalState state;
        };

        std::array<BufferState, 32> acceptedStates_, pendingStates_;
        std::size_t acceptedStateCount_{}, pendingStateCount_{};
        bool tablesUsed_{};
        /** @brief Returns exact accepted graph state or known real initial-upload/host-write state. */
        [[nodiscard]] Render::RenderGraphLogicalState BufferInitialState(Render::RenderBufferHandle buffer,
                                                                         Render::RenderQueueId queue) const noexcept;
        /** @brief Distinguishes original table upload state from actual accepted sampled use. */
        [[nodiscard]] Render::RenderGraphLogicalState TableInitialState(Render::RenderQueueId queue) const noexcept;
        /** @brief Stages final state evidence from only the closed pending graph; cancellation discards it. */
        void StageBufferState(Render::RenderBufferHandle buffer, Render::RenderGraphLogicalState state) noexcept;
        /** @brief Publishes staged final states only at actual encoded-and-accepted host completion. */
        void PublishBufferStates() noexcept;

        struct Slot final {
            Render::RenderBufferHandle histogram, seed, output;
            std::array<Render::RenderBufferHandle, 2> uniform;
            std::array<std::array<std::shared_ptr<const Render::ResidentMaterialBinding>, SlotCount + 1>, 2> bindings;
            Render::ExposureState produced;
        };

        struct Program final {
            Render::RenderShaderModuleHandle vertex, fragment;
            Render::RenderPipelineHandle pipeline;
            Render::MaterialBindingLayout layout;
            std::unique_ptr<Render::MaterialBindingTable> materials;
            std::size_t uniformBytes{};
        };

        /** @brief Rechecks actual native legacy SDR admission and resets history on real output/display revisions. */
        [[nodiscard]] Result<void> ValidateOutput();
        /** @brief Rejects malformed owner/view/capacity before any native output or ring admission. */
        [[nodiscard]] Result<void> ValidatePreparation(Render::RenderViewId view, Render::FramebufferExtent extent) const;
        Render::RenderPresentationOutput output_;
        /** @brief Uses canonical cold transform validation while retaining the exact decoded generation. */
        [[nodiscard]] Result<void> PrepareSelection(const EditorViewportColorInputs &inputs);
        /** @brief Bounds actual frame slots and requires the selected resource callbacks before native preparation. */
        [[nodiscard]] Result<void> ValidateInitialization();
        /** @brief Retires all old material cache generations before changing the source attachment generation. */
        [[nodiscard]] Result<void> ClearBindings();
        /** @brief Selects baseline/look and encode artifacts from the same real decoded product package. */
        [[nodiscard]] Result<void> PreparePrograms(const EditorViewportColorInputs &inputs);
        /** @brief Creates genuine modules/pipeline and preserves the actual reflected uniform layout. */
        [[nodiscard]] Result<void> PrepareProgram(const EditorViewportColorProgram &input, std::size_t index);
        /** @brief Uploads only original lossless analytic table bytes and exact declared samplers. */
        [[nodiscard]] Result<void> PrepareTables(const Render::CookedColorOutputProgram &input);
        /** @brief Creates bounded fullscreen geometry and per-slot host-visible uniform storage. */
        [[nodiscard]] Result<void> PrepareGeometry();
        /** @brief Uploads the selected immutable creative atlas only when the canonical selection enables it. */
        [[nodiscard]] Result<void> PrepareLook();
        /** @brief Copies exact validated domain/interpolation controls into actual reflected offsets. */
        [[nodiscard]] Result<void> WriteLookParameters(std::span<std::byte> bytes);
        /** @brief Creates one cold immutable slot/predecessor material variant without mutating GPU data. */
        [[nodiscard]] Result<void> Bind(std::size_t role, Render::RenderTextureViewHandle scene, Render::RenderTextureViewHandle display,
                                        std::size_t slot, std::size_t previous);
        /** @brief Preallocates the bounded material matrix at attachment-generation changes, avoiding hot allocation. */
        [[nodiscard]] Result<void> PrepareAllBindings(Render::RenderTextureViewHandle scene, Render::RenderTextureViewHandle display);
        /** @brief Writes only an actually idle candidate uniform with exact prior-state identity or explicit manual scale. */
        [[nodiscard]] Result<void> WriteParameters(std::size_t role);
        /** @brief Attempts independent native program retirements after their material generations close. */
        [[nodiscard]] Result<void> ClosePrograms();
        /** @brief Retires exact creation pairs, preserving failures and dependency pins for retry. */
        [[nodiscard]] Result<void> CloseStorage();
        /** @brief Retires views before texture parents and independent samplers without dropping failures. */
        [[nodiscard]] Result<void> CloseTables();
        /** @brief Retires the mesh before its vertex/index dependencies. */
        [[nodiscard]] Result<void> CloseGeometry();
        /** @brief Selects the frozen accepted predecessor or the explicit fresh reset seed. */
        [[nodiscard]] std::size_t PreviousSlot() const noexcept;

        struct CreationRecord final {
            const void *address{};
            Render::ResourceOperationId operation;
        };

        std::array<CreationRecord, 64> creations_;
        std::size_t creationCount_{};

        /** @brief Retains an exact ordinary frontend creation pair before any resource queue is processed. */
        template <class Handle> Result<void> OwnCreation(Result<Render::ResourceCreation<Handle>> result, Handle &handle) {
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            handle = result.Value().handle;
            if (creationCount_ == creations_.size())
                return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Capacity));
            creations_[creationCount_++] = {&handle, result.Value().operation};
            return Result<void>::Success();
        }
        template <class Handle> void RetireOwned(Handle &handle, std::optional<Error> &first);
        /** @brief Processes a bounded resource queue and checks every exact retained operation result. */
        [[nodiscard]] Result<void> DrainCreations();
        /** @brief Initializes all fixed history storage through actual device-local uploads. */
        [[nodiscard]] Result<void> PrepareStorage();
        /** @brief Retains one exact buffer creation before any resource queue processing. */
        [[nodiscard]] Result<void> CreateStorage(Render::RenderBufferHandle &destination, std::size_t bytes);
        /** @brief Polls actual complete candidate resources while preserving the last accepted output. */
        [[nodiscard]] Result<std::size_t> SelectSlot();
        /** @brief Issues bounded semantic per-view metadata and validated GPU parameters without claiming submission. */
        [[nodiscard]] Result<void> PrepareIdentity(Render::RenderViewId view, Render::FramebufferExtent extent, float deltaSeconds);
        Render::RenderFrontend *frontend_;
        EditorViewportColorResourcePort port_;
        std::array<Slot, SlotCount> slots_;
        std::array<Program, 2> programs_;
        std::array<Render::RenderTextureHandle, 2> tables_;
        std::array<Render::RenderTextureViewHandle, 2> tableViews_;
        std::array<Render::RenderSamplerHandle, 2> samplers_;
        Render::RenderTextureHandle lookTexture_;
        Render::RenderTextureViewHandle lookView_;
        std::optional<Render::ColorTransformSelection> selection_;
        std::array<Render::RenderBufferHandle, 2> geometry_;
        Render::RenderMeshHandle mesh_;
        Render::RenderTextureViewHandle boundScene_, boundDisplay_;
        std::uint64_t materialRevision_{};
        EditorViewportColorStatus status_{EditorViewportColorStatus::Unavailable};
        std::shared_ptr<Render::IResidentExposureGpuKernel> kernel_;
        Render::ExposureSettings settings_;
        Render::ExposureState previous_, empty_;
        Render::ExposureGpuDispatchParameters dispatch_;
        Render::FramebufferExtent extent_;
        Render::RenderViewId view_;
        std::uint64_t generation_{}, settingsGeneration_{1}, acceptedSubmission_{};
        std::size_t candidateSlot_{}, nextSlot_{}, activeSlots_{SlotCount};
        bool candidate_{}, encoded_{}, reset_{true}, resetQueued_{};
    };
}  // namespace Horo::Editor
