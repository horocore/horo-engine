#pragma once

/**
 * @file EditorCommandRegistry.h
 * @brief Host-owned command, menu, toolbar, and status contribution registry.
 */

#include "Horo/Extensions/EditorSurfaceContext.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Stable identity of a host-routed editor command. */
    struct EditorCommandId final {
        std::string value; /**< Canonical command identity. */

        bool operator==(const EditorCommandId &) const noexcept = default;
    };

    /** @brief Declarative condition used to compute command enablement. */
    enum class EditorCommandPredicateKind : std::uint8_t {
        ProjectOpen,
        SelectionPresent,
        SurfaceOpen,
        CapabilityAvailable,
    };

    /**
     * @brief One bounded enablement condition evaluated by the host.
     * @note ProjectOpen and SelectionPresent use an empty operand. SurfaceOpen
     * and CapabilityAvailable use a canonical surface or capability identity.
     */
    struct EditorCommandPredicate final {
        EditorCommandPredicateKind kind{EditorCommandPredicateKind::ProjectOpen};
        std::string operand;
        bool expected{true}; /**< Whether the condition must evaluate to true. */

        bool operator==(const EditorCommandPredicate &) const noexcept = default;
    };

    /** @brief Stable, localized, and ordered metadata for one command contribution. */
    struct EditorCommandDescriptor final {
        EditorCommandId id;
        std::string labelLocalizationKey;   /**< Localized command label allowlisted by the surface context. */
        std::string tooltipLocalizationKey; /**< Optional localized command tooltip allowlisted by the surface context. */
        std::string shortcut;               /**< Optional host-normalized shortcut identity. */
        std::int32_t order{};               /**< Lower values are presented first. */
        std::int32_t priority{};            /**< Higher values win host admission under pressure. */
        std::vector<EditorCommandPredicate> predicates;
    };

    /** @brief Immutable host state used to evaluate declarative command predicates. */
    struct EditorCommandEvaluationContext final {
        bool projectOpen{};
        bool selectionPresent{};
        std::span<const std::string_view> openSurfaceIds;
        std::span<const ExtensionCapabilityId> availableCapabilities;
    };

    /** @brief Result of evaluating one command against an editor state snapshot. */
    struct EditorCommandState final {
        bool enabled{};
    };

    /**
     * @brief Invocation envelope emitted by the host after enablement succeeds.
     * @note The host routes the stable ID. No extension callback, editor model,
     * renderer handle, or native UI object is stored in this envelope.
     */
    struct EditorCommandInvocation final {
        EditorCommandId id;
        EditorSurfaceProviderIdentity provider;
        EditorSurfaceContext context;
    };

    /** @brief Read-only metadata snapshot consumed by menu, toolbar, status, or command-palette hosts. */
    struct EditorCommandSnapshot final {
        EditorCommandDescriptor command;
        EditorSurfaceDescriptor surface;
        EditorSurfaceProviderIdentity provider;
    };

    /** @brief Host diagnostic category for a rejected command publication. */
    enum class EditorCommandDiagnosticKind : std::uint8_t {
        DuplicateId,
        ShortcutConflict,
    };

    /** @brief Bounded collision evidence retained by the host for diagnostics/UI presentation. */
    struct EditorCommandDiagnostic final {
        EditorCommandDiagnosticKind kind{EditorCommandDiagnosticKind::DuplicateId};
        std::string commandId;
        std::string conflictingCommandId;
        std::string shortcut;
        std::string detail;
    };

    /** @brief Finite host limits for command contributions and retained diagnostics. */
    struct EditorCommandRegistryLimits final {
        std::size_t maximumCommands{256};
        std::size_t maximumPredicates{16};
        std::size_t maximumIdentityBytes{256};
        std::size_t maximumShortcutBytes{64};
        std::size_t maximumLocalizationKeyBytes{128};
        std::size_t maximumDiagnostics{256};
    };

    struct EditorCommandRegistryState;
    struct EditorCommandEntry;

    /** @brief Move-only publication whose lifetime controls one command contribution. */
    class EditorCommandRegistration final {
    public:
        ~EditorCommandRegistration() noexcept;
        EditorCommandRegistration(const EditorCommandRegistration &) = delete;
        EditorCommandRegistration &operator=(const EditorCommandRegistration &) = delete;
        EditorCommandRegistration(EditorCommandRegistration &&other) noexcept;
        EditorCommandRegistration &operator=(EditorCommandRegistration &&other) noexcept;

        /** @brief Revokes the command and its activation-scoped surface context. */
        void Reset() noexcept;

        /** @brief Reports whether the command remains published and its provider context is usable. */
        [[nodiscard]] bool IsRegistered() const noexcept;

        /** @brief Returns the stable command identity, or an empty identity after move/reset. */
        [[nodiscard]] const EditorCommandId &Id() const noexcept;

    private:
        friend class EditorCommandRegistry;
        EditorCommandRegistration(std::weak_ptr<EditorCommandRegistryState> registry, std::shared_ptr<EditorCommandEntry> entry) noexcept;

        std::weak_ptr<EditorCommandRegistryState> registry_;
        std::shared_ptr<EditorCommandEntry> entry_;
    };

    /** @brief Explicit host registry for command-backed editor contributions. */
    class EditorCommandRegistry final {
    public:
        explicit EditorCommandRegistry(const EditorCommandRegistryLimits &limits = {});
        ~EditorCommandRegistry() noexcept;
        EditorCommandRegistry(const EditorCommandRegistry &) = delete;
        EditorCommandRegistry &operator=(const EditorCommandRegistry &) = delete;
        EditorCommandRegistry(EditorCommandRegistry &&) noexcept = default;
        EditorCommandRegistry &operator=(EditorCommandRegistry &&other) noexcept;

        /**
         * @brief Publishes one command through an activation-scoped surface context.
         * @param context Registration issued by the host surface-context provider.
         * @param descriptor Stable command metadata and declarative enablement rules.
         * @return Move-only publication or a typed validation/collision/capacity failure.
         * @note Registration never invokes extension code and consumes the context on failure.
         */
        [[nodiscard]] Result<EditorCommandRegistration> Register(EditorSurfaceContextRegistration context,
                                                                 EditorCommandDescriptor descriptor);

        /**
         * @brief Evaluates one command without invoking extension code.
         * @param id Stable command identity.
         * @param evaluation Immutable host state snapshot for this evaluation.
         * @return Enablement state or a typed unknown/provider/shutdown failure.
         */
        [[nodiscard]] Result<EditorCommandState> Evaluate(std::string_view id, const EditorCommandEvaluationContext &evaluation) const;

        /**
         * @brief Creates an unload-safe invocation envelope when a command is enabled.
         * @param id Stable command identity.
         * @param evaluation Immutable host state snapshot for this invocation.
         * @return Stable ID plus revocable context, or a typed disabled/provider failure.
         */
        [[nodiscard]] Result<EditorCommandInvocation> Invoke(std::string_view id, const EditorCommandEvaluationContext &evaluation) const;

        /**
         * @brief Returns live commands in deterministic order.
         * @return Snapshots ordered by order, descending priority, then stable ID.
         */
        [[nodiscard]] std::vector<EditorCommandSnapshot> Snapshot() const;

        /** @brief Returns retained collision diagnostics in publication order. */
        [[nodiscard]] std::vector<EditorCommandDiagnostic> Diagnostics() const;

        /** @brief Closes publication admission and revokes every command context. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether the registry has entered terminal shutdown. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        std::shared_ptr<EditorCommandRegistryState> state_;
    };
}  // namespace Horo::Extensions
