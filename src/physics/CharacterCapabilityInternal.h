#pragma once

#include "Horo/Physics/CharacterCapability.h"

#include <array>
#include <thread>

namespace Horo::Character {
    /** @brief Owner-thread-only world borrow; cancellation is the sole cross-thread mutation. */
    struct CharacterCapabilityState final {
        using ChildIssuer = Result<CharacterCapability> (*)(CharacterWorld &, const CancellationToken &);

        CharacterCapabilityState(CharacterWorld &owner, const CharacterCapabilityIdentity &issued, std::thread::id thread,
                                 const CancellationToken &parent, ChildIssuer issuer)
            : world(&owner), identity(issued), ownerThread(thread), revocation(parent), issueChild(issuer) {}

        CharacterWorld *world;
        const CharacterCapabilityIdentity identity;
        const std::thread::id ownerThread;
        CancellationSource revocation;
        std::uint8_t derivationDepth{}; /**< Owner-thread bound on parent cancellation traversal. */
        const ChildIssuer issueChild;   /**< Bound only by the host registry, never supplied by a consumer. */
    };

    namespace Detail {
        /** @brief Fixed grant slots with weak client ownership; never keeps the world or a consumer alive. */
        class CharacterCapabilityRegistry final {
        public:
            CharacterCapabilityRegistry() = default;
            // The registry alone retires these world borrows; client copies share states, never this guard.
            CharacterCapabilityRegistry(const CharacterCapabilityRegistry &) = delete;
            CharacterCapabilityRegistry &operator=(const CharacterCapabilityRegistry &) = delete;
            CharacterCapabilityRegistry(CharacterCapabilityRegistry &&) = delete;
            CharacterCapabilityRegistry &operator=(CharacterCapabilityRegistry &&) = delete;

            ~CharacterCapabilityRegistry() {
                Retire();
            }

            /** @brief Allocates one bounded grant transactionally; failures consume neither a slot nor a generation. */
            [[nodiscard]] Result<std::shared_ptr<CharacterCapabilityState>> Issue(CharacterWorld &world, std::thread::id ownerThread,
                                                                                  const CancellationToken &parent);
            /** @brief Nulls owner borrows before world storage release; owner-thread-only and idempotent. */
            void Retire() noexcept;

        private:
            std::array<std::weak_ptr<CharacterCapabilityState>, MaximumCharacterCapabilitiesPerWorld> clients_;
            std::uint64_t nextGeneration_{1};
        };
    }  // namespace Detail
}  // namespace Horo::Character
