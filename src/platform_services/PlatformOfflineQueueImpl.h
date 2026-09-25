#pragma once

#include "Horo/PlatformServices/PlatformOfflineQueue.h"

#include <algorithm>
#include <memory>

namespace Horo::PlatformServices {
    struct PlatformOfflineQueue::State final {
        struct Receipt final {
            PlatformOfflineIntent intent;
            TimePoint admittedAt;
            TimePoint expiresAt;
            PlatformOfflineIntentState state{PlatformOfflineIntentState::Pending};
            std::optional<TimePoint> terminalAt;
        };

        PlatformOfflineOperationHandle handle;
        PlatformOfflineLaneKey lane;
        PlatformOfflineOperation operation;
        PlatformOfflineOperationState state{PlatformOfflineOperationState::Pending};
        std::vector<Receipt> receipts;
        std::optional<TimePoint> terminalAt;
    };

    namespace detail {
        /** @brief Resolves a generation-fenced operation in a mutable or immutable operation range. */
        template <typename OperationRange>
        [[nodiscard]] auto FindOperation(OperationRange &operations, const PlatformOfflineOperationHandle operation,
                                         const PlatformOfflineQueueGeneration generation) noexcept {
            if (!operation.IsValid() || operation.generation != generation)
                return static_cast<decltype(std::to_address(operations.begin()))>(nullptr);
            const auto found = std::ranges::find_if(operations, [operation](const auto &candidate) {
                return candidate.handle.sequence == operation.sequence;
            });
            return found == operations.end() ? static_cast<decltype(std::to_address(found))>(nullptr) : std::to_address(found);
        }

        /** @brief Resolves the aggregate containing a receipt in a mutable or immutable operation range. */
        template <typename OperationRange>
        [[nodiscard]] auto FindReceipt(OperationRange &operations, const PlatformOfflineIntentId id) noexcept {
            const auto found = std::ranges::find_if(operations, [id](const auto &candidate) {
                return std::ranges::any_of(candidate.receipts, [id](const auto &receipt) {
                    return receipt.intent.id == id;
                });
            });
            return found == operations.end() ? static_cast<decltype(std::to_address(found))>(nullptr) : std::to_address(found);
        }
    }  // namespace detail
}  // namespace Horo::PlatformServices
