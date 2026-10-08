#pragma once

/** @file UiTheme.h
 * @brief Canvas-owned runtime theme preparation and atomic style/layout/render publication.
 */

#include "Horo/Runtime/Ui/UiRenderSnapshot.h"
#include "Horo/Runtime/Ui/UiStyle.h"

namespace Horo::Runtime::Ui {
    /** @brief Exact canvas incarnation and non-reusable generation of a theme change request. */
    struct UiThemeChangeId final {
        UiCanvasInstanceId canvas;
        RuntimeStyleGeneration generation;
        [[nodiscard]] auto operator<=>(const UiThemeChangeId &) const noexcept = default;
    };

    /** @brief Explicit literal substituted only for a removed token from the active registry. */
    struct UiThemeTokenFallback final {
        UiStyleTokenReference token;
        UiStyleValue value;
    };

    /** @brief Closed surface-component bindings into the registered property vocabulary; invalid IDs leave authored values intact. */
    struct UiThemeSurfaceProperties final {
        UiStylePropertyId width;
        UiStylePropertyId height;
        UiStylePropertyId padding;
        UiStylePropertyId fill;
        UiStylePropertyId opacity;
    };

    /** @brief Immutable component inputs borrowed only during one update, in retained-tree preorder. */
    struct UiThemeSurfaceInput final {
        UiStyleElementInput style; /**< An absent asset selects the theme; class IDs with absent assets use the selected theme. */
        UiLayoutStyle layout;
        UiLayoutIntrinsicSource intrinsic;
        UiLinearColor fill;
        float opacity{1.0F};
    };

    /** @brief Load-time capacities and exact ownership of one surface-component canvas. */
    struct UiThemeRuntimeDescriptor final {
        UiStyleResolverDescriptor style;
        UiLayoutEngineDescriptor layout;
        UiRenderExtractorDescriptor render;
        UiThemeSurfaceProperties properties;
        std::uint32_t classReferenceCapacity{}; /**< Total class-list references per frame, bounded by MaximumUiStyleClasses. */
    };

    /** @brief Complete synchronous frame request; no source files or gameplay objects are consulted. */
    struct UiThemeUpdateRequest final {
        UiStyleSourceRevisions styleSources; /**< Registry is supplied by the theme owner; all other revisions are required. */
        UiLayoutUpdateRequest layout;        /**< Evaluator is supplied by the theme owner, using the component descriptors. */
        std::span<const UiThemeSurfaceInput> surfaces;
        const UiLayoutIntrinsicProvider *intrinsic{};
        std::optional<UiThemeChangeId> change; /**< Exact ready request to publish; absent means update the active theme. */
    };

    /** @brief One coherent last-good theme generation; all three immutable leases survive cancellation and owner shutdown. */
    struct UiThemeSnapshot final {
        RuntimeStyleAssetId theme;
        std::uint32_t fallbackTokens{}; /**< Explicitly restored removed tokens, observable for diagnostics. */
        UiComputedStyleSnapshot style;
        UiLayoutSnapshot layout;
        UiRenderSnapshot render;
    };

    /** @brief Explicit admission state of a canvas theme owner. */
    enum class UiThemeRuntimeState : std::uint8_t {
        Active,
        Retiring,
        Stopped
    };

    /**
     * @brief Owns runtime theme changes and the real surface style, layout, and render consumers.
     * @details All methods are serialized on the canvas owner thread. BeginChange/Prepare are bounded safe-point preparation,
     *          may allocate, and perform no I/O. Update uses only preallocated storage and publishes all three snapshots together.
     *          Failed preparation/update retains Current and the active theme. No callbacks, tasks, or borrowed spans survive a call.
     *          Asset loading is host-owned: deliver its result with the exact request ID; cancellation rejects late completion.
     */
    class UiThemeRuntime final {
    public:
        /** @brief Creates the canvas owner and reserves every frame table.
         * @param descriptor Exact matching style/layout identities, view and capacities.
         * @param registry Validated initial registry transferred to this owner.
         * @param theme Initial style asset to select.
         * @return Active owner or typed identity/schema/capacity failure.
         */
        [[nodiscard]] static Result<UiThemeRuntime> Create(const UiThemeRuntimeDescriptor &descriptor, RuntimeStyleRegistry registry,
                                                           RuntimeStyleAssetId theme);
        /** @brief Closes admission and releases mutable ownership, preserving external leases. */
        ~UiThemeRuntime();
        /** @brief Transfers one canvas owner. @param other Owner to transfer. */
        UiThemeRuntime(UiThemeRuntime &&other) noexcept;
        /** @brief Replaces this owner by transfer. @param other Owner to transfer. @return This owner. */
        UiThemeRuntime &operator=(UiThemeRuntime &&other) noexcept;
        UiThemeRuntime(const UiThemeRuntime &) = delete;
        UiThemeRuntime &operator=(const UiThemeRuntime &) = delete;
        /** @brief Reserves a never-reused change ID, cancelling the prior pending change.
         * @return Exact request ID or lifecycle/generation-exhaustion failure.
         */
        [[nodiscard]] Result<UiThemeChangeId> BeginChange();
        /** @brief Validates a complete replacement and explicit removed-token fallbacks without publishing it.
         * @param change Exact pending request.
         * @param definition Owned replacement registry; required invalid data rejects the complete candidate.
         * @param theme Asset selected on successful publication.
         * @param fallbacks Exact-category literals for removed active tokens; missing required references otherwise fail.
         * @return Success or typed stale/schema/reference failure; a failed preparation may be retried with the same ID.
         */
        [[nodiscard]] Result<void> Prepare(UiThemeChangeId change, UiStyleRegistryDefinition definition, RuntimeStyleAssetId theme,
                                           std::span<const UiThemeTokenFallback> fallbacks = {});
        /** @brief Cancels the exact pending change and releases its candidate.
         * @param change Exact request to cancel.
         * @return Success or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> Cancel(UiThemeChangeId change);
        /** @brief Resolves component styles, measures/arranges and extracts paints before one atomic publication.
         * @param tree Exact active retained tree; gameplay/document ownership is never rebuilt.
         * @param request Complete revision-frozen surface inputs and optional ready change.
         * @return Coherent snapshot leases or original typed stage failure. Failure leaves Current and active registry intact.
         */
        [[nodiscard]] Result<UiThemeSnapshot> Update(const UiElementTree &tree, const UiThemeUpdateRequest &request);
        /** @brief Borrows the last-good publication until the next successful update/shutdown. @return Null before first publication. */
        [[nodiscard]] const UiThemeSnapshot *Current() const noexcept;
        /** @brief Cancels preparation and closes update admission. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently cancels work and releases mutable state and internal snapshot leases. */
        void Shutdown() noexcept;
        /** @brief Returns explicit admission state. @return Active, Retiring or Stopped. */
        [[nodiscard]] UiThemeRuntimeState State() const noexcept;
        /** @brief Checks shutdown and outstanding style/layout/render leases. @return True when stopped and all external publications
         * retired. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct Storage;
        explicit UiThemeRuntime(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
