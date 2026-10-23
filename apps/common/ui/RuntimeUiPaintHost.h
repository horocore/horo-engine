#pragma once

/** @file RuntimeUiPaintHost.h
 * @brief Non-installed fixed application lifecycle adapter joining the canonical UI source and same-frame renderer stage.
 */
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "RuntimeUiPaintProjection.h"

namespace Horo::Application::Internal {
    namespace Detail {
        struct RuntimeUiPaintHostState;
    }

    /** @brief Finite owner lifecycle; no source creates no native paint buffers or fabricated document. */
    enum class RuntimeUiPaintHostPhase : std::uint8_t {
        NoSource,
        Active,
        Retiring,
        Failed
    };

    /** @brief Exact host asset owner and already-realized native image, retained through source retirement. */
    struct RuntimeUiPaintImage final {
        std::shared_ptr<const Runtime::Ui::UiImageResourceRegistry> registry;
        Render::UiRenderImageTexture realization;
    };

    /** @brief Exact existing face/page realization; an atlas frame separately pins every actually sampled glyph. */
    struct RuntimeUiPaintFont final {
        std::shared_ptr<const Runtime::Ui::UiFontFace> face;
        Render::UiRenderAtlasTexture realization;
    };

    /** @brief Owned actual asset/control resource composition, never a backend texture registry. */
    struct RuntimeUiPaintResources final {
        std::vector<RuntimeUiPaintImage> images;
        std::vector<RuntimeUiPaintFont> fonts;
        std::shared_ptr<Runtime::Ui::UiGlyphAtlas> atlas;
        std::vector<Runtime::Ui::UiGlyphAtlasGlyphKey> glyphs;
    };

    /** @brief Complete safe-point source/derived/renderer preparation; only the trusted executable host supplies these resources. */
    struct RuntimeUiPaintSourcePreparation final {
        Runtime::Ui::RuntimeUiInstanceId instance;
        Runtime::Ui::UiElementSlotAllocator &allocator;
        Runtime::Ui::RuntimeStyleRegistry registry;
        Runtime::Ui::UiStyleResolver styles;
        Runtime::Ui::UiAnimationCanvasDefinition animation;
        Runtime::Ui::UiAnimationLimits animationLimits;
        Runtime::UiAnimationRuntimeConfig runtime;
        RuntimeUiPaintDeclaration declaration;
        RuntimeUiPaintResources resources;
        Runtime::Ui::UiRenderExtractorDescriptor extraction;
        Runtime::Ui::UiRenderGeometryArenaDescriptor geometry;
        Render::UiPaintProgramDescriptor program;
        Render::CookedUiPaintKernel kernel;
    };

    class RuntimeUiPaintHost;
    struct RuntimeUiPaintComposition;

