#pragma once

/**
 * @file PersistenceSource.h
 * @brief Runtime-only state callbacks implemented by one exact gameplay module generation.
 */

#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Inactive gameplay state prepared for one no-fail lifecycle publication. */
    class IPreparedPersistenceState {
    public:
        virtual ~IPreparedPersistenceState() = default;
        /** @brief Publishes prepared state without allocation, waiting, callbacks or failure. */
        virtual void Publish() noexcept = 0;
    };

    /**
     * @brief Exact-generation runtime owner of one explicitly durable semantic state record.
     *
     * Capture is called at the aggregate save safe point. Prepare restores only into an inactive
     * candidate. Neither callback receives authoring fields or an archive worker reference.
     * Expected failures use Result. The host boundary also contains arbitrary project exceptions:
     * allocation failures retain their phase-specific error and other exceptions become
     * LifecycleCallbackFailed. Project code is not assumed to throw only std::exception types.
     */
    class IPersistenceSource {
    public:
        virtual ~IPersistenceSource() = default;
        /** @brief Returns bounded runtime-only canonical bytes. @param maximumBytes Remaining host-admitted payload budget.
         * @return Owned bytes or typed failure.
         */
        [[nodiscard]] virtual Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t maximumBytes) const = 0;
        /** @brief Prepares an inactive candidate from validated bytes. @param bytes Borrowed detached bytes.
         * @return Owned no-fail publication candidate or typed failure.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IPreparedPersistenceState>> PrepareRuntimeState(std::span<const std::byte> bytes) = 0;
    };
}  // namespace Horo::Gameplay
