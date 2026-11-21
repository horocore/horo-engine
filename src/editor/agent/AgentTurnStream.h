#pragma once

#include "Horo/Agent/ModelProvider.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace Horo::Agent::Detail {
    /** @brief Finite limits for one inference response, derived from the enclosing turn budget. */
    struct TurnStreamBudget final {
        std::size_t events{};
        std::size_t bytes{};
        std::size_t calls{};
    };

    /** @brief Why a response cannot be committed to the turn history. */
    enum class TurnStreamFailure {
        Malformed,
        Capacity
    };

    /** @brief Collects bounded model output until an exact terminal event.
     * @details Tool requests stay inert while the provider callback runs. The owner validates and executes them separately.
     */
    class AgentTurnStream final {
    public:
        /** @brief Creates one draft assistant message with the remaining turn budget. */
        explicit AgentTurnStream(const TurnStreamBudget budget) noexcept : remaining_(budget) {}

        /** @brief Accepts one ordered typed event. False asks the provider to stop the stream. */
        [[nodiscard]] bool Accept(const ModelEvent &event) {
            if (failure_ || complete_)
                return Reject(TurnStreamFailure::Malformed);
            if (remaining_.events == 0)
                return Reject(TurnStreamFailure::Capacity);
            --remaining_.events;
            switch (event.kind) {
                case ModelEventKind::TextDelta:
                    if (!ConsumeBytes(event.text.size()))
                        return false;
                    message_.text.append(event.text);
                    return true;
                case ModelEventKind::ToolIntent:
                    return AcceptIntent(event.tool);
                case ModelEventKind::Usage:
                    if (usage_)
                        return Reject(TurnStreamFailure::Malformed);
                    usage_ = event.usage;
                    return true;
                case ModelEventKind::Completed:
                    complete_ = true;
                    return true;
            }
            return Reject(TurnStreamFailure::Malformed);
        }

        /** @brief Returns the first failure, or malformed if the provider omitted its terminal event. */
        [[nodiscard]] std::optional<TurnStreamFailure> Failure() const noexcept {
            if (failure_)
                return failure_;
            return complete_ ? std::nullopt : std::optional{TurnStreamFailure::Malformed};
        }

        /** @brief Reports an explicitly rejected event even if the provider reports cancellation in response. */
        [[nodiscard]] std::optional<TurnStreamFailure> Rejection() const noexcept {
            return failure_;
        }

        /** @brief Reports provider-declared usage, which may be absent. */
        [[nodiscard]] const std::optional<ModelUsage> &Usage() const noexcept {
            return usage_;
        }

        /** @brief Returns the remaining byte, event and intent allowance. */
        [[nodiscard]] TurnStreamBudget Remaining() const noexcept {
            return remaining_;
        }

        /** @brief Transfers the completed draft to the owner for validation and execution. */
        [[nodiscard]] ModelMessage TakeMessage() && {
            return std::move(message_);
        }

    private:
        /** @brief Retains the first reason and stops accepting events. */
        bool Reject(const TurnStreamFailure failure) noexcept {
            if (!failure_)
                failure_ = failure;
            return false;
        }

        /** @brief Counts bytes without overflowing the turn allowance. */
        bool ConsumeBytes(const std::size_t count) noexcept {
            if (count > remaining_.bytes)
                return Reject(TurnStreamFailure::Capacity);
            remaining_.bytes -= count;
            return true;
        }

        /** @brief Validates message/call correlation before retaining a bounded intent. */
        bool AcceptIntent(const ModelToolIntent &intent) {
            if (intent.callId.empty() || intent.callId.size() > 256 || intent.name.empty() || intent.name.size() > 256 ||
                intent.argumentsJson.empty())
                return Reject(TurnStreamFailure::Malformed);
            if (std::ranges::any_of(message_.toolIntents, [&](const ModelToolIntent &previous) {
                return previous.callId == intent.callId;
            }))
                return Reject(TurnStreamFailure::Malformed);
            if (remaining_.calls == 0)
                return Reject(TurnStreamFailure::Capacity);
            if (!ConsumeBytes(intent.callId.size()) || !ConsumeBytes(intent.name.size()) || !ConsumeBytes(intent.argumentsJson.size()))
                return false;
            --remaining_.calls;
            message_.toolIntents.push_back(intent);
            return true;
        }

        TurnStreamBudget remaining_;
        ModelMessage message_{.role = ModelRole::Assistant};
        std::optional<ModelUsage> usage_;
        std::optional<TurnStreamFailure> failure_;
        bool complete_{};
    };
}  // namespace Horo::Agent::Detail
