#pragma once

/** @file RuntimeUiPaintHost.h
 * @brief Non-installed fixed application lifecycle adapter joining the canonical UI source and same-frame renderer stage.
 */
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"
#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "RuntimeUiPaintProjection.h"

#include <functional>
#include <utility>

namespace Horo::Runtime {
    class SceneActivationCandidate;
}

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

    /** @brief Observes the current scene on its owner lane while the activation participant is alive. */
    using RuntimeUiPaintWorldSceneView = std::function<std::optional<Runtime::RuntimeSceneView>()>;

    /** @brief Exact authored World canvas placement and bounded offscreen dimensions before viewport admission. */
    struct RuntimeUiPaintWorldCanvas final {
        Runtime::EntityRef entity;
        Runtime::Ui::UiCanvasAssetReference reference;
        Runtime::Ui::UiDocumentRevision documentRevision;
        Assets::AssetRegistryRevision registryRevision;
        Runtime::Ui::UiWorldCanvasProjection plane; /**< Scene coordinates, before selected view's render-origin localization. */
        Runtime::Ui::UiCanvasLogicalExtent logicalExtent;
        Runtime::Ui::UiCanvasPixelExtent rasterExtent; /**< Authored raster dimensions independent of monitor extent and DPI. */
        Runtime::Ui::UiCanvasScaleFactor fontScale;
        std::weak_ptr<const RuntimeUiPaintWorldSceneView> currentScene;
    };

    /** @brief Owner-lane borrows for synchronous SceneCanvas consumer preparation within the extracted frame. */
    struct RuntimeUiPaintSceneCanvasFrame final {
        Runtime::Ui::RuntimeUiInstanceId instance;
        Runtime::SceneRuntimeId scene;
        const RuntimeUiPaintWorldCanvas *worldCanvas{};
        const Runtime::Ui::UiRenderSnapshot *prototype{};
        const Render::UiPaintProgramDescriptor *program{};
        const Render::CookedUiPaintKernel *kernel{};
        std::span<const Render::RenderTextureHandle> textures;
    };

    /** @brief Immutable hit geometry admitted by the same World canvas presentation as the drawn snapshot. */
    struct RuntimeUiPaintPresentedWorldCanvas final {
        Runtime::Ui::RuntimeUiInstanceId instance;
        Runtime::Ui::UiPresentationReceipt presentation;
        Runtime::Ui::UiHitTestSnapshot hit;
    };

    /** @brief Bounded input submitted by the owner of an already admitted World pointer context. */
    struct RuntimeUiPaintWorldPointerBatch final {
        Runtime::Ui::UiPointerInteraction &interaction;
        std::span<const Runtime::Ui::UiPointerSample> samples;
        std::uint64_t milliseconds;
        std::uint64_t &nextSequence;
        std::span<Runtime::Ui::UiControlDefaultAction> defaults;
        std::uint32_t &writtenDefaults;
    };

    /** @brief One headset submission's presentation and hit geometry, retained until the matching native frame completes. */
    class RuntimeUiPaintHeadsetCanvasReceipt final {
        friend class RuntimeUiPaintPort;

        RuntimeUiPaintHeadsetCanvasReceipt(Runtime::Ui::UiPresentationReceipt receipt, Runtime::Ui::UiHitTestSnapshot hit)
            : receipt_(std::move(receipt)), hit_(std::move(hit)) {}

        Runtime::Ui::UiPresentationReceipt receipt_;
        Runtime::Ui::UiHitTestSnapshot hit_;
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
        std::shared_ptr<Runtime::Ui::UiElementSlotAllocator>
            allocatorLifetime;                                /**< Optional exact owner pin; must alias allocator when supplied. */
        std::optional<RuntimeUiPaintWorldCanvas> worldCanvas; /**< Scene-owned source routed through the selected viewport graph. */
        bool sceneCanvas{}; /**< Route the authored Camera or World source through the selected scene graph. */
    };

    /** @brief Same canonical private source plus fully owned derived/native preparation; no Scene component borrow survives. */
    struct RuntimeUiPaintSceneSourcePreparation final {
        Runtime::Ui::UiSceneActivation activation;
        RuntimeUiPaintSourcePreparation paint;
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
        /** @brief Prepares all exact incoming Scene sources and one sealed canonical transaction before Scene publication.
         * @param transition Actual previous/next runtime Scene generation; absence expresses initial activation or normal unload.
         * @param incoming Complete authored component inventory, in actual Scene paint order.
         * @param replacements Explicit complete persistent provider rebindings, never inferred from current Scene state.
         * @param cancellation Exact load-operation ancestry.
         * @return Generic Scene-owned activation/retirement candidate or original failure preserving every active source.
         * @details Fixed product inventory admits at most eight TOTAL manual/Scene sources and uses reserved retirement slots.
         * The candidate's no-fail Publish runs only under the existing Scene claim; Shutdown revokes only its exact Scene.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareScene(
            const Runtime::Ui::UiSceneTransitionRequest &transition, std::vector<RuntimeUiPaintSceneSourcePreparation> incoming,
            std::vector<Runtime::Ui::UiSceneBindingReplacement> replacements = {}, const CancellationToken &cancellation = {}) const;
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
        /** @brief Updates the exact game viewport metrics independently of the primary output and its safe area. */
        [[nodiscard]] Result<void> UpdateCameraOutput(Runtime::Ui::UiCanvasPixelExtent extent) const;
        /** @brief Admits one committed host preference for all active and subsequently attached UI sources.
         * @param policy Complete immutable accessibility policy from one committed configuration revision.
         * @return Original owner, stale, or invalid policy failure. No local snapshot changes until the derived update.
         */
        [[nodiscard]] Result<void> UpdateVisualAccessibility(const Runtime::Ui::UiVisualPresentationPolicy &policy) const;
        /** @brief Closes new source/render work and retains the stopped child until the canonical drain fence permits collection. */
        [[nodiscard]] Result<void> DetachSource() const;
        /** @brief Collects retired child/source resources only at explicit application quiescence; never waits for native completion. */
        [[nodiscard]] Result<bool> CollectRetired() const;
        /** @brief Extends the actual active scene frame with the exact prepared UI; no separate Execute or Present authority. */
        [[nodiscard]] Result<void> ComposeUi(Render::RenderFrameScope &frame, std::uint64_t frameNumber) const;
        /** @brief Observes the one extracted Camera or World source for synchronous consumer preparation.
         * @param frameNumber Exact Runtime extraction frame.
         * @param scene Exact selected viewport scene.
         * @param mode Optional consumer-specific filter for the authored canvas render mode.
         * @return Empty when the scene owns no extracted viewport source, or original owner/extraction failure.
         * @details Borrows expire on the next extraction, source transition, retirement or shutdown. Do not retain them.
         */
        [[nodiscard]] Result<std::optional<RuntimeUiPaintSceneCanvasFrame>> ReadSceneCanvasFrame(
            std::uint64_t frameNumber, Runtime::SceneRuntimeId scene, std::optional<Runtime::Ui::UiRenderMode> mode = std::nullopt) const;
        /** @brief Transfers the exact extracted paint submission once and binds the renderer's actual SceneCanvas slot.
         * @param instance Exact active viewport source. @param frameNumber Exact Runtime extraction frame.
         * @param program Selected SceneCanvas program. @param slot Slot chosen by the viewport graph.
         * @return Move-only submission with frame-local borrows or original lifecycle/binding failure.
         */
        [[nodiscard]] Result<Render::UiRenderSubmission> TakeSceneCanvasSubmission(Runtime::Ui::RuntimeUiInstanceId instance,
                                                                                   std::uint64_t frameNumber,
                                                                                   const Render::UiPaintProgram &program,
                                                                                   std::uint32_t slot) const;
        /** @brief Records the completed viewport encoding before native presentation.
         * @param instance Exact submitted scene source. @param frameNumber Exact Runtime extraction frame.
         * @param plane Scene-space plane only for a World source.
         * @return Success or typed stale/invalid evidence. Presented determines the terminal outcome.
         */
        [[nodiscard]] Result<void> SceneCanvasEncoded(Runtime::Ui::RuntimeUiInstanceId instance, std::uint64_t frameNumber,
                                                      std::optional<Runtime::Ui::UiWorldCanvasProjection> plane) const;
        /** @brief Transfers the encoded world canvas receipt to the headset presentation owner.
         * @param instance Exact active viewport source.
         * @param frameNumber Exact Runtime extraction frame.
         * @param plane The same world geometry used by the encoded eye draws.
         * @return Matching interaction receipt or a typed stale/invalid-source error.
         */
        [[nodiscard]] Result<RuntimeUiPaintHeadsetCanvasReceipt> DetachHeadsetCanvas(
            Runtime::Ui::RuntimeUiInstanceId instance, std::uint64_t frameNumber, const Runtime::Ui::UiWorldCanvasProjection &plane) const;
        /** @brief Applies the detached receipt only after the matching headset frame ends.
         * @param instance Exact original canvas owner.
         * @param receipt Receipt returned by DetachHeadsetCanvas.
         * @param presented Whether the native compositor accepted the drawn projection.
         * @return Success for an applied or retired receipt, or the original interaction error.
         */
        [[nodiscard]] Result<void> HeadsetCanvasPresented(Runtime::Ui::RuntimeUiInstanceId instance,
                                                          RuntimeUiPaintHeadsetCanvasReceipt receipt, bool presented) const;
        /** @brief Acquires admitted World hit geometry for the selected scene.
         * @param scene Exact current scene identity.
         * @return Empty until the canvas presents and is eligible, or the current immutable hit snapshot.
         */
        [[nodiscard]] Result<std::optional<RuntimeUiPaintPresentedWorldCanvas>> ReadPresentedWorldCanvas(
            Runtime::SceneRuntimeId scene) const;
        /** @brief Routes admitted World rays through the source's exact presented geometry and shared UI owners.
         * @param scene Exact active scene. @param batch Caller-owned input and ordered default output.
         * @param observer Synchronous gesture observer.
         * @return Applied result, or the original stale, unavailable, capacity or dispatch error.
         * @post An unavailable presentation neutralizes this recognizer's capture.
         */
        [[nodiscard]] Result<Runtime::Ui::UiPointerInteractionResult> PumpWorldPointers(Runtime::SceneRuntimeId scene,
                                                                                        const RuntimeUiPaintWorldPointerBatch &batch,
                                                                                        Runtime::Ui::UiEventHandler &observer) const;
        /** @brief Cancels the context's captured World pointers on focus, session or source loss.
         * @param interaction Exact owning recognizer. @param nextSequence Shared ordered UI event sequence.
         * @return Cleanup result. A retired source still releases the caller's transient leases.
         */
        [[nodiscard]] Result<void> CancelWorldPointers(Runtime::Ui::UiPointerInteraction &interaction, std::uint64_t &nextSequence) const;
        /** @brief Applies a receipt only for UI actually encoded in that same native frame; presentation never retires GPU claims. */
        [[nodiscard]] Result<void> Presented(std::uint64_t frameNumber, bool nativePresentSucceeded) const;
        /** @brief Pins the actual current derived frame for host inspection; its lifetime participates in the existing drain fence. */
        [[nodiscard]] Result<Runtime::Ui::UiAnimationFrameLease> AcquireFrame() const;
        /** @brief Reserves an independently bounded render-only snapshot view for an active source.
         * @param instance Exact source owner. @param view Exact view identity. @param viewport Local geometry and policy.
         * @return Attachment identity or the original admission failure.
         * @details Call at a source safe point. Successful source reload or removal revokes the attachment.
         */
        [[nodiscard]] Result<Runtime::Ui::UiPresentationAttachmentId> AttachSnapshotView(Runtime::Ui::RuntimeUiInstanceId instance,
                                                                                         Runtime::Ui::UiRenderViewId view,
                                                                                         Runtime::Ui::UiAnimationViewport viewport) const;
        /** @brief Replaces the viewport evidence for a render-only attachment at the source safe point.
         * @param instance Exact source. @param attachment Exact admitted view. @param viewport New local evidence.
         * @return Success or the original stale, lifecycle or validation error.
         */
        [[nodiscard]] Result<void> UpdateSnapshotView(Runtime::Ui::RuntimeUiInstanceId instance,
                                                      Runtime::Ui::UiPresentationAttachmentId attachment,
                                                      Runtime::Ui::UiAnimationViewport viewport) const;
        /** @brief Retires one render-only attachment and its pool. Existing snapshot leases remain valid.
         * @param instance Exact source. @param attachment Exact admitted view.
         * @return Success or the original stale or lifecycle error.
         */
        [[nodiscard]] Result<void> DetachSnapshotView(Runtime::Ui::RuntimeUiInstanceId instance,
                                                      Runtime::Ui::UiPresentationAttachmentId attachment) const;
        /** @brief Extracts published layout into an independent preallocated snapshot slot.
         * @param instance Exact source. @param attachment Exact admitted view.
         * @return Immutable render snapshot, or typed stale, unpublished or exhausted storage failure.
         * @details Does not submit a presentation receipt or admit focus, pointer or control input.
         */
        [[nodiscard]] Result<Runtime::Ui::UiRenderSnapshot> ExtractSnapshotView(Runtime::Ui::RuntimeUiInstanceId instance,
                                                                                Runtime::Ui::UiPresentationAttachmentId attachment) const;
        /** @brief Observes the actual receipt-fenced input admission for the composed view. */
        [[nodiscard]] bool InputEligible(Runtime::Ui::UiRenderViewId view) const noexcept;
        /** @brief Observes finite source admission without borrowing a mutable owner. */
        [[nodiscard]] RuntimeUiPaintHostPhase Phase() const noexcept;

    private:
        friend class RuntimeUiPaintHost;
        friend class RuntimeUiPaintSceneTransaction;
        friend class RuntimeUiPaintProjectSources;

        explicit RuntimeUiPaintPort(std::shared_ptr<Detail::RuntimeUiPaintHostState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::RuntimeUiPaintHostState> state_;
    };

    /** @brief Owns one World ray's bounded gestures for the exact current presentation. */
    class RuntimeUiPaintWorldPointer final {
    public:
        /** @brief Converts one admitted source sample into a move, press or release on the presented canvas.
         * @param port Actual paint owner. @param scene Exact active scene. @param target Actual presented canvas.
         * @param ray Current source ray. @param down Current physical button state. @param milliseconds Owner clock.
         * @param observer Routed event observer. @return Shared UI outcome or typed failure.
         * @post A new presentation lineage starts disarmed until the input reaches neutral.
         */
        [[nodiscard]] Result<Runtime::Ui::UiPointerInteractionResult> Update(const RuntimeUiPaintPort &port, Runtime::SceneRuntimeId scene,
                                                                             const RuntimeUiPaintPresentedWorldCanvas &target,
                                                                             const Math::Ray &ray, bool down, std::uint64_t milliseconds,
                                                                             Runtime::Ui::UiEventHandler &observer);

        /** @brief Releases all held state and requires a fresh press before another activation.
         * @param port Actual paint owner. @return Cleanup result.
         */
        [[nodiscard]] Result<void> Cancel(const RuntimeUiPaintPort &port);

    private:
        /** @brief Reuses the current binding or replaces it after cleaning up capture. */
        [[nodiscard]] Result<void> Bind(const RuntimeUiPaintPort &port, const RuntimeUiPaintPresentedWorldCanvas &target);

        std::optional<Runtime::Ui::UiPointerInteraction> interaction_;
        std::array<Runtime::Ui::UiControlDefaultAction, Runtime::Ui::MaximumUiInteractionSamples + 1> defaults_{};
        std::uint64_t sequence_{1};
        std::uint32_t written_{};
        bool armed_{};
        bool down_{};
    };

    /** @brief Fixed participant registered before RuntimeHost startup and before the editor frame consumer. */
    class RuntimeUiPaintHost final : public Runtime::RuntimeLifecycleParticipant {
        class ConstructionKey final {
            friend class RuntimeUiPaintHost;
            ConstructionKey() noexcept = default;

        public:
            ConstructionKey(const ConstructionKey &) noexcept = default;
        };

    public:
        /** @brief Factory-only stable host construction; callers cannot forge the private state/key pair. */
        RuntimeUiPaintHost(ConstructionKey, std::shared_ptr<Detail::RuntimeUiPaintHostState> state) noexcept : state_(std::move(state)) {}

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
        RuntimeUiPaintHost(const RuntimeUiPaintHost &) = delete;
        RuntimeUiPaintHost &operator=(const RuntimeUiPaintHost &) = delete;
        RuntimeUiPaintHost(RuntimeUiPaintHost &&) = delete;
        RuntimeUiPaintHost &operator=(RuntimeUiPaintHost &&) = delete;
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override;
        [[nodiscard]] Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override;
        void Shutdown() noexcept override;

    private:
        std::shared_ptr<Detail::RuntimeUiPaintHostState> state_;
    };

    /** @brief Explicit application ownership transfer; only the participant is registered with RuntimeHost. */
    struct RuntimeUiPaintComposition final {
        std::unique_ptr<RuntimeUiPaintHost> participant;
        RuntimeUiPaintPort port;
    };
}  // namespace Horo::Application::Internal
