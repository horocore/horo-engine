#pragma once

/** @file CameraService.h
 * @brief View-scoped camera ownership and immutable rendered-frame publication.
 */
#include "Horo/Foundation/StableIdentity.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <optional>

namespace Horo::Runtime {
    struct CameraViewContextTag;
    struct CameraClaimTag;
    using CameraViewContextId = Foundation::StableIdentity<CameraViewContextTag>;
    using CameraClaimId = Foundation::StableIdentity<CameraClaimTag>;

    /** @brief Distinct authority domains; editor preview never acquires a game context. */
    enum class CameraViewDomain : std::uint8_t {
        Runtime,
        PlayInEditor,
        EditorPreview,
        Count
    };

    /** @brief Host-admitted one-view transition policy for this camera owner. */
    enum class CameraTransitionSupport : std::uint8_t {
        HardCutOnly,
        SingleViewBlend,
        Count
    };

    /** @brief Owner-visible outcome of the producer's admitted camera transition. */
    enum class CameraTransitionResolution : std::uint8_t {
        Requested,
        HardCutFallback,
        Count
    };

    /** @brief Host-issued complete context fence, immutable for the owner's lifetime. */
    struct CameraViewContext final {
        CameraViewContextId view;
        SceneRuntimeId scene;
        CameraViewDomain domain{CameraViewDomain::Runtime};
        constexpr auto operator<=>(const CameraViewContext &) const noexcept = default;
    };

    /** @brief Copied camera values with no mutable scene or backend pointer. */
    struct CameraProposal final {
        EntityRef camera; /**< Scene camera identity; empty only for an editor authoring camera. */
        Render::RenderCameraView values;
        std::uint64_t discontinuityRevision{}; /**< Producer-owned cut/seek revision; zero denotes ordinary continuous motion. */
        CameraTransitionResolution transitionResolution{
            CameraTransitionResolution::Requested}; /**< Copied admission outcome for history invalidation. */
        std::uint64_t sceneRevision{};              /**< Exact committed RuntimeSceneView structural revision sampled by this proposal. */
    };

    /** @brief Owner-issued generation fence for one bounded cinematic override slot. */
    struct CameraOverrideLease final {
        CameraViewContext context;
        CameraClaimId claim;
        std::uint32_t slot{};
        std::uint32_t generation{};
        constexpr auto operator<=>(const CameraOverrideLease &) const noexcept = default;
    };

    /** @brief Stable claim order; higher priority then lower identity wins. */
    struct CameraOverrideRequest final {
        CameraViewContext context;
        CameraClaimId claim;
        std::int32_t priority{};
    };

    /** @brief One owned decision shared unchanged by every pass of a rendered frame. */
    struct CameraSelectionSnapshot final {
        CameraViewContext context;
        std::uint64_t renderedFrame{};
        std::uint64_t selectionEpoch{};
        CameraProposal selected;
        std::optional<CameraClaimId> overrideOwner;
        bool discontinuity{};
    };

    /**
     * @brief Final camera selector owned by a runtime/PIE session or editor viewport controller.
     * @details Single owner thread only. No jobs, scene borrows or source references are retained.
     * Commit is the cutoff after VariableUpdate and before RenderExtraction. Repeated commits of
     * the same frame and Scene revision return its immutable decision. Later submissions affect the next frame.
     * A Scene mutation rejects re-extraction of an already committed frame with StaleFrame.
     * The owner must outlive every adapter holding a lease and must not move while leases exist.
     */
    class CameraService final {
    public:
        static constexpr std::size_t MaximumOverrides = 32;
        /** @brief Creates one generation-scoped final owner. @param context Complete host fence.
         * @param transitions Host-admitted capability; no frontend compositor is admitted by this one-view owner.
         * @return Owner or typed invalid/unsupported context failure. */
        [[nodiscard]] static Result<CameraService> Create(const CameraViewContext &context,
                                                          CameraTransitionSupport transitions = CameraTransitionSupport::SingleViewBlend);
        CameraService(const CameraService &) = delete;
        CameraService &operator=(const CameraService &) = delete;
        /** @brief Transfers a prepared owner and closes the moved-from owner.
         * @param other Owner with no externally borrowed service address. @pre No adapter borrows other. */
        CameraService(CameraService &&other) noexcept;
        CameraService &operator=(CameraService &&) = delete;

