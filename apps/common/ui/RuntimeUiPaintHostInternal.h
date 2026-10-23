#pragma once

#include "RuntimeUiPaintHost.h"

#include <optional>
#include <thread>

namespace Horo::Application::Internal::Detail {
    /** @brief One transient extraction/upload/presentation transaction; all storage is reserved by source admission. */
    struct RuntimeUiPaintFrame final {
        Runtime::Ui::UiAnimationFrameLease animation;
        std::optional<Runtime::Ui::UiRenderSnapshot> snapshot;
        std::optional<Runtime::Ui::UiRenderGeometryPlan> geometry;
        std::optional<Render::UiPaintPreparedFrame> prepared;
        std::array<Render::UiRenderImageBinding, Render::MaximumUiSubmissionImages> images;
        std::array<Render::UiRenderFontBinding, Render::MaximumUiSubmissionFonts> fonts;
        Render::UiRenderSubmission submission;
        std::optional<Runtime::Ui::UiPresentationReceipt> encoded;
        std::uint64_t encodedFrame{};
        std::uint32_t slot{};
    };

    /** @brief Actual immutable source policy plus latest native evidence; observation never authors a second canvas. */
    struct RuntimeUiPaintOutput final {
        Runtime::Ui::UiCanvasDescriptor canvas;
        Runtime::Ui::UiAnimationViewport viewport;
        std::optional<Runtime::Ui::UiCanvasViewportEvidence> current;
        std::optional<Runtime::Ui::UiCanvasViewportEvidence> pending;
        bool awaitingPresentation{};
    };

    /** @brief Complete owned renderer/extractor preparation policy transferred once at source admission. */
    struct RuntimeUiPaintSourcePolicy final {
        Runtime::Ui::UiRenderExtractorDescriptor extraction;
        Runtime::Ui::UiRenderGeometryArenaDescriptor geometry;
        Render::UiPaintProgramDescriptor program;
        Render::CookedUiPaintKernel kernel;
        Runtime::Ui::UiAnimationViewport viewport;
    };

    /** @brief One actual derived source and its renderer preparations; teardown never owns the canonical source service. */
    struct RuntimeUiPaintSource final {
        Runtime::Ui::RuntimeUiInstanceId instance;
        std::unique_ptr<Runtime::UiAnimationRuntimeParticipant> animation;
        Runtime::UiAnimationClockController controller;
        std::unique_ptr<RuntimeUiPaintProjection> projection;
        RuntimeUiPaintResources resources;
        Runtime::Ui::UiRenderExtractorDescriptor extraction;
        Runtime::Ui::UiRenderGeometryArena arena;
        Runtime::Ui::UiRenderGeometryArenaDescriptor geometryPolicy;
        Render::UiPaintProgramDescriptor policy;
        Render::CookedUiPaintKernel kernel;
        std::unique_ptr<Render::UiPaintProgram> program;
        std::unique_ptr<Render::UiPaintProgram> retiredProgram;
        RuntimeUiPaintFrame frame;
        std::vector<Render::RenderTextureHandle> textures;
        bool refresh{true};
        RuntimeUiPaintOutput output;

        RuntimeUiPaintSource(Runtime::Ui::RuntimeUiInstanceId id, Runtime::UiAnimationRuntimeComposition derived,
                             std::unique_ptr<RuntimeUiPaintProjection> projected, RuntimeUiPaintResources assets,
                             Runtime::Ui::UiRenderGeometryArena geometryArena, RuntimeUiPaintSourcePolicy prepared)
            : instance(id), animation(std::move(derived.participant)), controller(std::move(derived.controller)),
              projection(std::move(projected)), resources(std::move(assets)), extraction(prepared.extraction),
              arena(std::move(geometryArena)), geometryPolicy(prepared.geometry), policy(prepared.program),
              kernel(std::move(prepared.kernel)), output{projection->Canvas(), prepared.viewport} {}

        /** @brief Resolves coalesced native evidence through canonical canvas policy before the next derived update. */
        [[nodiscard]] Result<void> ApplyOutput();
        /** @brief Validates/preserves the complete pre-admitted native texture inventory outside frame work. */
        [[nodiscard]] Result<void> AdmitResources();
        /** @brief Prepares a complete structural replacement only at the explicit application owner safe point. */
        [[nodiscard]] Result<void> Refresh(Render::RenderFrontend &frontend, const CancellationToken &cancellation);
        /** @brief Builds allocation-free current image/font/atlas evidence for the exact selected native slot. */
        [[nodiscard]] Result<void> BindResources();
        /** @brief Extracts current canonical source/derived data before the editor opens its native frame. */
        [[nodiscard]] Result<void> Extract(Runtime::Ui::RuntimeUiService &service, std::uint64_t frameNumber,
                                           const CancellationToken &cancellation);
        /** @brief Releases local CPU pins before the existing derived/source drain fence is queried. */
        void ReleaseFrame() noexcept;
    };

    /** @brief Shared revocable port storage with no callback/host-wrapper pointer or second publisher. */
    struct RuntimeUiPaintHostState final {
        const std::thread::id owner{std::this_thread::get_id()};
        Runtime::Ui::RuntimeUiService service;
        Runtime::RuntimeDispatchSource dispatch;
        Render::RenderFrontend *frontend{}; /**< Cleared at Shutdown before any retained port may outlive presentation. */
        std::unique_ptr<RuntimeUiPaintSource> source;
        CancellationSource cancellation;
        RuntimeUiPaintHostPhase phase{RuntimeUiPaintHostPhase::NoSource};
        bool started{};
        bool revoked{};
        bool dispatching{};

        RuntimeUiPaintHostState(Runtime::Ui::RuntimeUiService canonical, Runtime::RuntimeDispatchSource issuer,
                                Render::RenderFrontend &renderer)
            : service(std::move(canonical)), dispatch(std::move(issuer)), frontend(&renderer) {}

        /** @brief Admits owner-thread calls before touching mutable source or renderer state. */
        [[nodiscard]] bool Available() const noexcept {
            return owner == std::this_thread::get_id() && !revoked;
        }

        /** @brief Stops admission, releases CPU observations and retains the actual stopped child for explicit drain. */
        [[nodiscard]] Result<void> Retire();
    };
}  // namespace Horo::Application::Internal::Detail
