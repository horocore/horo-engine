#pragma once

/** @file SceneQueryBudgetOrder.h @brief Fair fixed-tick order for live Scene query owners. */

#include "Horo/AI/AIIdentity.h"

#include <algorithm>
#include <span>

namespace Horo::Application::Internal {
    /** @brief One ordered occurrence of a query owned by an exact live agent generation. */
    struct SceneQueryOccurrence final {
        AI::AgentHandle owner;
        std::uint64_t order{};
        std::size_t index{};

        auto operator<=>(const SceneQueryOccurrence &) const noexcept = default;
    };

    /** @brief Rotates after the last consumer and records the first consumer of the current budget. */
    template <typename Row, typename Key, typename Projection, typename Visitor>
    Key AdvanceSceneQueryBudgetOrder(std::span<const Row> rows, Key lastConsumer, Projection key, Visitor visit) {
        const auto start = std::ranges::upper_bound(rows, lastConsumer, {}, key);
        Key next = lastConsumer;
        bool spent = false;
        for (const auto part : {std::span{start, rows.end()}, std::span{rows.begin(), start}}) {
            for (const auto &row : part) {
                if (visit(row) && !spent) {
                    spent = true;
                    next = key(row);
                }
            }
        }
        return next;
    }

    /** @brief Visits each exact agent as one budget group, including groups whose queries are waiting. */
    template <typename Visitor>
    AI::AgentHandle AdvanceSceneQueryGroups(std::span<const SceneQueryOccurrence> rows, AI::AgentHandle lastConsumer, Visitor visit) {
        const auto start = std::ranges::upper_bound(rows, lastConsumer, {}, &SceneQueryOccurrence::owner);
        auto next = lastConsumer;
        bool spent = false;
        for (auto part : {std::span{start, rows.end()}, std::span{rows.begin(), start}}) {
            while (!part.empty()) {
                const auto owner = part.front().owner;
                const auto end = std::ranges::find_if(part, [owner](const auto &row) {
                    return row.owner != owner;
                });
                const auto group = std::span{part.begin(), end};
                if (visit(owner, group) && !spent) {
                    spent = true;
                    next = owner;
                }
                part = std::span{end, part.end()};
            }
        }
        return next;
    }
}  // namespace Horo::Application::Internal