        /** @brief Reserves permission to propose without changing the active camera.
         * @param request Exact context, stable claim and priority.
         * @return Lease or typed duplicate/context/capacity/lifecycle failure. */
        [[nodiscard]] Result<CameraOverrideLease> Acquire(const CameraOverrideRequest &request);
        /** @brief Copies a validated proposal for the next commit cutoff.
         * @param lease Exact live owner-issued lease. @param proposal Complete copied camera facts.
         * @pre Scene facts were resolved against the current owner-safe-point scene, normally with ResolveSceneCamera.
         * @return Success or typed stale/target/lifecycle failure, with no partial replacement. */
        [[nodiscard]] Result<void> Submit(const CameraOverrideLease &lease, const CameraProposal &proposal);
        /** @brief Interpolates compatible camera pose and lens in the final camera owner.
         * @param lease Exact live override lease. @param from Current source in the same scene epoch.
         * @param to Current destination in the same scene epoch, including its discontinuity revision.
         * @param weight Finite destination weight in [0,1].
         * @return Success or typed invalid lease/target, stale scene revision, unsupported or incompatible transition,
         * without partial replacement. */
        [[nodiscard]] Result<void> SubmitBlend(const CameraOverrideLease &lease, const CameraProposal &from, const CameraProposal &to,
                                               float weight);
        /** @brief Retains a live lease while removing its current proposal before first-key/end-policy handoff.
         * @param lease Exact live lease. @return Success or typed stale/lifecycle failure. */
        [[nodiscard]] Result<void> Withdraw(const CameraOverrideLease &lease);
        /** @brief Removes an override at the owner safe point; committed snapshots remain intact.
         * @param lease Exact live lease. @return Success or typed stale lease failure. */
        [[nodiscard]] Result<void> Release(const CameraOverrideLease &lease);
        /** @brief Validates the current Scene fence and publishes exactly one camera for this frame.
         * @param renderedFrame Non-zero monotonically increasing extraction frame.
         * @param scene Current scene at the extraction safe point; no scene borrow is retained.
         * @param base Current gameplay or editor authoring proposal; never a saved pre-playback pointer.
         * @return Immutable owned selection or typed suspended/stale/revision failure. A Scene change after this frame's
         * first commit rejects re-extraction for that frame, preserving its immutable decision. */
        [[nodiscard]] Result<CameraSelectionSnapshot> Commit(std::uint64_t renderedFrame, RuntimeSceneView scene,
                                                             const std::optional<CameraProposal> &base);
        /** @brief Closes admission and releases all overrides; already copied frames remain readable. */
        void Shutdown() noexcept;

        /** @brief Returns the immutable owning context. @return Exact context fence. */
        [[nodiscard]] const CameraViewContext &Context() const noexcept {
            return context_;
        }

        /** @brief Returns host-admitted transition support. @return Immutable one-view capability. */
        [[nodiscard]] CameraTransitionSupport TransitionSupport() const noexcept {
            return transitions_;
        }

    private:
        struct Slot final {
            std::uint32_t generation{1};
            std::optional<CameraOverrideRequest> request;
            std::optional<CameraProposal> proposal;
        };

        explicit CameraService(const CameraViewContext &context, CameraTransitionSupport transitions) noexcept
            : context_(context), transitions_(transitions) {}

        [[nodiscard]] Result<std::size_t> Resolve(const CameraOverrideLease &lease) const;
        [[nodiscard]] bool ValidProposal(const CameraProposal &proposal, bool isOverride) const noexcept;
        [[nodiscard]] const Slot *WinningProposal() const noexcept;
        [[nodiscard]] Result<void> ValidateFrame(std::uint64_t renderedFrame) const;
        [[nodiscard]] bool ChangesSelection(const CameraProposal &proposal, const std::optional<CameraClaimId> &claim) const noexcept;
        [[nodiscard]] bool CanAdvanceEpoch(bool changed) const noexcept;
        [[nodiscard]] bool HasCommittedFrame(std::uint64_t renderedFrame) const noexcept;
        [[nodiscard]] bool EligibleBase(const std::optional<CameraProposal> &base) const noexcept;
        CameraViewContext context_;
        CameraTransitionSupport transitions_;
        std::array<Slot, MaximumOverrides> slots_{};
        std::optional<CameraSelectionSnapshot> committed_;
        bool admissionOpen_{true};
    };

    /** @brief Copies one enabled scene camera's current hierarchy pose and lens.
     * @param scene Current scene borrow, used only during this call. @param camera Exact generation-checked target.
     * @return Complete camera proposal carrying the current structural revision, or typed target/stale/hierarchy failure.
     * No source pointers are retained. */
    [[nodiscard]] Result<CameraProposal> ResolveSceneCamera(RuntimeSceneView scene, EntityRef camera);
}  // namespace Horo::Runtime
