#pragma once

/**
 * @file EditorSurfaceRegistry.h
 * @brief Host-owned registration and lifecycle state for external editor panels and tabs.
 */

#include "Horo/Extensions/EditorSurfaceContext.h"
#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionRetirement.h"
#include "Horo/Foundation/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    struct EditorSurfaceRegistryState;
    struct EditorSurfaceState;

    /** @brief Persistent provider identity without activation evidence. */
    struct EditorSurfaceProviderKey final {
        std::string extensionId;
        std::string moduleId;

        bool operator==(const EditorSurfaceProviderKey &) const noexcept = default;
    };

    /** @brief Host availability of a provider-owned editor surface. */
    enum class EditorSurfaceProviderStatus : std::uint8_t {
        Active,
        Disabled,
        Missing,
    };

    /** @brief Durable user placement of an activity; provider declarations remain immutable. */
    struct EditorActivityPlacement final {
        EditorActivitySide side{EditorActivitySide::Left};
        std::uint8_t group{};
        std::int32_t order{}; /**< Nonnegative insertion ordinal for a user override. */
        bool operator==(const EditorActivityPlacement &) const noexcept = default;
    };

    /** @brief Bounded presentation state persisted for one surface contribution. */
    struct EditorSurfaceWorkspaceEntry final {
        std::string surfaceId;
        EditorSurfaceProviderKey provider;
        bool open{};
        bool focused{};
        std::vector<std::uint8_t> opaqueState;
        bool activityVisible{true}; /**< Host presentation preference; provider badges are never persisted. */
        std::optional<EditorActivityPlacement> activityPlacement;
    };

    /** @brief Versioned workspace presentation state owned by the editor host. */
    struct EditorSurfaceWorkspaceState final {
        std::uint32_t schemaVersion{2};
        std::vector<EditorSurfaceWorkspaceEntry> surfaces;
    };

    /**
     * @brief Finite bounds enforced before external surface state enters the registry.
     * @details maximumWorkspaceEntries bounds registered plus unresolved persistent surfaces across all scopes, reserving a slot so
     * teardown can preserve each registration without allocating.
     */
    struct EditorSurfaceRegistryLimits final {
        std::size_t maximumSurfaces{256};
        std::size_t maximumWorkspaceEntries{512};
        std::size_t maximumOpaqueStateBytes{8192};
        std::size_t maximumTotalStateBytes{1024U * 1024U};
    };

    /** @brief Observable result of an idempotent open, focus, or close request. */
    enum class EditorSurfaceOperationKind : std::uint8_t {
        Opened,
        Focused,
        Closed,
        AlreadyOpen,
        AlreadyFocused,
        AlreadyClosed,
    };

    /** @brief Typed surface operation result; no layout or provider callback is performed here. */
    struct EditorSurfaceOperation final {
        EditorSurfaceOperationKind kind{EditorSurfaceOperationKind::AlreadyClosed};

        /** @brief Returns whether the request changed the host-owned state. */
        [[nodiscard]] bool Changed() const noexcept {
            using enum EditorSurfaceOperationKind;
            return kind == Opened || kind == Focused || kind == Closed;
        }
    };

    /** @brief Restore result identifying state that remains deferred until a provider returns. */
    struct EditorSurfaceRestoreReport final {
        std::vector<std::string> restored;
        std::vector<std::string> disabledProvider;
        std::vector<std::string> missingProvider;
    };

    /** @brief Copied host snapshot of one registered external surface. */
    struct EditorSurfaceSnapshot final {
        EditorSurfaceDescriptor descriptor;
        EditorSurfaceProviderStatus providerStatus{EditorSurfaceProviderStatus::Missing};
        bool open{};
        bool focused{};
        std::vector<std::uint8_t> opaqueState;
        EditorActivityPresentation activity;
        std::optional<EditorUiForm> form; /**< Validated copied panel content, without provider callbacks. */
    };

    /**
     * @brief Move-only lifetime registration for one exact external surface contribution.
     *
     * Reset withdraws the contribution from future host admission while preserving its
     * bounded presentation state as an unresolved entry for a later provider activation.
     */
    class EditorSurfaceRegistration final {
    public:
        ~EditorSurfaceRegistration() noexcept;
        EditorSurfaceRegistration(const EditorSurfaceRegistration &) = delete;
        EditorSurfaceRegistration &operator=(const EditorSurfaceRegistration &) = delete;
        EditorSurfaceRegistration(EditorSurfaceRegistration &&other) noexcept;
        EditorSurfaceRegistration &operator=(EditorSurfaceRegistration &&other) noexcept;

        /** @brief Withdraws this exact contribution and preserves its bounded presentation state. */
        void Reset() const noexcept;
        /**
         * @brief Binds this real surface publication to its verified module retirement owner.
         * @param retirement Verified owning module's retirement controller.
         * @return True when revocation is registered; false for invalid, retired or already bound ownership.
         * @details Failure before work admission leaves the registration unchanged. Failure after creating
         * the revocation adapter withdraws publication through the adapter's RAII cleanup.
         * @throws std::bad_alloc Allocation failure; any constructed revocation adapter still withdraws publication.
         */
        [[nodiscard]] bool AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement);
        /** @brief Reports whether this contribution remains published in the registry. */
        [[nodiscard]] bool IsRegistered() const noexcept;
        /** @brief Returns the stable contribution identity, or an empty view after move. */
        [[nodiscard]] std::string_view Id() const noexcept;
        /**
         * @brief Returns the copied descriptor owned by this registration.
         * @pre The registration has not been moved from.
         */
        [[nodiscard]] const EditorSurfaceDescriptor &Descriptor() const noexcept;

    private:
        friend class EditorSurfaceRegistry;

        EditorSurfaceRegistration(std::weak_ptr<EditorSurfaceRegistryState> registry, std::shared_ptr<EditorSurfaceState> surface) noexcept;

        mutable std::weak_ptr<EditorSurfaceRegistryState> registry_;
        mutable std::shared_ptr<EditorSurfaceState> surface_;
        mutable std::shared_ptr<ExtensionExecutableLease> retirementLease_;
    };

    /**
     * @brief Transactional host registry for persistent external panels and document tabs.
     *
     * The registry owns copied metadata and lifecycle state only. It never invokes module
     * code, exposes layout internals, or treats persisted state as activation evidence.
     */
    class EditorSurfaceRegistry final {
    public:
        static constexpr std::uint32_t WorkspaceSchemaVersion = 2;

        explicit EditorSurfaceRegistry(const EditorSurfaceRegistryLimits &limits = {});
        ~EditorSurfaceRegistry() noexcept;
        EditorSurfaceRegistry(const EditorSurfaceRegistry &) = delete;
        EditorSurfaceRegistry &operator=(const EditorSurfaceRegistry &) = delete;
        EditorSurfaceRegistry(EditorSurfaceRegistry &&) noexcept = delete;
        EditorSurfaceRegistry &operator=(EditorSurfaceRegistry &&) noexcept = delete;

        /**
         * @brief Publishes one restricted editor context as a persistent panel or tab.
         * @param context Live activation-scoped context returned by EditorSurfaceContextProvider.
         * @return Move-only registration, or a typed validation, duplicate, capacity, or shutdown failure.
         * @note Registration copies no executable callback and never invokes provider code.
         */
        [[nodiscard]] Result<EditorSurfaceRegistration> Register(EditorSurfaceContextRegistration context) const;

        /**
         * @brief Publishes a copied validated form for one active panel generation.
         * @param provider Exact provider activation; stale generations fail closed.
         * @param surfaceId Registered panel identity.
         * @param form Bounded declarative standard-component content.
         * @return Success or a typed validation, ownership, revocation or shutdown error.
         */
        [[nodiscard]] Result<void> PublishForm(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                               EditorUiForm form) const;
        /**
         * @brief Changes bounded activity visibility, admission and badge presentation.
         * @param provider Exact provider activation; stale generations fail closed.
         * @param surfaceId Registered activity identity.
         * @param presentation Copied semantic state; no provider callback runs.
         * @return Success or a typed ownership, revocation or shutdown error.
         */
        [[nodiscard]] Result<void> PublishActivity(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                                   EditorActivityPresentation presentation) const;

        /**
         * @brief Changes the durable host-owned visibility preference independently of provider presentation.
         * @param surfaceId Registered activity identity.
         * @param visible User workspace visibility intent.
         * @return Success or a typed unknown, wrong-kind or shutdown error; hiding closes the paired drawer.
         */
        [[nodiscard]] Result<void> SetActivityVisibility(std::string_view surfaceId, bool visible) const;

        /**
         * @brief Toggles a live activity destination and its drawer, replacing the previous destination on the same side.
         * @param provider Exact provider activation observed by the host projection.
         * @param surfaceId Registered activity identity.
         * @return Opened or Closed, or a typed stale, disabled, missing or shutdown failure without mutation.
         */
        [[nodiscard]] Result<EditorSurfaceOperation> ToggleActivity(const EditorSurfaceProviderIdentity &provider,
                                                                    std::string_view surfaceId) const;

        /**
         * @brief Moves a live activity to a bounded insertion slot, preserving user placement across provider reloads.
         * @param provider Exact observed provider generation.
         * @param surfaceId Registered activity identity.
         * @param destination Side, group and insertion ordinal in the current group (including hidden activities).
         * @return Success or a typed stale, unavailable, wrong-kind or invalid-slot failure without mutation.
         * @details Moving an open destination to an occupied side closes the previous paired destination atomically.
         */
        [[nodiscard]] Result<void> MoveActivity(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                                EditorActivityPlacement destination) const;

        /** @brief Opens and focuses an active registered surface. */
        [[nodiscard]] Result<EditorSurfaceOperation> Open(std::string_view surfaceId) const;
        /** @brief Focuses an already-open active registered surface. */
        [[nodiscard]] Result<EditorSurfaceOperation> Focus(std::string_view surfaceId) const;
        /** @brief Closes a surface while retaining its persistent presentation state. */
        [[nodiscard]] Result<EditorSurfaceOperation> Close(std::string_view surfaceId) const;

        /**
         * @brief Changes provider availability without changing the persisted open intent.
         * @param provider Stable extension/module identity; activation generations are not persisted.
         * @param status New host availability state.
         * @return Success or a typed malformed/shutdown failure.
         */
        [[nodiscard]] Result<void> SetProviderStatus(const EditorSurfaceProviderKey &provider, EditorSurfaceProviderStatus status) const;

        /**
         * @brief Replaces one surface's bounded opaque presentation state.
         * @param surfaceId Registered surface identity.
         * @param state Copied presentation bytes; it carries no trust, capability, or executable state.
         * @return Success or a typed unknown, bound, or shutdown failure.
         */
        [[nodiscard]] Result<void> SetOpaqueState(std::string_view surfaceId, std::span<const std::uint8_t> state) const;

        /**
         * @brief Restores workspace-scoped state atomically and defers unavailable contributions.
         * @param state Versioned copied workspace state.
         * @return Restore report, or a typed invalid-state/shutdown failure with no partial mutation.
         * @note Session and Project surfaces are not changed by workspace restoration.
         */
        [[nodiscard]] Result<EditorSurfaceRestoreReport> Restore(const EditorSurfaceWorkspaceState &state) const;

        /**
         * @brief Saves deterministic copied state for registered and unresolved workspace-scoped contributions.
         * @return Workspace state; Session and Project surfaces are excluded.
         */
        [[nodiscard]] EditorSurfaceWorkspaceState Save() const;
        /** @brief Returns deterministic copied snapshots in contribution-ID order. */
        [[nodiscard]] std::vector<EditorSurfaceSnapshot> Snapshot() const;

        /** @brief Returns the publication revision for owner-lane projection caches; no forms or strings are copied. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

        /** @brief Closes admission and withdraws every registered surface without invoking provider code. */
        void BeginShutdown() const noexcept;
        /** @brief Reports whether the registry has entered terminal shutdown. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        friend class EditorSurfaceRegistration;

        static void Remove(const std::shared_ptr<EditorSurfaceRegistryState> &registry,
                           const std::shared_ptr<EditorSurfaceState> &surface) noexcept;

        std::shared_ptr<EditorSurfaceRegistryState> state_;
    };
}  // namespace Horo::Extensions
