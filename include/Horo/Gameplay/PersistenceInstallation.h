#pragma once

/** @file PersistenceInstallation.h
 * @brief Native generation installation evidence for explicit backend-neutral save composition.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/PersistenceRegistration.h"

#include <atomic>
#include <memory>
#include <thread>

namespace Horo::Gameplay {
    class LoadedGameModule;
}

namespace Horo::Runtime {
    /** @brief Unforgeable installed persistence evidence pinned to one actual native module generation.
     * @details The existing module admission atomic protects native callback admission; only the owning host revokes it.
     *          Receipts retain that generation through capture, rollback and destruction without extending admission.
     */
    class GameplayPersistenceInstallation final {
    public:
        GameplayPersistenceInstallation(const GameplayPersistenceInstallation &) = delete;
        GameplayPersistenceInstallation &operator=(const GameplayPersistenceInstallation &) = delete;
        GameplayPersistenceInstallation(GameplayPersistenceInstallation &&) noexcept = default;
        GameplayPersistenceInstallation &operator=(GameplayPersistenceInstallation &&) noexcept = default;

        /** @brief Checks exact-generation admission and cancellation. @return False after move, reload or shutdown. */
        [[nodiscard]] bool CanUse() const noexcept {
            return adapter_ && admission_ && admission_->load(std::memory_order_acquire) && !cancellation_.IsCancellationRequested();
        }

        /** @brief Checks admission on the acquiring runtime owner thread.
         * @return True only on that owner while the native generation is admitted.
         * @pre The host serializes this owner boundary with LoadedGameModule reload/shutdown; CanUse alone is not a callback lease.
         */
        [[nodiscard]] bool CanAdmit() const noexcept {
            return std::this_thread::get_id() == ownerThread_ && CanUse();
        }

        /** @brief Compares the exact pinned native generation, including across reloads reusing a module ID.
         * @param other Another privately issued installation receipt.
         * @return True only when both unmoved receipts pin the same admission authority; this does not grant admission.
         */
        [[nodiscard]] bool SameGeneration(const GameplayPersistenceInstallation &other) const noexcept {
            return admission_ && other.admission_ && admission_.get() == other.admission_.get();
        }

        /** @brief Returns the frozen declaration produced by this installation. @return Descriptor; requires an unmoved receipt. */
        [[nodiscard]] const GameplayPersistenceDescriptor &Descriptor() const noexcept {
            return *descriptor_;
        }

        /** @brief Retains the exact registered source/native pin for an admitted operation. @return Owned adapter or empty if revoked. */
        [[nodiscard]] std::shared_ptr<const ICanonicalStateAdapter> AcquireAdapter() const noexcept {
            return CanAdmit() ? adapter_ : nullptr;
        }

    private:
        friend class Gameplay::LoadedGameModule;

        GameplayPersistenceInstallation(std::shared_ptr<const GameplayPersistenceDescriptor> descriptor,
                                        std::shared_ptr<const ICanonicalStateAdapter> adapter,
                                        std::shared_ptr<const std::atomic_bool> admission, CancellationToken cancellation) noexcept
            : descriptor_(std::move(descriptor)), adapter_(std::move(adapter)), admission_(std::move(admission)),
              cancellation_(std::move(cancellation)), ownerThread_(std::this_thread::get_id()) {}

        std::shared_ptr<const GameplayPersistenceDescriptor> descriptor_;
        std::shared_ptr<const ICanonicalStateAdapter> adapter_;
        std::shared_ptr<const std::atomic_bool> admission_;
        CancellationToken cancellation_;
        std::thread::id ownerThread_;
    };
}  // namespace Horo::Runtime
