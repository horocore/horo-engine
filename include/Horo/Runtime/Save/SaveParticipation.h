#pragma once

/**
 * @file SaveParticipation.h
 * @brief Versioned generation-scoped gameplay and module participation in Runtime Save.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"
#include "Horo/Runtime/Save/SaveProjectPolicy.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

#include <cstdint>
#include <memory>

namespace Horo::Runtime {
    namespace SaveParticipationDetail {
        struct State;
    }

    /** @brief Current semantic version of the gameplay/module save participation contract. */
    inline constexpr std::uint32_t SaveParticipationApiVersion = 1;

    /** @brief Exact host features exposed to one module generation. */
    struct SaveParticipationCapabilities final {
        std::uint32_t apiVersion{SaveParticipationApiVersion}; /**< Exact semantic contract version. */
        bool captureParticipants{};                            /**< Capture-role registration is admitted. */
        bool restoreParticipants{};                            /**< Restore-role registration is admitted. */
        bool saveRequests{};                                   /**< Typed save requests are admitted. */
        bool loadRequests{};                                   /**< Typed load requests are admitted. */

        [[nodiscard]] constexpr auto operator<=>(const SaveParticipationCapabilities &) const noexcept = default;
    };

    /** @brief Path-free gameplay request targeting one opaque logical slot. */
    struct SaveParticipationSaveRequest final {
        SaveGameSlotId slot;                         /**< Logical slot identity; never a path or display name. */
        SavePolicyMode mode{SavePolicyMode::Manual}; /**< Project-policy mode evaluated by the host. */

        [[nodiscard]] constexpr auto operator<=>(const SaveParticipationSaveRequest &) const noexcept = default;
    };

    /** @brief Path-free gameplay restore request targeting one opaque logical slot. */
    struct SaveParticipationLoadRequest final {
        SaveGameSlotId slot; /**< Logical slot identity; never a path or display name. */

        [[nodiscard]] constexpr auto operator<=>(const SaveParticipationLoadRequest &) const noexcept = default;
    };

    /**
     * @brief Application-owned operation admission seam exposed to gameplay through a guarded client.
     *
     * Implementations resolve the active namespace, project policy, safe point, storage, and operation
     * identity. Gameplay and module code can supply neither a filesystem path nor a storage adapter.
     */
    class ISaveParticipationOperationHost {
    public:
        virtual ~ISaveParticipationOperationHost() = default;

        /** @brief Admits one typed save request. @param request Validated path-free request.
         * @return Pollable operation handle or the host's original typed admission failure.
         */
        [[nodiscard]] virtual Result<SaveOperationHandle> RequestSave(const SaveParticipationSaveRequest &request) = 0;
        /** @brief Admits one typed load request. @param request Validated path-free request.
         * @return Pollable operation handle or the host's original typed admission failure.
         */
        [[nodiscard]] virtual Result<SaveOperationHandle> RequestLoad(const SaveParticipationLoadRequest &request) = 0;
    };

    /** @brief Canonical capture surface shared by native gameplay and scripting adapters. */
    using ISaveParticipationCaptureAdapter = ICanonicalStateAdapter;
    /** @brief Staged no-fail-publication restore surface shared by native gameplay and scripting adapters. */
    using ISaveParticipationRestoreAdapter = IStagedRestoreParticipant;

    /**
     * @brief Copyable non-owning capability for one exact module generation.
     *
     * The host state is weakly referenced so retaining this object across module reload cannot keep
     * callbacks or registry authority alive. All mutating methods are owner-thread operations.
     */
    class SaveParticipationClient final {
    public:
        SaveParticipationClient() = default;

        /** @brief Registers an inert descriptor and pinned capture adapter in the host registry.
         * @param descriptor Stable IDs, schema, roles, bounds, dependencies, and owned records.
         * @param adapter Module-generation lease implementing bounded capture.
         * @return Registry evidence or a typed lifecycle, capability, validation, or allocation failure.
         */
        [[nodiscard]] Result<SaveParticipantRegistration> RegisterParticipant(
            const CanonicalStateParticipantDescriptor &descriptor, std::shared_ptr<const ISaveParticipationCaptureAdapter> adapter) const;
        /** @brief Requests a save without exposing namespace or storage paths. @param request Typed request.
         * @return Pollable operation handle or a typed lifecycle, capability, validation, or host failure.
         */
        [[nodiscard]] Result<SaveOperationHandle> RequestSave(const SaveParticipationSaveRequest &request) const;
        /** @brief Requests a restore without exposing namespace or storage paths. @param request Typed request.
         * @return Pollable operation handle or a typed lifecycle, capability, validation, or host failure.
         */
        [[nodiscard]] Result<SaveOperationHandle> RequestLoad(const SaveParticipationLoadRequest &request) const;
        /** @brief Reports whether this exact generation still admits calls. @return True only while its host is open. */
        [[nodiscard]] bool IsOpen() const noexcept;
        /** @brief Returns the captured module generation. @return Zero only for a default client. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        friend class SaveParticipationHost;
        SaveParticipationClient(std::weak_ptr<SaveParticipationDetail::State> state, std::uint64_t generation) noexcept;

        std::weak_ptr<SaveParticipationDetail::State> state_;
        std::uint64_t generation_{};
    };

    /** @brief Host-owned generation boundary for gameplay/module save participation. */
    class SaveParticipationHost final {
    public:
        /** @brief Closes this generation and unregisters its live participants. */
        ~SaveParticipationHost();
        SaveParticipationHost(SaveParticipationHost &&other) noexcept;
        SaveParticipationHost &operator=(SaveParticipationHost &&other) noexcept;
        SaveParticipationHost(const SaveParticipationHost &) = delete;
        SaveParticipationHost &operator=(const SaveParticipationHost &) = delete;

        /** @brief Creates one generation-scoped facade without registering or invoking module code.
         * @param generation Non-zero exact gameplay/module generation.
         * @param capabilities Versioned host capabilities for this generation.
         * @param registry Application-owned canonical participant registry.
         * @param operations Application-owned path-free operation admission seam.
         * @return Empty host or a typed invalid/allocation failure.
         */
        [[nodiscard]] static Result<SaveParticipationHost> Create(std::uint64_t generation, SaveParticipationCapabilities capabilities,
                                                                  CanonicalStateParticipantRegistry &registry,
                                                                  ISaveParticipationOperationHost &operations);

        /** @brief Creates a weak generation-bound client. @return Invalid default client after host move/close. */
        [[nodiscard]] SaveParticipationClient Client() const noexcept;
        /** @brief Revokes admission and removes this generation's live registry bindings.
         * @return Success, or the first registry lifecycle failure; repeated calls succeed.
         * @post Previously issued registry snapshots continue to pin their exact adapter leases.
         */
        [[nodiscard]] Result<void> Close() noexcept;
        /** @brief Reports whether this generation still admits participation. @return Current state. */
        [[nodiscard]] bool IsOpen() const noexcept;

    private:
        explicit SaveParticipationHost(std::shared_ptr<SaveParticipationDetail::State> state) noexcept;

        std::shared_ptr<SaveParticipationDetail::State> state_;
    };
}  // namespace Horo::Runtime