    /** @brief Revocable application port; it never retains or dereferences the lifecycle adapter wrapper.
     * @details Calls are serialized on the original UI/render owner thread. Destruction/Shutdown revokes admission before
     * participant/renderer teardown; retained ports pin only revoked storage, never the host wrapper. Native modules receive only
     * Issue's client. Release the final port on the owner thread after explicit CollectRetired succeeds.
     */
    class RuntimeUiPaintPort final {
    public:
        RuntimeUiPaintPort() noexcept = default;
        /** @brief Admits a real complete source through the sole canonical publisher. */
        [[nodiscard]] Result<void> Admit(const Runtime::Ui::UiSceneInstanceDescriptor &descriptor, Runtime::Ui::UiHotReload &&publisher,
                                         Runtime::Ui::UiStructuralCommitPoint point) const;
        /** @brief Issues the existing narrowly scoped native-module client; never the source service. */
        [[nodiscard]] Result<std::shared_ptr<const Runtime::Ui::RuntimeUiClient>> Issue(const Runtime::Ui::RuntimeUiGrant &grant,
                                                                                        const CancellationToken &cancellation = {}) const;
        /** @brief Attaches one real admitted source and reserves its complete derived producer transactionally. */
        [[nodiscard]] Result<void> AttachSource(RuntimeUiPaintSourcePreparation preparation) const;
        /** @brief Coordinates the same source publisher/derived owner; failed preparation keeps the prior source and native program. */
        [[nodiscard]] Result<Runtime::UiAnimationRuntimeReload> Reload(Runtime::Ui::UiAnimationReloadPreparation preparation,
                                                                       RuntimeUiPaintDeclaration declaration,
                                                                       RuntimeUiPaintResources resources,
                                                                       const CancellationToken &cancellation = {}) const;
        /** @brief Coalesces actual output facts for the next owner-command safe point; an empty host admits no paint work.
         * @param evidence Committed physical output, independently observed DPI and physical safe-area insets.
         * @return Admission or typed invalid/source/lifecycle failure. Prior prepared geometry cannot restore input after a change.
         */
        [[nodiscard]] Result<void> UpdateOutput(const Runtime::Ui::UiCanvasViewportEvidence &evidence) const;
        /** @brief Closes new source/render work and retains the stopped child until the canonical drain fence permits collection. */
        [[nodiscard]] Result<void> DetachSource() const;
        /** @brief Collects retired child/source resources only at explicit application quiescence; never waits for native completion. */
        [[nodiscard]] Result<bool> CollectRetired() const;
        /** @brief Extends the actual active scene frame with the exact prepared UI; no separate Execute or Present authority. */
        [[nodiscard]] Result<void> ComposeUi(Render::RenderFrameScope &frame, std::uint64_t frameNumber) const;
        /** @brief Applies a receipt only for UI actually encoded in that same native frame; presentation never retires GPU claims. */
        [[nodiscard]] Result<void> Presented(std::uint64_t frameNumber, bool nativePresentSucceeded) const;
        /** @brief Pins the actual current derived frame for host inspection; its lifetime participates in the existing drain fence. */
        [[nodiscard]] Result<Runtime::Ui::UiAnimationFrameLease> AcquireFrame() const;
        /** @brief Observes the actual receipt-fenced input admission for the composed view. */
        [[nodiscard]] bool InputEligible(Runtime::Ui::UiRenderViewId view) const noexcept;
        /** @brief Observes finite source admission without borrowing a mutable owner. */
        [[nodiscard]] RuntimeUiPaintHostPhase Phase() const noexcept;

    private:
        friend class RuntimeUiPaintHost;

        explicit RuntimeUiPaintPort(std::shared_ptr<Detail::RuntimeUiPaintHostState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::RuntimeUiPaintHostState> state_;
    };

    /** @brief Fixed participant registered before RuntimeHost startup and before the editor frame consumer. */
    class RuntimeUiPaintHost final : public Runtime::RuntimeLifecycleParticipant {
    public:
        /** @brief Creates a lawful empty canonical service and fixed adapter; no native residents exist until real source admission.
         * @param frontend Actual selected render owner, alive through adapter Shutdown and native drain.
         * @param source Actual RuntimeHost dispatch issuer. @param limits Explicit finite semantic registry policy.
         * @return Fixed participant and revocable application port or original typed failure.
         */
        [[nodiscard]] static Result<RuntimeUiPaintComposition> Compose(Render::RenderFrontend &frontend,
                                                                       Runtime::RuntimeDispatchSource source,
                                                                       Runtime::Ui::UiSceneReconciliationLimits limits);
        /** @brief Transfers an already composed canonical host service without republishing or copying its source inventory.
         * @param frontend Same selected render owner. @param source Actual host dispatch issuer.
         * @param service Sole existing host service, moved into the fixed adapter. @return Fixed participant and owned revocable port.
         */
        [[nodiscard]] static Result<RuntimeUiPaintComposition> Compose(Render::RenderFrontend &frontend,
                                                                       Runtime::RuntimeDispatchSource source,
                                                                       Runtime::Ui::RuntimeUiService service);
        ~RuntimeUiPaintHost() override;
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override;
        [[nodiscard]] Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override;
        void Shutdown() noexcept override;

    private:
        explicit RuntimeUiPaintHost(std::shared_ptr<Detail::RuntimeUiPaintHostState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::RuntimeUiPaintHostState> state_;
    };

    /** @brief Explicit application ownership transfer; only the participant is registered with RuntimeHost. */
    struct RuntimeUiPaintComposition final {
        std::unique_ptr<RuntimeUiPaintHost> participant;
        RuntimeUiPaintPort port;
    };
}  // namespace Horo::Application::Internal
