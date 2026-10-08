#pragma once

#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "UiAnimationCommittedTicks.h"

#include <atomic>
#include <thread>

namespace Horo::Runtime {
    namespace AnimationRuntimeInternal {
        /** @brief Actual application-issued local command state; copied into inactive frame work before consumption. */
        struct ControlledDomain final {
            Ui::UiAnimationClockId clock;
            Ui::UiDuration pending;
            std::optional<Ui::UiDuration> seek;
            Ui::UiPlaybackRate rate;
            Ui::UiTimeRemainder remainder;
            std::uint64_t revision{1};
            bool available{};
            bool playing{};
        };
    }  // namespace AnimationRuntimeInternal

    struct UiAnimationClockController::Storage final {
        const std::thread::id ownerThread{std::this_thread::get_id()};
        std::atomic<bool> retired{};
        std::array<AnimationRuntimeInternal::ControlledDomain, Ui::UiTimeDomainCount> domains;
        std::uint32_t capacity{};
        std::uint32_t commands{};
        bool frameReserved{};
    };

    struct UiAnimationRuntimeParticipant::Storage final {
        Storage(Ui::UiAnimationOwner owner, RuntimeDispatchSource source, UiAnimationRuntimeConfig config,
                Ui::IntegrationInternal::CommittedTickLedger ledger, std::shared_ptr<UiAnimationClockController::Storage> controls)
            : owner(std::move(owner)), source(std::move(source)), config(std::move(config)), ledger(std::move(ledger)),
              controls(std::move(controls)) {}

        const std::thread::id ownerThread{std::this_thread::get_id()};
        Ui::UiAnimationOwner owner;
        RuntimeDispatchSource source;
        UiAnimationRuntimeConfig config;
        Ui::IntegrationInternal::CommittedTickLedger ledger;
        std::shared_ptr<UiAnimationClockController::Storage> controls;
        std::optional<Ui::UiAnimationOwner::Prepared> prepared;
        std::optional<Ui::IntegrationInternal::CommittedTickLedger::Prepared> consumption;
        std::array<AnimationRuntimeInternal::ControlledDomain, Ui::UiTimeDomainCount> candidateControls;
        Ui::UiAnimationHostSourceId binding;
        RuntimeDispatchFacts candidateFacts;
        Duration consumedPresentation;
        std::uint64_t lastVariableFrame{};
        std::uint64_t lastFixedAttempt{};
        std::uint64_t lastPublishedFrame{};
        bool started{};
        bool stopped{};
    };
}  // namespace Horo::Runtime
