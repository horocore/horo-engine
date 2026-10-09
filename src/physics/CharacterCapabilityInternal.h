#pragma once

#include "Horo/Physics/CharacterCapability.h"

#include <array>
#include <thread>

namespace Horo::Character {
    /** @brief Owner-thread-only world borrow; cancellation is the sole cross-thread mutation. */
    struct CharacterCapabilityState final {
        CharacterCapabilityState(CharacterWorld &owner, CharacterCapabilityIdentity issued, std::thread::id thread,
                                 const CancellationToken &parent)
            : world(&owner), identity(issued), ownerThread(thread), revocation(parent) {}

        CharacterWorld *world;
        const CharacterCapabilityIdentity identity;
        const std::thread::id ownerThread;
        CancellationSource revocation;
    };

    namespace Detail {
        /** @brief Fixed grant slots with weak client ownership; never keeps the world or a consumer alive. */
        class CharacterCapabilityRegistry final {
        public:
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
