#include "Horo/Packages/PackageLifecycleErrors.h"
#include "Horo/Packages/PackageModuleHost.h"
#include "PackageActivationComposition.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace Horo::Packages {
    struct PackageModuleRetirementQueue::State final {
        explicit State(const std::size_t capacity) : maximum(capacity) {
            pending.reserve(capacity);
            draining.reserve(capacity);
        }

        std::thread::id owner{std::this_thread::get_id()};
        std::size_t maximum;
        std::size_t reserved{};
        std::vector<std::unique_ptr<Detail::PackageComposition>> pending;
        std::vector<std::unique_ptr<Detail::PackageComposition>> draining;
        bool inDrain{};
    };

    /** @copydoc PackageModuleRetirementQueue::PackageModuleRetirementQueue */
    PackageModuleRetirementQueue::PackageModuleRetirementQueue(const std::size_t maximumCompositions) {
        if (maximumCompositions == 0 || maximumCompositions > 64)
            throw std::invalid_argument("Invalid package retirement bound");
        state_ = std::make_shared<State>(maximumCompositions);
    }

    /** @copydoc PackageModuleRetirementQueue::~PackageModuleRetirementQueue */
    PackageModuleRetirementQueue::~PackageModuleRetirementQueue() noexcept {
        try {
            (void)Drain();
        } catch (...) {
        }
        // Violating the explicit owner lifetime precondition cannot safely unload executable work.
        if (!state_->pending.empty() || state_->reserved != 0)
            std::terminate();
    }

    /** @copydoc PackageModuleRetirementQueue::Drain */
    std::size_t PackageModuleRetirementQueue::Drain() {
        if (std::this_thread::get_id() != state_->owner || state_->inDrain)
            return Pending();
        state_->inDrain = true;
        state_->draining.swap(state_->pending);
        for (auto &composition : state_->draining) {
            bool complete = false;
            try {
                complete = composition->Finalize();
            } catch (...) {
            }
            if (complete) {
                composition.reset();
                --state_->reserved;
            } else
                state_->pending.push_back(std::move(composition));
        }
        state_->draining.clear();
        state_->inDrain = false;
        return Pending();
    }

    /** @copydoc PackageModuleRetirementQueue::Close */
    Result<void> PackageModuleRetirementQueue::Close() {
        if (std::this_thread::get_id() != state_->owner || Drain() != 0 || state_->reserved != 0)
            return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle,
                                                   "Retain the package retirement owner until all activations and callbacks drain."));
        return Result<void>::Success();
    }

    /** @copydoc PackageModuleRetirementQueue::Pending */
    std::size_t PackageModuleRetirementQueue::Pending() const noexcept {
        return state_->pending.size() + state_->draining.size();
    }

    /** @copydoc PackageModuleRetirementQueue::Reserve */
    bool PackageModuleRetirementQueue::Reserve() noexcept {
        if (std::this_thread::get_id() != state_->owner || state_->inDrain || state_->reserved >= state_->maximum)
            return false;
        ++state_->reserved;
        return true;
    }

    /** @copydoc PackageModuleRetirementQueue::Retain */
    void PackageModuleRetirementQueue::Retain(std::unique_ptr<Detail::PackageComposition> composition) noexcept {
        composition->Retire();
        state_->pending.push_back(std::move(composition));
    }

    /** @copydoc PackageModuleRetirementQueue::Release */
    void PackageModuleRetirementQueue::Release() noexcept {
        --state_->reserved;
    }
}  // namespace Horo::Packages
