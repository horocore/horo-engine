#pragma once

/** @file SaveCommands.h
 * @brief Host-owned nonblocking manual-save, quick-save and slot-load admission.
 */

#include "Horo/Runtime/Save/SaveManagerProjection.h"
#include "Horo/Runtime/Save/SaveOperationArbiter.h"

#include <array>
#include <thread>

namespace Horo::Runtime {
    /** @brief User intent; quick commands resolve an address from the host's reserved quick slot. */
    enum class SaveCommandKind : std::uint8_t {
        ManualSave,
        QuickSave,
        QuickLoad,
        LoadSlot
    };
    /** @brief Current trusted host authority, never supplied by gameplay/script requests. */
    enum class SaveCommandAuthority : std::uint8_t {
        Denied,
        Authorized
    };
    /** @brief Exact host activity used by cooked mode eligibility. */
    enum class SaveCommandRuntimeState : std::uint8_t {
        ActiveGameplay,
        PausedOrMenu,
        Stable,
        SuspendTransition,
        Loading,
        Unavailable
    };
    /** @brief Adapter response to a prompt bound to exact request revisions and target generation. */
    enum class SaveCommandConfirmation : std::uint8_t {
        Unconfirmed,
        Confirmed
    };

    /** @brief Owner-thread facts maintained by the host. Borrowed spans and references outlive the API.
     * Increment runtimeRevision whenever session/authority/eligibility changes. Catalog belongs to binding.
     * Quick slot is the stable product-owned identity required by cooked ReplaceSingle policy.
     */
    struct SaveCommandHostState final {
        SaveNamespaceBindingSnapshot binding;
        SaveSlotIndex catalog;
        std::span<const SaveManagerSlotAssessment> assessments;
        std::optional<SaveGameSlotId> quickSlot;
        SaveCommandAuthority authority{SaveCommandAuthority::Denied};
        SaveCommandRuntimeState runtime{SaveCommandRuntimeState::Unavailable};
        std::uint64_t runtimeRevision{};
        std::uint64_t monotonicMilliseconds{};
    };

    /** @brief Typed gameplay/script intent with no authority, storage provider or UI callback.
     * ManualSave and LoadSlot require slot; quick commands omit it. Existing explicit slots require
     * expectedGeneration; new manual slots omit it. Confirmed retries retain all original revisions.
     */
    struct SaveCommandRequest final {
        SaveCommandKind kind{SaveCommandKind::ManualSave};
        SaveNamespaceAccessRequest access;
        std::uint64_t catalogRevision{};
        std::uint64_t runtimeRevision{};
        std::optional<SaveGameSlotId> slot;
        std::optional<SlotGenerationId> expectedGeneration;
        SaveCommandConfirmation confirmation{SaveCommandConfirmation::Unconfirmed};
    };

    /** @brief Exact resolved preconditions passed to host capture/storage composition.
     * Load compatibility is admission evidence only; archive verification and migration remain mandatory.
     */
    struct SaveCommandTarget final {
        SaveCommandKind kind{SaveCommandKind::ManualSave};
        SaveNamespaceAccessRequest access;
        std::uint64_t catalogRevision{};
        std::uint64_t runtimeRevision{};
        SaveGameSlotId slot;
        std::optional<SlotGenerationId> generation;
    };
    /** @brief ConfirmationRequired returns no operation; adapters own prompt text and interaction. */
    enum class SaveCommandDisposition : std::uint8_t {
        Admitted,
        Coalesced,
        ConfirmationRequired
    };

    /** @brief Admission or revision-bound confirmation target; handles support polling and cancellation. */
    struct SaveCommandSubmission final {
        SaveCommandDisposition disposition{SaveCommandDisposition::Admitted};
        SaveOperationHandle operation;
        SaveCommandTarget target;
    };

    /** @brief Owner-thread command adapter borrowing host facts and the sole session arbiter.
     * At most one nonterminal command is retained. Equivalent repeated input shares its handle;
     * other work returns OperationInProgress rather than accumulating queued requests. No I/O,
     * capture, callbacks or blocking occurs here. Host owns safe points, producers and acknowledgement.
     * Host must revalidate under the slot mutation lease before capture/load/publication, use the
     * returned generation as a CAS precondition, and never dispatch after a failed revalidation.
     */
    class SaveCommands final {
    public:
        /** @brief Composes an owner-thread adapter with immutable cooked policy and trusted host facts.
         * @param policy Cooked policy copied into this adapter.
         * @param host Live owner-thread facts; must outlive this adapter.
         * @param arbiter Sole session operation authority; must outlive this adapter.
         */
        SaveCommands(CookedSaveProjectPolicy policy, const SaveCommandHostState &host, SaveOperationArbiter &arbiter) noexcept;
        SaveCommands(const SaveCommands &) = delete;
        SaveCommands &operator=(const SaveCommands &) = delete;

        /** @brief Validates authority, policy, exact target and confirmation before bounded admission.
         * @param request Typed intent captured from the current host view.
         * @param operation Fresh host OperationStore descriptor with matching Save/Load kind.
         * @return Handle, exact confirmation target, or stable typed rejection; no work starts synchronously.
         * Duplicate input reuses the admitted handle; its cancellation/deadline remain the first request's.
         */
        [[nodiscard]] Result<SaveCommandSubmission> Submit(const SaveCommandRequest &request, SaveOperationDescriptor operation);
        /** @brief Rechecks the retained exact command against current authority, policy and catalog.
         * @param operation Retained nonterminal operation identity.
         * @return Exact dispatch target or typed rejection. Caller fails/cancels stale work through arbiter.
         * Cooldown applies only to new admission; this does not resolve a different quick target.
         */
        [[nodiscard]] Result<SaveCommandTarget> Revalidate(OperationId operation) const;
        /** @brief Closes command admission without cancelling or waiting for host-owned work.
         * @return Success or owner-thread error. Host shutdown settles the shared arbiter separately.
         */
        [[nodiscard]] Result<void> BeginShutdown();

    private:
        /** @brief Checks thread affinity and closed admission. */
        [[nodiscard]] Result<void> ValidateOwner() const;
        /** @brief Checks trusted bounded state and exact namespace/runtime/catalog revisions. */
        [[nodiscard]] Result<void> ValidateContext(const SaveCommandRequest &request) const;
        /** @brief Resolves and validates one explicit or deterministic quick target. */
        [[nodiscard]] Result<SaveCommandTarget> Resolve(const SaveCommandRequest &request) const;
        /** @brief Checks mode enablement and runtime eligibility. */
        [[nodiscard]] Result<void> ValidateMode(SavePolicyMode mode) const;
        /** @brief Validates slot kind, capacity and generation-specific load assessments. */
        [[nodiscard]] Result<void> ValidateTarget(const SaveCommandTarget &target) const;

        CookedSaveProjectPolicy policy_;
        const SaveCommandHostState *host_;
        SaveOperationArbiter *arbiter_;
        std::thread::id owner_;
        std::optional<SaveCommandTarget> pending_;
        SaveOperationHandle operation_;
        std::array<std::optional<std::uint64_t>, SaveProjectPolicy::ModeCount> lastAdmission_{};
        bool closed_{};
    };
}  // namespace Horo::Runtime
