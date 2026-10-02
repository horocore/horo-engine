#pragma once

/** @file GameEventRegistry.h @brief Exact-generation native gameplay event callback registration. */

#include "Horo/Foundation/BorrowedCallbackContext.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Gameplay::Detail {
    struct GenerationLeaseBinding;
}

namespace Horo::Gameplay {
    /** @brief Owner outcome of a cinematic command consumed at a gameplay safe point. */
    enum class GameplayEventOutcome : std::uint8_t {
        Accepted,
        SuppressedByAuthority,
        CapabilityUnavailable,
        InvalidTarget,
        Backpressured,
        HandlerFailed
    };

    /** @brief Durable occurrence evidence and immutable cooked argument bytes borrowed for one callback. */
    struct GameplayEventRequest final {
        std::uint64_t session{};
        std::uint64_t sessionGeneration{};
        std::uint64_t player{};
        std::uint64_t playerGeneration{};
        std::uint64_t track{};
        std::uint64_t trackGeneration{};
        std::uint64_t key{};
        std::uint64_t keyGeneration{};
        std::uint64_t traversal{};
        std::uint64_t committedTick{};
        bool reverse{};
        std::span<const std::byte> payload;
    };

    /** @brief Inert native contribution; the host pins the module before retaining its callback. */
    struct GameplayEventRegistration final {
        std::uint64_t binding{};
        std::uint64_t bindingVersion{};
        std::uint64_t schema{};
        std::uint64_t schemaVersion{};
        BorrowedCallbackContext context;
        GameplayEventOutcome (*callback)(const BorrowedCallbackContext &, const GameplayEventRequest &){};
    };

    /** @brief Callable lease retaining the native code image and fencing module cancellation. */
    class GameplayEventLease final {
        struct ConstructionKey {
        private:
            friend class GameEventRegistry;
            ConstructionKey() = default;
        };

    public:
        /** @brief Registry-only construction for an already admitted generation; callers cannot create the private key. */
        GameplayEventLease(ConstructionKey, std::shared_ptr<void> owner, const GameplayEventRegistration &registration,
                           CancellationToken cancellation, const std::atomic_bool *admission);
        /** @brief Invokes the pinned module callback at its owner safe point. @param request Borrowed occurrence and payload.
         * @return Typed owner result; exceptions become HandlerFailed and cancellation becomes CapabilityUnavailable. */
        [[nodiscard]] GameplayEventOutcome Invoke(const GameplayEventRequest &request) const noexcept;

    private:
        friend class GameEventRegistry;
        std::shared_ptr<void> owner_;
        GameplayEventRegistration registration_;
        CancellationToken cancellation_;
        const std::atomic_bool *admission_{}; /**< Points into the pinned owner generation; never outlives owner_. */
    };

    /** @brief Open module registration transaction, frozen before startup and leased by application composition. */
    class GameEventRegistry final {
    public:
        /** @brief Copies one inert exact-generation declaration. @param registration Native callback and versioned identities.
         * @return Success or typed duplicate, capacity, or malformed-registration failure. */
        [[nodiscard]] Result<void> Register(const GameplayEventRegistration &registration);
        /** @brief Closes registration before module startup. */
        void Freeze() noexcept;
        /** @brief Acquires an exact binding/schema callback while native admission is open.
         * @param binding Stable event identity. @param bindingVersion Binding contract revision.
         * @param schema Payload schema identity. @param schemaVersion Payload schema revision.
         * @param cancellation Native module cancellation token.
         * @return Owned callable lease or a typed unavailable-generation error. */
        [[nodiscard]] Result<std::shared_ptr<GameplayEventLease>> Acquire(std::uint64_t binding, std::uint64_t bindingVersion,
                                                                          std::uint64_t schema, std::uint64_t schemaVersion,
                                                                          CancellationToken cancellation) const;

    private:
        friend struct Detail::GenerationLeaseBinding;
        std::vector<GameplayEventRegistration> registrations_;
        std::weak_ptr<void> generationLease_;
        const std::atomic_bool *generationLeaseAdmission_{};
        bool frozen_{};
    };
}  // namespace Horo::Gameplay
