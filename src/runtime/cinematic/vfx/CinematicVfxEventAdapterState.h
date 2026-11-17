#pragma once

/** @file CinematicVfxEventAdapterState.h @brief Private prepared cinematic semantic payloads and retained callback state. */
#include "Horo/Cinematic/VfxEventAdapter.h"

#include <thread>

namespace Horo::Cinematic {
    namespace VfxEventDetail {
        inline constexpr std::size_t MaximumKeys = 2'048;
        inline constexpr std::size_t MaximumPayloadBytes = 64U * 1024U;
        inline constexpr std::size_t MaximumPlanBytes = 32U * 1024U * 1024U;

        /** @brief Decodes the genuine ScriptValue argument array and copies its declared semantic projection cold. */
        [[nodiscard]] Result<Gameplay::GameplayVfxEventPayload> PreparePayload(std::span<const std::byte> bytes,
                                                                               const VfxEventPayloadProjection &projection);
        /** @brief Checks bounded projection metadata before any cooked payload decoding. */
        [[nodiscard]] bool ValidProjection(const VfxEventPayloadProjection &projection) noexcept;
    }  // namespace VfxEventDetail

    struct CinematicVfxEventAdapter::State final {
        struct PreparedKey final {
            const CookedEventKey *source{};  // Immutable plan below pins this exact borrowed storage.
            Gameplay::GameplayVfxEventPayload payload;
        };

        VfxEventBinding binding;
        std::shared_ptr<const CookedEventPlan> plan;
        std::shared_ptr<const Gameplay::GameplayVfxCommittedProducer> producer;
        Gameplay::GameplayVfxLookup semantic;
        std::vector<PreparedKey> keys;
        std::vector<VfxEventDispatchRecord> results;
        std::size_t resultCount{};
        const std::thread::id lane{std::this_thread::get_id()};
        bool revoked{};
        bool busy{};

        /** @brief Validates the complete bounded plan and decodes matching keys without changing a source owner. */
        [[nodiscard]] Result<void> PrepareKeys(const VfxEventPayloadProjection &projection);
        /** @brief Checks exact cooked contract and callback occurrence before allowing any semantic command. */
        [[nodiscard]] const PreparedKey *FindRequest(const EventDispatchRequest &request) const noexcept;
        /** @brief Captures complete correlation before source admission, retaining even a typed source rejection. */
        [[nodiscard]] EventDispatchOutcome Dispatch(const EventDispatchRequest &request) noexcept;
        /** @brief Resolves the exact leased callback state; dispatcher retains this same state through callback retirement. */
        [[nodiscard]] static EventDispatchOutcome Invoke(const BorrowedCallbackContext &context,
                                                         const EventDispatchRequest &request) noexcept;
    };
}  // namespace Horo::Cinematic
